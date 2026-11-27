// SPDX-License-Identifier: GPL-3.0-or-later
#include "deskengine.h"

#include "chatstream.h"
#include "tradingconfig.h"
#include "tradingengine.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTimer>

#include <algorithm>
#include <cmath>

using namespace Trading::Desk;

namespace {

const int kMaxMessages = 400;      // kept and saved
const int kMaxLength = 4000;       // characters of one message
const int kReadDelayMs = 250;      // messages arriving within this are read by the model together
const int kReadBatch = 10;         // at most this many new messages per model read
const int kTickMs = 700;           // the live price against the levels, at most this often
const int kReadingsMs = 5000;      // indicator readings compared at most this often
const int kRecentTurns = 12;       // earlier messages an answer sees as conversation
const int kMaxLog = 400;

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

QString qs(const std::string &s)
{
    return QString::fromStdString(s);
}

std::string ss(const QString &s)
{
    return s.toStdString();
}

QString price(double v)
{
    return qs(formatPrice(v));
}

QString pct(double v)
{
    return QString::number(std::fabs(v), 'f', std::fabs(v) < 0.1 ? 3 : 2);
}

// The first {...} of a model's answer, wherever the model put it (fences, thinking first).
QJsonObject firstObject(const QString &text)
{
    QString t = text;
    const int think = t.lastIndexOf(QLatin1String("</think>"));
    if (think >= 0)
        t = t.mid(think + 8);
    const int open = t.indexOf(QLatin1Char('{'));
    const int close = t.lastIndexOf(QLatin1Char('}'));
    if (open < 0 || close <= open)
        return {};
    return QJsonDocument::fromJson(t.mid(open, close - open + 1).toUtf8()).object();
}

double number(const QJsonValue &v)
{
    if (v.isDouble())
        return v.toDouble();
    if (v.isString()) {
        QString s = v.toString().trimmed();
        s.remove(QLatin1Char(','));
        s.remove(QLatin1Char('$'));
        bool ok = false;
        const double d = s.toDouble(&ok);
        return ok ? d : 0;
    }
    return 0;
}

QJsonObject entryJson(const Entry &e)
{
    QJsonObject o{{QStringLiteral("topic"), qs(e.topic)}, {QStringLiteral("text"), qs(e.text)},
                  {QStringLiteral("side"), QLatin1String(sideName(e.side))},
                  {QStringLiteral("source"), QLatin1String(sourceName(e.source))},
                  {QStringLiteral("time"), e.time}};
    if (!e.by.empty())
        o.insert(QStringLiteral("by"), qs(e.by));
    if (e.message)
        o.insert(QStringLiteral("message"), e.message);
    return o;
}

Entry entryFrom(const QJsonObject &o)
{
    Entry e;
    e.topic = ss(o.value(QLatin1String("topic")).toString());
    e.text = ss(o.value(QLatin1String("text")).toString());
    e.side = sideFromName(ss(o.value(QLatin1String("side")).toString()));
    e.source = sourceFromName(ss(o.value(QLatin1String("source")).toString()));
    e.by = ss(o.value(QLatin1String("by")).toString());
    e.message = o.value(QLatin1String("message")).toInteger();
    e.time = o.value(QLatin1String("time")).toInteger();
    return e;
}

QVariantMap entryMap(const Entry &e)
{
    return entryJson(e).toVariantMap();
}

bool indicatorTopic(const std::string &topic)
{
    return topic == "rsi" || topic == "macd" || topic == "moving averages" || topic == "bollinger"
        || topic == "volume" || topic == "divergence";
}

QString rsiZone(double rsi)
{
    return rsi >= 70 ? QStringLiteral("overbought") : rsi <= 30 ? QStringLiteral("oversold") : QStringLiteral("neutral");
}

} // namespace

DeskEngine::DeskEngine(TradingConfig *config, TradingEngine *engine, QObject *parent)
    : QObject(parent)
    , m_config(config)
    , m_engine(engine)
    , m_messages(new DeskMessages(this))
    , m_stream(new ChatStream(config, this))
    , m_readTimer(new QTimer(this))
    , m_saveTimer(new QTimer(this))
    , m_tickTimer(new QTimer(this))
{
    m_clock.start();
    // Kept: the destructor saves after the settings object may be gone.
    m_path = m_config->dataDir() + QStringLiteral("/desk.json");
    m_hasKey = !m_config->apiKey().isEmpty();
    m_stream->setLimit(4);
    m_readTimer->setSingleShot(true);
    m_readTimer->setInterval(kReadDelayMs);
    connect(m_readTimer, &QTimer::timeout, this, &DeskEngine::flushRead);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(400);
    connect(m_saveTimer, &QTimer::timeout, this, &DeskEngine::save);
    m_tickTimer->setSingleShot(true);
    m_tickTimer->setInterval(kTickMs);
    connect(m_tickTimer, &QTimer::timeout, this, &DeskEngine::onMarketTick);

    connect(m_stream, &ChatStream::started, this, [this](quint64 id) {
        const auto it = m_tasks.find(id);
        if (it == m_tasks.end())
            return;
        it->stage = QStringLiteral("sent");
        if (it->kind == QLatin1String("answer"))
            m_messages->update(it->message, [](DeskMessages::Message &m) {
                if (m.state == QLatin1String("waiting"))
                    m.state = QStringLiteral("thinking"); // with the model now
            }, {DeskMessages::StateRole});
        logEvent(QStringLiteral("sent"), it->kind == QLatin1String("answer") ? it->message : 0, it->kind);
        taskChanged();
    });
    connect(m_stream, &ChatStream::thinking, this, [this](quint64 id) {
        const auto it = m_tasks.find(id);
        if (it == m_tasks.end() || it->stage == QLatin1String("thinking"))
            return;
        it->stage = QStringLiteral("thinking");
        if (it->kind == QLatin1String("answer"))
            m_messages->update(it->message, [](DeskMessages::Message &m) { m.state = QStringLiteral("thinking"); },
                               {DeskMessages::StateRole});
        taskChanged();
    });
    connect(m_stream, &ChatStream::retrying, this, [this](quint64 id, const QString &why) {
        const auto it = m_tasks.find(id);
        if (it == m_tasks.end())
            return;
        it->stage = QStringLiteral("waiting");
        if (it->kind == QLatin1String("answer"))
            m_messages->update(it->message, [why](DeskMessages::Message &m) { m.detail = why; },
                               {DeskMessages::DetailRole});
        taskChanged();
    });
    connect(m_stream, &ChatStream::delta, this, &DeskEngine::onDelta);
    connect(m_stream, &ChatStream::finished, this, &DeskEngine::onFinished);
    connect(m_stream, &ChatStream::failed, this, &DeskEngine::onFailed);

    // The market side, independent of the conversation.
    const auto tick = [this] {
        if (m_running && !m_tickTimer->isActive())
            m_tickTimer->start();
    };
    connect(m_engine, &TradingEngine::chartChanged, this, tick);
    connect(m_engine, &TradingEngine::marketChanged, this, [this, tick] {
        tick();
        invalidateStatus(); // the feed's state
    });
    connect(m_engine, &TradingEngine::selectionChanged, this, [this] { invalidate(); });
    connect(m_engine, &TradingEngine::signalChanged, this, &DeskEngine::onSignal);
    connect(m_engine, &TradingEngine::accountChanged, this, [this] { invalidate(); });
    connect(m_config, &TradingConfig::changed, this, [this] { Q_EMIT settingsChanged(); invalidateStatus(); });
    connect(m_config, &TradingConfig::keysChanged, this, [this] {
        m_hasKey = !!m_hasKey; // read from disk once per change, not per update
        invalidateStatus();
        scheduleRead(); // messages that waited for a key
    });
}

DeskEngine::~DeskEngine()
{
    // Answers under way are saved as cut off; the next start says so.
    m_stream->blockSignals(true);
    save();
}

void DeskEngine::start()
{
    if (m_running)
        return;
    load();
    m_running = true;
    logEvent(QStringLiteral("start"), 0);
    invalidateStatus();
    invalidate();
    // Messages accepted before the application closed and never read.
    for (const DeskMessages::Message &m : m_messages->all()) {
        if (m.role == QLatin1String("you") && m.state == QLatin1String("accepted"))
            QTimer::singleShot(0, this, [this, id = m.id] { readMessage(id); });
    }
    scheduleRead();
    m_tickTimer->start();
}

// ---- the conversation ------------------------------------------------------------

QVariantMap DeskEngine::post(const QString &raw, const QString &token)
{
    const QString text = raw.trimmed();
    if (text.isEmpty())
        return {{QStringLiteral("accepted"), false}, {QStringLiteral("error"), tr("Nothing to send.")}};
    if (text.size() > kMaxLength)
        return {{QStringLiteral("accepted"), false},
                {QStringLiteral("error"), tr("The message is too long (at most %1 characters).").arg(kMaxLength)}};
    if (!token.isEmpty() && m_tokens.contains(token))
        return {{QStringLiteral("accepted"), true}, {QStringLiteral("id"), double(m_tokens.value(token))},
                {QStringLiteral("duplicate"), true}};

    const qint64 t0 = m_clock.nsecsElapsed();
    DeskMessages::Message m;
    m.id = ++m_nextId;
    m.role = QStringLiteral("you");
    m.text = text;
    m.state = QStringLiteral("accepted");
    m.time = nowMs();
    m_messages->append(m);
    m_messages->trim(kMaxMessages);
    if (!token.isEmpty()) {
        m_tokens.insert(token, m.id);
        m_tokenOrder.append(token);
        while (m_tokenOrder.size() > 64)
            m_tokens.remove(m_tokenOrder.takeFirst());
    }
    scheduleSave();
    // Read on the next turn of the event loop: the caller has its answer first.
    QTimer::singleShot(0, this, [this, id = m.id] { readMessage(id); });
    logEvent(QStringLiteral("accepted"), m.id, QString::number((m_clock.nsecsElapsed() - t0) / 1000) + QStringLiteral("us"));
    Q_EMIT accepted(m.id);
    invalidateStatus();
    return {{QStringLiteral("accepted"), true}, {QStringLiteral("id"), double(m.id)},
            {QStringLiteral("duplicate"), false}};
}

DeskEngine::Resolved DeskEngine::resolve(const Reading &reading) const
{
    Resolved r;
    if (reading.markets.empty())
        return r;
    // The first market the message names.
    const Mention &mention = reading.markets.front();
    const QString base = qs(mention.base), quote = qs(mention.quote);
    r.base = base;
    const QVariantList list = m_engine->watchlist();
    static const QStringList dollars{QStringLiteral("USDT"), QStringLiteral("USD"), QStringLiteral("USDC"),
                                     QStringLiteral("FDUSD"), QStringLiteral("BUSD")};
    const auto matches = [&](const QString &symbol) {
        if (base == QLatin1String("XAU"))
            return symbol.startsWith(QLatin1String("PAXG")) || symbol.startsWith(QLatin1String("XAUT"))
                || symbol.startsWith(QLatin1String("XAU"));
        if (!quote.isEmpty() && symbol == base + quote)
            return true;
        if (!symbol.startsWith(base))
            return false;
        const QString rest = symbol.mid(base.size());
        if (quote.isEmpty() || dollars.contains(quote))
            return dollars.contains(rest);
        return false;
    };
    for (const QVariant &v : list) {
        const QVariantMap row = v.toMap();
        const QString symbol = row.value(QStringLiteral("symbol")).toString();
        if (matches(symbol)) {
            r.symbol = symbol;
            r.name = row.value(QStringLiteral("name")).toString();
            r.listed = true;
            return r;
        }
    }
    // Not on the list: named all the same, without live data.
    const bool forex = m_engine->marketKind() == QLatin1String("forex");
    r.symbol = base == QLatin1String("XAU") ? QStringLiteral("XAUUSD")
             : base + (quote.isEmpty() ? (forex ? QStringLiteral("USD") : QStringLiteral("USDT")) : quote);
    r.name = TradingEngine::displayName(r.symbol);
    return r;
}

bool DeskEngine::livePrice(const QString &symbol, double *value) const
{
    if (symbol.isEmpty())
        return false;
    if (symbol == m_engine->symbol()) {
        const QVariantMap q = m_engine->quote();
        const double close = q.value(QStringLiteral("close")).toDouble();
        if (close > 0) {
            *value = close;
            return true;
        }
    }
    const QVariantList list = m_engine->watchlist();
    for (const QVariant &v : list) {
        const QVariantMap row = v.toMap();
        if (row.value(QStringLiteral("symbol")).toString() == symbol && row.value(QStringLiteral("hasPrice")).toBool()
            && row.value(QStringLiteral("price")).toDouble() > 0) {
            *value = row.value(QStringLiteral("price")).toDouble();
            return true;
        }
    }
    return false;
}

QStringList DeskEngine::describe(const Reading &r) const
{
    QStringList tags;
    for (const LevelNote &l : r.levels)
        tags.append(qs(l.kind) + QLatin1Char(' ') + price(l.price)
                    + (l.relation.empty() || l.relation == "at" ? QString() : QStringLiteral(" · ") + qs(l.relation)));
    for (const Observation &o : r.observations)
        tags.append(qs(o.text));
    if (!r.tradeSide.empty())
        tags.append(r.tradeSide == "long" ? tr("weighing a long") : tr("weighing a short"));
    if (!r.facts.empty()) {
        QStringList facts;
        for (const std::string &f : r.facts)
            facts.append(qs(f));
        tags.append(tr("asks: %1").arg(facts.join(QStringLiteral(", "))));
    } else if (r.question) {
        tags.append(r.tradeDecision ? tr("trade question") : tr("question"));
    }
    return tags;
}

void DeskEngine::readMessage(qint64 id)
{
    const DeskMessages::Message *found = m_messages->find(id);
    if (!found || found->role != QLatin1String("you") || found->state != QLatin1String("accepted"))
        return;
    const QString text = found->text;
    const qint64 now = nowMs();
    Reading reading = read(ss(text));
    std::vector<Change> changes;

    // The market it is about.
    const Resolved market = resolve(reading);
    if (!market.symbol.isEmpty()) {
        const std::vector<Change> c = m_state.focus(ss(market.symbol), ss(market.name), now);
        changes.insert(changes.end(), c.begin(), c.end());
        if (!c.empty())
            m_focusMessage = id;
        if (!market.listed && !m_warnedUnlisted.contains(market.symbol)) {
            m_warnedUnlisted.insert(market.symbol);
            const bool forexLike = market.base == QLatin1String("XAU") || market.base == QLatin1String("EUR")
                || market.base == QLatin1String("GBP") || market.base == QLatin1String("AUD")
                || market.base == QLatin1String("NZD") || market.base == QLatin1String("USD");
            note(QStringLiteral("warning"),
                 forexLike && m_engine->marketKind() != QLatin1String("forex")
                     ? tr("%1 is not on your crypto markets, so there is no live data for it here. Gold and currency "
                          "pairs are under Forex on Live Trading.").arg(market.name)
                     : tr("%1 is not on your markets, so there is no live data for it. Add it with + on Live Trading "
                          "to follow its price.").arg(market.name),
                 market.symbol);
        } else if (market.listed && m_config->deskFollowChart() && market.symbol != m_engine->symbol()) {
            if (m_teamAsked && m_engine->analyzing()) {
                // Switching now would cancel the call the desk asked the team for.
                note(QStringLiteral("info"), tr("The chart stays on %1 until the analysis team's call is in; %2 is "
                                                "followed from its price meanwhile.")
                                                 .arg(m_engine->symbolName(), market.name),
                     market.symbol);
            } else {
                m_engine->setSymbol(market.symbol);
                note(QStringLiteral("info"), tr("Chart switched to %1 to follow the conversation.").arg(market.name),
                     market.symbol);
            }
        }
    }
    const QString current = qs(m_state.current());

    // A number far from the market's price is not a level of it ("above the 200").
    double live = 0;
    const bool haveLive = livePrice(current, &live);
    if (haveLive) {
        std::vector<LevelNote> kept;
        for (const LevelNote &l : reading.levels) {
            if (l.price >= live * 0.5 && l.price <= live * 2.0)
                kept.push_back(l);
        }
        reading.levels = kept;
    }
    const std::vector<Change> applied = m_state.apply(reading, id, now);
    changes.insert(changes.end(), applied.begin(), applied.end());

    m_lastRead = now;
    m_messages->update(id, [&](DeskMessages::Message &m) {
        m.state = QStringLiteral("read");
        m.tags = describe(reading);
        m.market = current;
    });
    logEvent(QStringLiteral("read"), id, QString::number(reading.levels.size()) + QStringLiteral(" levels, ")
                                            + QString::number(reading.observations.size()) + QStringLiteral(" observations"));
    if (!changes.empty())
        touched(QStringLiteral("you"), qs(changes.back().text));

    // The levels it named, against the live price, at once.
    if (!reading.levels.empty() && !current.isEmpty()) {
        if (haveLive) {
            // The same check as the regular one: what it finds is said.
            const std::vector<Change> moves = m_state.priceUpdate(ss(current), live, nearPctFor(current), now);
            QStringList lines;
            const Book *book = m_state.find(ss(current));
            for (const LevelNote &n : reading.levels) {
                if (!book)
                    break;
                for (const Level &l : book->levels) {
                    if (std::fabs(l.price - n.price) > n.price * 0.0005 || !l.hasDistance)
                        continue;
                    lines.append(tr("%1 %2 is %3 % %4 the price.")
                                     .arg(l.kind == "level" ? tr("Level") : qs(l.kind).left(1).toUpper() + qs(l.kind).mid(1))
                                     .arg(price(l.price), pct(l.distancePct),
                                          l.distancePct >= 0 ? tr("below") : tr("above")));
                }
            }
            if (!lines.isEmpty())
                note(QStringLiteral("market"),
                     tr("Live %1: %2. %3").arg(qs(book->name.empty() ? book->market : book->name), price(live),
                                                lines.join(QLatin1Char(' '))),
                     current);
            notes(moves);
        } else {
            note(QStringLiteral("market"), tr("No live price for %1 yet, so the level is noted but not yet compared.")
                                               .arg(current.isEmpty() ? tr("this market") : TradingEngine::displayName(current)),
                 current);
        }
    }

    // What it asks.
    if (!reading.facts.empty()) {
        DeskMessages::Message a;
        a.id = ++m_nextId;
        a.role = QStringLiteral("desk");
        a.kind = QStringLiteral("live");
        a.by = tr("live data");
        a.state = QStringLiteral("done");
        a.replyTo = id;
        a.market = current;
        a.time = nowMs();
        a.text = liveFacts(reading);
        m_messages->append(a);
        m_answered.insert(id);
        logEvent(QStringLiteral("answered-live"), a.id);
        Q_EMIT answerFinished(a.id);
    } else if (reading.question) {
        const QString lowerText = text.toLower();
        const bool deep = reading.tradeDecision || text.size() > 140 || lowerText.contains(QLatin1String("analy"))
            || lowerText.contains(QLatin1String("plan")) || lowerText.contains(QLatin1String("explain"))
            || lowerText.contains(QLatin1String("why")) || lowerText.contains(QLatin1String("compare"))
            || lowerText.contains(QLatin1String("strategy"));
        startAnswer(id, deep, current);
        if (reading.tradeDecision)
            maybeStartTeam(current);
    }

    // The model reads it too, with whatever else arrives meanwhile.
    m_unread.append(id);
    scheduleRead();
    invalidate();
    invalidateStatus();
    scheduleSave();
}

QString DeskEngine::liveFacts(const Reading &r) const
{
    QString market = qs(m_state.current());
    if (market.isEmpty())
        market = m_engine->symbol();
    const QString name = TradingEngine::displayName(market);
    const bool onChart = market == m_engine->symbol();
    const QVariantMap rd = onChart ? m_engine->readings() : QVariantMap();
    const bool ta = rd.value(QStringLiteral("taValid")).toBool();
    QStringList lines;
    for (const std::string &fact : r.facts) {
        if (fact == "price") {
            double p = 0;
            if (livePrice(market, &p))
                lines.append(tr("%1 is at %2 (live).").arg(name, price(p)));
            else
                lines.append(tr("No live price for %1.").arg(name));
        } else if (fact == "rsi" || fact == "macd" || fact == "trend") {
            if (!onChart) {
                lines.append(tr("Indicators are read for the chart's market; %1 is not on the chart.").arg(name));
                continue;
            }
            if (!ta) {
                lines.append(tr("Not enough price history for %1 yet.").arg(name));
                continue;
            }
            if (fact == "rsi")
                lines.append(tr("RSI(14) on %1 %2: %3 (%4).").arg(name, m_engine->interval())
                                 .arg(rd.value(QStringLiteral("rsi14")).toDouble(), 0, 'f', 1)
                                 .arg(rsiZone(rd.value(QStringLiteral("rsi14")).toDouble())));
            else if (fact == "macd")
                lines.append(tr("MACD on %1 %2: histogram %3, %4.").arg(name, m_engine->interval())
                                 .arg(rd.value(QStringLiteral("macdHistogram")).toDouble(), 0, 'g', 4)
                                 .arg(rd.value(QStringLiteral("macdState")).toString()));
            else
                lines.append(tr("Trend on %1 %2: %3 by the moving averages; structure %4.").arg(name, m_engine->interval())
                                 .arg(rd.value(QStringLiteral("trend")).toString(),
                                      rd.value(QStringLiteral("structureTrend"), tr("not read yet")).toString()));
        } else if (fact == "signal") {
            // Trades come from SBT Model 1: what it shows on this market.
            if (!onChart) {
                lines.append(tr("SBT Model 1 runs on the chart's market; %1 is not on the chart.").arg(name));
                continue;
            }
            const QVariantMap current = m_engine->sbt().value(QStringLiteral("current")).toMap();
            if (current.isEmpty()) {
                lines.append(tr("SBT Model 1 on %1 %2: no setup yet; it is watching for a 1-2-3 with a break of "
                                "structure.").arg(name, m_engine->interval()));
                continue;
            }
            const QVariantMap zone = current.value(QStringLiteral("zone")).toMap();
            const QVariantMap review = current.value(QStringLiteral("review")).toMap();
            const QString status = current.value(QStringLiteral("status")).toString();
            lines.append(tr("SBT Model 1 on %1 %2: %3 setup, %4. Level 1 %5, Level 2 zone %6 – %7, Level 3 %8.")
                             .arg(name, m_engine->interval(), current.value(QStringLiteral("direction")).toString(), status)
                             .arg(price(current.value(QStringLiteral("level1")).toMap().value(QStringLiteral("price")).toDouble()),
                                  price(zone.value(QStringLiteral("bottom")).toDouble()),
                                  price(zone.value(QStringLiteral("top")).toDouble()),
                                  price(current.value(QStringLiteral("target")).toDouble())));
            if (review.value(QStringLiteral("done")).toBool())
                lines.append(review.value(QStringLiteral("confirmed")).toBool()
                                 ? tr("The AI team confirmed it (%1 of %2).").arg(review.value(QStringLiteral("agreeing")).toInt())
                                       .arg(review.value(QStringLiteral("voters")).toInt())
                                 : tr("The AI team did not confirm it."));
            else if (review.value(QStringLiteral("running")).toBool())
                lines.append(tr("The AI team is reviewing it now."));
        } else if (fact == "position") {
            bool any = false;
            const QVariantList open = m_engine->positions();
            for (const QVariant &v : open) {
                const QVariantMap p = v.toMap();
                if (p.value(QStringLiteral("symbol")).toString() != market)
                    continue;
                any = true;
                lines.append(tr("Open %1 on %2: entry %3, P&L %4 %5.")
                                 .arg(p.value(QStringLiteral("long")).toBool() ? tr("long") : tr("short"), name,
                                      price(p.value(QStringLiteral("entry")).toDouble()))
                                 .arg(p.value(QStringLiteral("pnl")).toDouble(), 0, 'f', 2)
                                 .arg(m_engine->accountCurrency()));
            }
            if (!any)
                lines.append(tr("No open paper position on %1.").arg(name));
        } else if (fact == "levels") {
            const Book *book = m_state.find(ss(market));
            if (!book || book->levels.empty()) {
                lines.append(tr("No levels have been named for %1 yet.").arg(name));
                continue;
            }
            QStringList parts;
            for (const Level &l : book->levels)
                parts.append(l.hasDistance ? tr("%1 %2 (%3 % %4)").arg(qs(l.kind), price(l.price), pct(l.distancePct),
                                                                    l.distancePct >= 0 ? tr("below") : tr("above"))
                                           : tr("%1 %2").arg(qs(l.kind), price(l.price)));
            lines.append(tr("Levels on %1: %2.").arg(name, parts.join(QStringLiteral(", "))));
        }
    }
    return lines.join(QLatin1Char('\n'));
}

QJsonObject DeskEngine::bookJson(const Book &book, bool compact) const
{
    QJsonArray entries, levels, conclusions;
    for (const Entry &e : book.entries) {
        if (compact)
            entries.append(QJsonObject{{QStringLiteral("text"), qs(e.text)},
                                       {QStringLiteral("side"), QLatin1String(sideName(e.side))},
                                       {QStringLiteral("source"), QLatin1String(sourceName(e.source))}});
        else
            entries.append(entryJson(e));
    }
    for (const Level &l : book.levels) {
        QJsonObject o{{QStringLiteral("price"), l.price}, {QStringLiteral("kind"), qs(l.kind)},
                      {QStringLiteral("source"), QLatin1String(sourceName(l.source))}};
        if (!l.relation.empty())
            o.insert(QStringLiteral("relation"), qs(l.relation));
        if (compact) {
            if (l.hasDistance)
                o.insert(QStringLiteral("price_vs_level_pct"), std::round(l.distancePct * 1000) / 1000);
        } else {
            o.insert(QStringLiteral("message"), l.message);
            o.insert(QStringLiteral("time"), l.time);
        }
        levels.append(o);
    }
    const size_t from = compact && book.conclusions.size() > 3 ? book.conclusions.size() - 3 : 0;
    for (size_t i = from; i < book.conclusions.size(); ++i)
        conclusions.append(compact ? QJsonValue(qs(book.conclusions[i].text)) : QJsonValue(entryJson(book.conclusions[i])));
    const Plan &p = book.plan;
    QJsonObject plan{{QStringLiteral("side"), qs(p.side)}, {QStringLiteral("entry"), p.entry},
                     {QStringLiteral("stop"), p.stop}, {QStringLiteral("target"), p.target},
                     {QStringLiteral("source"), QLatin1String(sourceName(p.source))}};
    if (!p.note.empty())
        plan.insert(QStringLiteral("note"), qs(p.note));
    if (compact) {
        const double rr = planRewardRisk(p);
        if (rr > 0)
            plan.insert(QStringLiteral("reward_risk"), std::round(rr * 100) / 100);
    } else {
        plan.insert(QStringLiteral("by"), qs(p.by));
        plan.insert(QStringLiteral("time"), p.time);
    }
    QJsonObject bias{{QStringLiteral("side"), QLatin1String(sideName(book.bias.side))},
                     {QStringLiteral("source"), QLatin1String(sourceName(book.bias.source))},
                     {QStringLiteral("why"), qs(book.bias.why)}};
    if (!compact) {
        bias.insert(QStringLiteral("by"), qs(book.bias.by));
        bias.insert(QStringLiteral("time"), book.bias.time);
    }
    const Lean lean = leanOf(book);
    QJsonObject o{{QStringLiteral("market"), qs(book.market)}, {QStringLiteral("name"), qs(book.name)},
                  {QStringLiteral("observations"), entries}, {QStringLiteral("levels"), levels},
                  {QStringLiteral("plan"), plan}, {QStringLiteral("conclusions"), conclusions}};
    if (book.bias.side != Side::None || !compact)
        o.insert(QStringLiteral("assessed_bias"), bias);
    if (compact)
        o.insert(QStringLiteral("conversation_lean"),
                 QJsonObject{{QStringLiteral("side"), QLatin1String(sideName(lean.side))},
                             {QStringLiteral("bullish_notes"), lean.bullish}, {QStringLiteral("bearish_notes"), lean.bearish}});
    else
        o.insert(QStringLiteral("updated"), book.updated);
    return o;
}

QJsonObject DeskEngine::liveJson(const QString &symbol) const
{
    if (symbol.isEmpty())
        return {{QStringLiteral("available"), false}, {QStringLiteral("note"), QStringLiteral("no market named yet")}};
    double p = 0;
    if (!livePrice(symbol, &p))
        return {{QStringLiteral("available"), false},
                {QStringLiteral("note"), listed(symbol) ? QStringLiteral("no live price yet (the market data is connecting or failing)")
                                                        : QStringLiteral("no live data for this market: it is not on the desk's market list")}};
    QJsonObject live{{QStringLiteral("available"), true}, {QStringLiteral("price"), p},
                     {QStringLiteral("time"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
    if (symbol != m_engine->symbol()) {
        live.insert(QStringLiteral("note"), QStringLiteral("only the price is live for this market; indicators are read "
                                                           "for the market on the chart"));
        return live;
    }
    live.insert(QStringLiteral("interval"), m_engine->interval());
    const QVariantMap rd = m_engine->readings();
    static const char *keys[] = {"trend", "ema20", "ema50", "ema200", "rsi14", "momentum", "macdHistogram", "macdState",
                                 "bandPosition", "bandWidthPct", "atr14", "atrPct", "volumeState", "structureTrend",
                                 "zone", "rangeHigh", "rangeLow", "lastEvent", "lastEventBullish", "lastEventLevel"};
    QJsonObject readings;
    for (const char *k : keys) {
        const QVariant v = rd.value(QLatin1String(k));
        if (v.isValid() && !v.isNull())
            readings.insert(QLatin1String(k), QJsonValue::fromVariant(v));
    }
    if (!readings.isEmpty())
        live.insert(QStringLiteral("readings"), readings);
    else
        live.insert(QStringLiteral("readings_note"), rd.value(QStringLiteral("taReason")).toString());
    const QVariantList open = m_engine->positions();
    for (const QVariant &v : open) {
        const QVariantMap pos = v.toMap();
        if (pos.value(QStringLiteral("symbol")).toString() == symbol)
            live.insert(QStringLiteral("open_paper_position"), QJsonObject::fromVariantMap(pos));
    }
    return live;
}

QJsonArray DeskEngine::recentTurns(qint64 before, int max) const
{
    QJsonArray turns;
    const QList<DeskMessages::Message> &all = m_messages->all();
    QList<const DeskMessages::Message *> picked;
    for (int i = int(all.size()) - 1; i >= 0 && picked.size() < max; --i) {
        const DeskMessages::Message &m = all.at(i);
        if (m.id >= before)
            continue;
        if (m.role == QLatin1String("you") || (m.role == QLatin1String("desk") && m.state == QLatin1String("done")))
            picked.prepend(&m);
    }
    for (const DeskMessages::Message *m : picked)
        turns.append(QJsonObject{{QStringLiteral("role"), m->role == QLatin1String("you") ? QStringLiteral("user")
                                                                                         : QStringLiteral("assistant")},
                                 {QStringLiteral("content"), m->text.left(1500)}});
    return turns;
}

QString DeskEngine::answerSystem() const
{
    return QStringLiteral(
        "You are the analyst on a trading desk, in a running conversation with the trader. You have followed the "
        "whole conversation; the desk context in the last message holds what has been established so far, the live "
        "market data and the analysis team's latest call.\n\n"
        "- Use the live data given. Never invent a price, an indicator value or news. If live data for the market is "
        "missing, say so.\n"
        "- Keep apart what the trader said (source \"you\"), what the live data shows (\"live\") and what the analysis "
        "team concluded (\"team\"). When a claim of the trader disagrees with the live readings, say so plainly.\n"
        "- This is a paper-trading desk. Give a clear view: the bias, the case for and against, where the idea is "
        "wrong (invalidation), and when a trade is discussed: entry, stop, target and reward/risk. Promise nothing.\n"
        "- The desk trades only by the trader's own strategy, SBT Model 1: a proper 1-2-3, a break of structure "
        "(a close beyond Level 1), the Level 2 zone (the lowest bearish candle at 2; bearish: the highest bullish "
        "one), a retest that does not close through the zone, entry at the next candle's open, target Level 3, with "
        "PD arrays (FVG, order block, demand/supply) as confirmation. Neither you nor the other models trade on "
        "your own view. Judge trade ideas in the strategy's terms, and say what it shows now (\"strategy\" in the "
        "context): a setup in play, its status, its levels, or that there is none.\n"
        "- Be concise: at most 170 words, short paragraphs or bullets. Answer in the language the trader writes in "
        "(they may write Roman Urdu).\n\n"
        "After the answer, as the very last thing, add the desk's state in exactly this form:\n"
        "```state\n"
        "{\"bias\": \"bullish|bearish|neutral|unclear\", \"why\": \"at most 15 words\", \"conclusion\": \"at most 20 "
        "words\", \"plan\": {\"side\": \"long|short|\", \"entry\": 0, \"stop\": 0, \"target\": 0}}\n"
        "```\n"
        "Use 0 and \"\" in the plan for what is not decided.");
}

QString DeskEngine::readSystem() const
{
    return QStringLiteral(
        "You keep the running analysis of a trading conversation up to date. You get a short summary of the "
        "conversation so far, the current analysis state of the market being discussed (JSON), and only the NEW "
        "messages from the trader. Work out what the new messages add, in the light of what came before (a "
        "follow-up like \"and the 4h?\" refers to the market already discussed). Do not repeat what the state "
        "already has. Take numbers only from the trader's words; invent nothing.\n\n"
        "Answer with one JSON object and nothing else:\n"
        "{\"summary\": \"the conversation so far, new messages included, at most 60 words\",\n"
        " \"market\": \"the market now discussed, e.g. BTC, ETH, XAUUSD, EURUSD; empty if not named\",\n"
        " \"observations\": [{\"topic\": \"rsi|macd|moving averages|bollinger|volume|divergence|structure|pattern|"
        "trend|macro|funding|sentiment|other\", \"text\": \"at most 8 words\", \"side\": \"bullish|bearish|neutral\"}],\n"
        " \"levels\": [{\"price\": number, \"kind\": \"support|resistance|entry|stop|target|level\"}],\n"
        " \"plan\": {\"side\": \"long|short|\", \"entry\": 0, \"stop\": 0, \"target\": 0},\n"
        " \"needs_answer\": [ids of new messages that ask a question or request something]}\n"
        "Use empty lists, \"\" and 0 for what the new messages do not say.");
}

void DeskEngine::startAnswer(qint64 question, bool deep, const QString &market, qint64 after)
{
    if (m_answered.contains(question))
        return;
    m_answered.insert(question);
    DeskMessages::Message a;
    a.id = ++m_nextId;
    a.role = QStringLiteral("desk");
    a.replyTo = question;
    a.market = market;
    a.time = nowMs();

    if (!m_hasKey) {
        a.kind = QStringLiteral("info");
        a.state = QStringLiteral("done");
        a.by = tr("desk");
        a.text = tr("I have read this and updated the analysis, but an opinion needs a model, and no NVIDIA API key "
                    "is set. Add one in Model Panel. Live facts (price, RSI, MACD, trend, the team's call, your "
                    "position, your levels) I can answer without one.");
        m_messages->insertAfter(after, a);
        logEvent(QStringLiteral("answered-nokey"), a.id);
        Q_EMIT answerFinished(a.id);
        return;
    }

    const QString model = deep ? m_config->deskDeepModel() : m_config->deskFastModel();
    a.kind = QStringLiteral("model");
    a.state = QStringLiteral("waiting");
    a.by = TradingConfig::displayName(model);
    m_messages->insertAfter(after, a);
    m_messages->trim(kMaxMessages);

    const DeskMessages::Message *q = m_messages->find(question);
    QJsonObject context{
        {QStringLiteral("market"), market.isEmpty() ? QStringLiteral("not named yet") : TradingEngine::displayName(market)},
        {QStringLiteral("conversation_summary"), qs(m_state.summary())},
        {QStringLiteral("analysis"), bookJson(m_state.book(ss(market)), true)},
        {QStringLiteral("live"), liveJson(market)},
    };
    const QVariantMap s = m_engine->signal();
    if (s.value(QStringLiteral("state")).toString() == QLatin1String("ready")
        && s.value(QStringLiteral("symbol")).toString() == market) {
        context.insert(QStringLiteral("team"),
                       QJsonObject{{QStringLiteral("action"), s.value(QStringLiteral("action")).toString()},
                                   {QStringLiteral("confidence"), s.value(QStringLiteral("confidence")).toDouble()},
                                   {QStringLiteral("agreeing"), s.value(QStringLiteral("agreeing")).toInt()},
                                   {QStringLiteral("voters"), s.value(QStringLiteral("voters")).toInt()},
                                   {QStringLiteral("interval"), s.value(QStringLiteral("interval")).toString()},
                                   {QStringLiteral("minutes_ago"),
                                    int((nowMs() - qint64(s.value(QStringLiteral("time")).toDouble())) / 60000)},
                                   {QStringLiteral("lead_reasoning"), s.value(QStringLiteral("reasoning")).toString()}});
    } else if (m_engine->analyzing()) {
        context.insert(QStringLiteral("team"), QJsonObject{{QStringLiteral("state"), QStringLiteral("working")},
                                                           {QStringLiteral("forming"),
                                                            QJsonObject::fromVariantMap(s.value(QStringLiteral("provisional")).toMap())}});
    }
    QJsonArray others;
    for (const auto &[key, book] : m_state.books()) {
        if (!key.empty() && qs(key) != market && (!book.entries.empty() || !book.levels.empty()))
            others.append(qs(book.name.empty() ? key : book.name));
    }
    if (!others.isEmpty())
        context.insert(QStringLiteral("other_markets_discussed"), others);
    // The strategy on the chart's market: the setup in play, or what it watches.
    if (!market.isEmpty() && market == m_engine->symbol()) {
        const QVariantMap sbt = m_engine->sbt();
        QJsonObject strategy{{QStringLiteral("name"), QStringLiteral("SBT Model 1")},
                             {QStringLiteral("interval"), m_engine->interval()}};
        const QVariantMap current = sbt.value(QStringLiteral("current")).toMap();
        if (current.isEmpty())
            strategy.insert(QStringLiteral("setup"), QStringLiteral("none yet: watching for a 1-2-3 with a break of structure"));
        else
            strategy.insert(QStringLiteral("setup"), QJsonObject::fromVariantMap(current));
        strategy.insert(QStringLiteral("watching"), QJsonObject::fromVariantMap(sbt.value(QStringLiteral("watching")).toMap()));
        // How the strategy's past entries on the loaded candles ended (by its rules, not traded).
        const QVariantMap history = m_engine->sbtHistory();
        QJsonObject past = QJsonObject::fromVariantMap(history.value(QStringLiteral("stats")).toMap());
        past.insert(QStringLiteral("candles"), history.value(QStringLiteral("candles")).toInt());
        past.insert(QStringLiteral("note"), QStringLiteral("the strategy's rules applied to the candles loaded; "
                                                           "a record of the past, not a forecast"));
        strategy.insert(QStringLiteral("history"), past);
        context.insert(QStringLiteral("strategy"), strategy);
    } else if (!market.isEmpty()) {
        context.insert(QStringLiteral("strategy"),
                       QJsonObject{{QStringLiteral("name"), QStringLiteral("SBT Model 1")},
                                   {QStringLiteral("setup"), QStringLiteral("not read: the strategy runs on the chart's market")}});
    }

    QJsonArray messages{QJsonObject{{QStringLiteral("role"), QStringLiteral("system")},
                                    {QStringLiteral("content"), answerSystem()}}};
    const QJsonArray turns = recentTurns(question, kRecentTurns);
    for (const QJsonValue &t : turns)
        messages.append(t);
    messages.append(QJsonObject{
        {QStringLiteral("role"), QStringLiteral("user")},
        {QStringLiteral("content"), QStringLiteral("Desk context (live now; it may have changed since earlier turns):\n")
                                        + QString::fromUtf8(QJsonDocument(context).toJson(QJsonDocument::Compact))
                                        + QStringLiteral("\n\nThe trader's message:\n") + (q ? q->text : QString())}});

    ChatStream::Request req;
    req.model = model;
    req.messages = messages;
    req.maxTokens = 1400;
    req.temperature = 0.3;
    req.stream = true;
    req.idleTimeoutMs = deep ? m_config->modelTimeoutSeconds() * 1000 : 60000;
    req.priority = 2;
    Task task;
    task.kind = QStringLiteral("answer");
    task.model = model;
    task.since = nowMs();
    task.stage = QStringLiteral("waiting");
    task.message = a.id;
    task.question = question;
    task.market = market;
    const quint64 request = m_stream->start(req);
    m_tasks.insert(request, task);
    logEvent(QStringLiteral("answer-asked"), a.id, model);
    taskChanged();
}

void DeskEngine::maybeStartTeam(const QString &market)
{
    if (market.isEmpty() || market != m_engine->symbol() || !m_hasKey)
        return;
    // The team reviews the strategy's setup: once per setup is enough.
    const QVariantMap current = m_engine->sbt().value(QStringLiteral("current")).toMap();
    if (current.isEmpty())
        return; // nothing in play: the models do not trade on their own view
    const QVariantMap review = current.value(QStringLiteral("review")).toMap();
    if (review.value(QStringLiteral("done")).toBool())
        return;
    if (m_engine->analyzing() || review.value(QStringLiteral("running")).toBool()) {
        m_teamAsked = true;
        m_teamMarket = market;
        return;
    }
    if (m_engine->analyzeNow()) {
        m_teamAsked = true;
        m_teamMarket = market;
        m_teamSince = nowMs();
        note(QStringLiteral("team"), tr("Asked the analysis team to review the SBT Model 1 setup on %1 in the "
                                        "background; its verdict will appear here when it is in.")
                                         .arg(TradingEngine::displayName(market)),
             market);
        taskChanged();
    }
}

// ---- the model's reading of new messages ------------------------------------------

void DeskEngine::scheduleRead()
{
    if (!m_running || m_unread.isEmpty() || m_reading)
        return;
    if (!m_readTimer->isActive()) {
        m_readTimer->start();
        invalidateStatus();
    }
}

void DeskEngine::flushRead()
{
    if (m_reading || m_unread.isEmpty())
        return;
    if (!m_hasKey) {
        invalidateStatus();
        return; // read again when a key comes
    }
    QList<qint64> batch;
    QString lines;
    while (!m_unread.isEmpty() && batch.size() < kReadBatch) {
        const qint64 id = m_unread.takeFirst();
        const DeskMessages::Message *m = m_messages->find(id);
        if (!m)
            continue;
        batch.append(id);
        lines += QStringLiteral("#%1: %2\n").arg(id).arg(m->text.left(1500));
    }
    if (batch.isEmpty())
        return;
    const QString market = qs(m_state.current());
    const QString user = QStringLiteral("Conversation summary so far:\n")
        + (m_state.summary().empty() ? QStringLiteral("(none yet)") : qs(m_state.summary()))
        + QStringLiteral("\n\nAnalysis state of ") + (market.isEmpty() ? QStringLiteral("(no market named yet)")
                                                                       : TradingEngine::displayName(market))
        + QStringLiteral(":\n") + QString::fromUtf8(QJsonDocument(bookJson(m_state.book(ss(market)), true))
                                                        .toJson(QJsonDocument::Compact))
        + QStringLiteral("\n\nNew messages:\n") + lines;
    ChatStream::Request req;
    req.model = m_config->deskFastModel();
    req.messages = QJsonArray{
        QJsonObject{{QStringLiteral("role"), QStringLiteral("system")}, {QStringLiteral("content"), readSystem()}},
        QJsonObject{{QStringLiteral("role"), QStringLiteral("user")}, {QStringLiteral("content"), user}}};
    req.maxTokens = 900;
    req.temperature = 0.1;
    req.stream = false;
    req.idleTimeoutMs = 60000;
    req.priority = 1;
    Task task;
    task.kind = QStringLiteral("read");
    task.model = req.model;
    task.since = nowMs();
    task.stage = QStringLiteral("waiting");
    task.batch = batch;
    task.market = market;
    task.focusAt = m_focusMessage;
    for (qint64 id : batch)
        m_messages->update(id, [this](DeskMessages::Message &m) {
            m.detail = tr("Being read by %1").arg(TradingConfig::displayName(m_config->deskFastModel()));
        }, {DeskMessages::DetailRole});
    m_reading = m_stream->start(req);
    m_tasks.insert(m_reading, task);
    QStringList ids;
    for (qint64 id : batch)
        ids.append(QString::number(id));
    logEvent(QStringLiteral("read-asked"), batch.front(), ids.join(QLatin1Char(',')));
    taskChanged();
}

void DeskEngine::applyReading(const Task &task, const QString &text)
{
    const QJsonObject o = firstObject(text);
    if (o.isEmpty()) {
        m_readError = tr("%1's reading could not be used.").arg(TradingConfig::displayName(task.model));
        m_readErrorAt = nowMs();
        for (qint64 id : task.batch)
            m_messages->update(id, [this](DeskMessages::Message &m) { m.detail = m_readError; },
                               {DeskMessages::DetailRole});
        return;
    }
    m_readError.clear();
    const qint64 now = nowMs();
    const std::string by = ss(TradingConfig::displayName(task.model));
    std::vector<Change> changes;
    const QString summary = o.value(QLatin1String("summary")).toString().trimmed();
    if (!summary.isEmpty())
        m_state.setSummary(ss(summary.left(600)));
    // The messages were about the market discussed when they were sent,
    // unless the model names another one (one the quick reading missed).
    std::string market = ss(task.market);
    const QString named = o.value(QLatin1String("market")).toString().trimmed();
    if (!named.isEmpty()) {
        const Resolved found = resolve(read(ss(named)));
        if (!found.symbol.isEmpty() && ss(found.symbol) != market) {
            market = ss(found.symbol);
            m_state.book(market).name = ss(found.name);
            // The conversation moves there only if no newer message has moved it since.
            if (m_focusMessage == task.focusAt) {
                const std::vector<Change> c = m_state.focus(market, ss(found.name), now);
                changes.insert(changes.end(), c.begin(), c.end());
            }
        }
    }
    const qint64 message = task.batch.isEmpty() ? 0 : task.batch.back();
    const QJsonArray observations = o.value(QLatin1String("observations")).toArray();
    QStringList added;
    for (const QJsonValue &v : observations) {
        const QJsonObject x = v.toObject();
        Entry e;
        e.topic = ss(x.value(QLatin1String("topic")).toString().toLower().left(24));
        e.text = ss(x.value(QLatin1String("text")).toString().simplified().left(80));
        e.side = sideFromName(ss(x.value(QLatin1String("side")).toString()));
        if (e.text.empty())
            continue;
        if (e.side == Side::None)
            e.side = Side::Neutral;
        e.source = Source::You; // the trader's words, read by a model
        e.by = "read by " + by;
        e.message = message;
        e.time = now;
        if (m_state.addEntry(market, e, &changes))
            added.append(qs(e.text));
    }
    double live = 0;
    const bool haveLive = livePrice(qs(market), &live);
    const QJsonArray levels = o.value(QLatin1String("levels")).toArray();
    Plan plan;
    plan.source = Source::You;
    plan.by = "read by " + by;
    plan.time = now;
    for (const QJsonValue &v : levels) {
        const QJsonObject x = v.toObject();
        Level l;
        l.price = number(x.value(QLatin1String("price")));
        if (!(l.price > 0) || (haveLive && (l.price < live * 0.5 || l.price > live * 2.0)))
            continue;
        static const QStringList kinds{QStringLiteral("support"), QStringLiteral("resistance"), QStringLiteral("entry"),
                                       QStringLiteral("stop"), QStringLiteral("target"), QStringLiteral("level")};
        const QString kind = x.value(QLatin1String("kind")).toString().toLower();
        l.kind = ss(kinds.contains(kind) ? kind : QStringLiteral("level"));
        l.source = Source::You;
        l.message = message;
        l.time = now;
        if (m_state.addLevel(market, l, &changes))
            added.append(price(l.price));
    }
    const QJsonObject p = o.value(QLatin1String("plan")).toObject();
    const QString side = p.value(QLatin1String("side")).toString().toLower();
    if (side == QLatin1String("long") || side == QLatin1String("short"))
        plan.side = ss(side);
    const auto sane = [&](double v) { return v > 0 && (!haveLive || (v >= live * 0.5 && v <= live * 2.0)) ? v : 0; };
    plan.entry = sane(number(p.value(QLatin1String("entry"))));
    plan.stop = sane(number(p.value(QLatin1String("stop"))));
    plan.target = sane(number(p.value(QLatin1String("target"))));
    if (!plan.side.empty() || plan.entry > 0 || plan.stop > 0 || plan.target > 0)
        m_state.setPlan(market, plan, &changes);
    m_state.record(changes);
    if (!changes.empty())
        touched(QStringLiteral("model"), qs(changes.back().text));

    m_lastUnderstood = now;
    const QString readBy = tr("Read by %1").arg(qs(by));
    for (qint64 id : task.batch) {
        m_messages->update(id, [&](DeskMessages::Message &m) {
            m.state = QStringLiteral("understood");
            m.detail = readBy;
            if (id == message)
                for (const QString &a : added)
                    if (!m.tags.contains(a))
                        m.tags.append(a);
        }, {DeskMessages::StateRole, DeskMessages::DetailRole, DeskMessages::TagsRole});
    }
    // A question the quick reading above did not see as one.
    const QJsonArray needs = o.value(QLatin1String("needs_answer")).toArray();
    for (const QJsonValue &v : needs) {
        const qint64 id = qint64(number(v));
        if (task.batch.contains(id) && !m_answered.contains(id)) {
            startAnswer(id, true, qs(market));
            logEvent(QStringLiteral("answer-from-reading"), id);
        }
    }
    invalidate();
}

// ---- answers ---------------------------------------------------------------------------

QString DeskEngine::withoutState(const QString &text)
{
    const int block = text.indexOf(QLatin1String("```state"));
    return block >= 0 ? text.left(block) : text;
}

QString DeskEngine::visiblePart(const QString &text)
{
    const int block = text.indexOf(QLatin1String("```state"));
    if (block >= 0)
        return text.left(block);
    // Hold back the start of the block while it is still arriving.
    static const QString marker = QStringLiteral("```state");
    for (int n = marker.size() - 1; n >= 1; --n) {
        if (text.endsWith(marker.left(n)))
            return text.left(text.size() - n);
    }
    return text;
}

void DeskEngine::onDelta(quint64 request, const QString &piece)
{
    const auto it = m_tasks.find(request);
    if (it == m_tasks.end() || it->kind != QLatin1String("answer"))
        return;
    if (it->stage != QLatin1String("writing")) {
        it->stage = QStringLiteral("writing");
        logEvent(QStringLiteral("first-words"), it->message);
        taskChanged();
    }
    m_messages->update(it->message, [&](DeskMessages::Message &m) {
        m.raw += piece; // the answer so far, the state block included
        m.text = visiblePart(m.raw);
        m.state = QStringLiteral("writing");
        m.detail.clear();
    }, {DeskMessages::TextRole, DeskMessages::StateRole, DeskMessages::DetailRole});
}

void DeskEngine::onFinished(quint64 request, const QString &text, qint64 ms)
{
    const Task task = m_tasks.take(request);
    if (task.kind == QLatin1String("read")) {
        m_reading = 0;
        logEvent(QStringLiteral("read-done"), task.batch.isEmpty() ? 0 : task.batch.front(), QString::number(ms));
        applyReading(task, text);
        taskChanged();
        scheduleRead();
        scheduleSave();
        return;
    }
    if (task.kind != QLatin1String("answer"))
        return;
    QString visible = withoutState(text).trimmed();
    const int block = text.indexOf(QLatin1String("```state"));
    if (block >= 0)
        applyAnswerState(task, firstObject(text.mid(block)));
    if (visible.isEmpty())
        visible = tr("(The model gave no answer text.)");
    m_messages->update(task.message, [&](DeskMessages::Message &m) {
        m.text = visible;
        m.state = QStringLiteral("done");
        m.ms = ms;
        m.detail.clear();
        m.raw.clear();
    });
    logEvent(QStringLiteral("answered"), task.message, QString::number(ms));
    Q_EMIT answerFinished(task.message);
    taskChanged();
    scheduleSave();
}

void DeskEngine::applyAnswerState(const Task &task, const QJsonObject &o)
{
    if (o.isEmpty())
        return;
    const qint64 now = nowMs();
    const std::string market = ss(task.market);
    const std::string by = ss(TradingConfig::displayName(task.model));
    std::vector<Change> changes;
    const QString biasName = o.value(QLatin1String("bias")).toString();
    const Side side = sideFromName(ss(biasName));
    if (side != Side::None || biasName == QLatin1String("unclear")) {
        Bias b;
        b.side = side;
        b.source = Source::Model;
        b.by = by;
        b.why = ss(o.value(QLatin1String("why")).toString().simplified().left(160));
        b.time = now;
        m_state.setBias(market, b, &changes);
    }
    const QString conclusion = o.value(QLatin1String("conclusion")).toString().simplified();
    if (!conclusion.isEmpty()) {
        Entry e;
        e.topic = "conclusion";
        e.text = ss(conclusion.left(200));
        e.side = side;
        e.source = Source::Model;
        e.by = by;
        e.message = task.message;
        e.time = now;
        m_state.addConclusion(market, e, &changes);
    }
    const QJsonObject p = o.value(QLatin1String("plan")).toObject();
    Plan plan;
    plan.source = Source::Model;
    plan.by = by;
    plan.time = now;
    const QString planSide = p.value(QLatin1String("side")).toString().toLower();
    if (planSide == QLatin1String("long") || planSide == QLatin1String("short"))
        plan.side = ss(planSide);
    double live = 0;
    const bool haveLive = livePrice(task.market, &live);
    const auto sane = [&](double v) { return v > 0 && (!haveLive || (v >= live * 0.5 && v <= live * 2.0)) ? v : 0; };
    plan.entry = sane(number(p.value(QLatin1String("entry"))));
    plan.stop = sane(number(p.value(QLatin1String("stop"))));
    plan.target = sane(number(p.value(QLatin1String("target"))));
    // A plan with numbers; a bare direction from the model does not replace the trader's.
    if (plan.entry > 0 || plan.stop > 0 || plan.target > 0)
        m_state.setPlan(market, plan, &changes);
    m_state.record(changes);
    if (!changes.empty())
        touched(QStringLiteral("model"), qs(changes.back().text));
    invalidate();
}

void DeskEngine::onFailed(quint64 request, const QString &code, const QString &message, qint64 ms)
{
    const Task task = m_tasks.take(request);
    if (task.kind == QLatin1String("read")) {
        m_reading = 0;
        if (code != QLatin1String("cancelled")) {
            m_readError = message;
            m_readErrorAt = nowMs();
            for (qint64 id : task.batch)
                m_messages->update(id, [&](DeskMessages::Message &m) {
                    m.detail = tr("Not read by a model: %1").arg(message);
                }, {DeskMessages::DetailRole});
        }
        logEvent(QStringLiteral("read-failed"), task.batch.isEmpty() ? 0 : task.batch.front(), code);
        taskChanged();
        scheduleRead();
        return;
    }
    if (task.kind != QLatin1String("answer"))
        return;
    const bool stopped = code == QLatin1String("cancelled");
    m_messages->update(task.message, [&](DeskMessages::Message &m) {
        m.text = withoutState(m.raw).trimmed();
        m.raw.clear();
        m.state = stopped ? QStringLiteral("stopped") : QStringLiteral("failed");
        m.detail = stopped ? tr("Stopped.") : message;
        m.ms = ms;
    });
    logEvent(stopped ? QStringLiteral("stopped") : QStringLiteral("answer-failed"), task.message, code);
    Q_EMIT answerFinished(task.message);
    taskChanged();
    scheduleSave();
}

void DeskEngine::stopAnswer(double id)
{
    for (auto it = m_tasks.cbegin(); it != m_tasks.cend(); ++it) {
        if (it->kind == QLatin1String("answer") && it->message == qint64(id)) {
            m_stream->cancel(it.key());
            return;
        }
    }
}

void DeskEngine::retryAnswer(double id)
{
    const DeskMessages::Message *m = m_messages->find(qint64(id));
    if (!m || m->role != QLatin1String("desk") || m->replyTo == 0)
        return;
    const QString state = m->state;
    if (state != QLatin1String("failed") && state != QLatin1String("stopped") && state != QLatin1String("interrupted"))
        return;
    const qint64 question = m->replyTo;
    const QString market = m->market; // asked again about what it was asked about
    m_answered.remove(question);
    m_messages->update(qint64(id), [](DeskMessages::Message &x) {
        x.state = QStringLiteral("superseded");
    }, {DeskMessages::StateRole});
    startAnswer(question, true, market, qint64(id));
}

void DeskEngine::clearConversation()
{
    m_stream->cancelAll();
    m_tasks.clear();
    m_reading = 0;
    m_unread.clear();
    m_answered.clear();
    m_tokens.clear();
    m_tokenOrder.clear();
    m_warnedUnlisted.clear();
    m_state.clear();
    m_signalChanges.clear();
    m_messages->reset({});
    m_lastUpdate = nowMs();
    m_lastUpdateSource = QStringLiteral("you");
    m_lastUpdateText = tr("Conversation cleared");
    logEvent(QStringLiteral("cleared"), 0);
    invalidate();
    taskChanged();
    save();
}

QString DeskEngine::setModel(const QString &which, const QString &model)
{
    const QString error = m_config->setDeskModel(which, model);
    Q_EMIT settingsChanged();
    return error;
}

void DeskEngine::setFollowChart(bool on)
{
    m_config->setDeskFollowChart(on);
    Q_EMIT settingsChanged();
}

// ---- the market side ----------------------------------------------------------------------

void DeskEngine::onMarketTick()
{
    const qint64 now = nowMs();
    m_lastTick = now;
    std::vector<Change> moves;
    for (const auto &[key, book] : m_state.books()) {
        if (key.empty() || book.levels.empty())
            continue;
        double p = 0;
        if (!livePrice(qs(key), &p))
            continue;
        const std::vector<Change> c = m_state.priceUpdate(key, p, nearPctFor(qs(key)), now);
        moves.insert(moves.end(), c.begin(), c.end());
    }
    notes(moves);

    // Indicator readings of the chart's market, compared with the last ones.
    const QString symbol = m_engine->symbol();
    Snapshot &last = m_snapshots[symbol];
    if (now - last.time >= kReadingsMs) {
        const QVariantMap rd = m_engine->readings();
        if (rd.value(QStringLiteral("taValid")).toBool()) {
            Snapshot next;
            next.readings = rd;
            next.time = now;
            next.zoneRsi = rsiZone(rd.value(QStringLiteral("rsi14")).toDouble());
            next.macd = rd.value(QStringLiteral("macdState")).toString();
            next.trend = rd.value(QStringLiteral("trend")).toString();
            next.event = rd.value(QStringLiteral("lastEvent")).toString() + QLatin1Char(':')
                + QString::number(rd.value(QStringLiteral("lastEventLevel")).toDouble(), 'g', 10);
            const QString name = TradingEngine::displayName(symbol);
            QStringList found;
            if (last.time > 0) {
                if (next.zoneRsi != last.zoneRsi)
                    found.append(tr("RSI(14) now %1 (%2)").arg(next.zoneRsi)
                                     .arg(rd.value(QStringLiteral("rsi14")).toDouble(), 0, 'f', 1));
                if (next.macd != last.macd && !next.macd.isEmpty())
                    found.append(tr("MACD now %1").arg(next.macd));
                if (next.trend != last.trend && !next.trend.isEmpty())
                    found.append(tr("Trend now %1").arg(next.trend));
                if (next.event != last.event && !rd.value(QStringLiteral("lastEvent")).toString().isEmpty())
                    found.append(tr("%1 %2 at %3")
                                     .arg(rd.value(QStringLiteral("lastEventBullish")).toBool() ? tr("Bullish") : tr("Bearish"),
                                          rd.value(QStringLiteral("lastEvent")).toString() == QLatin1String("choch")
                                              ? tr("change of character") : tr("break of structure"),
                                          price(rd.value(QStringLiteral("lastEventLevel")).toDouble())));
            }
            for (const QString &f : found) {
                m_signalChanges.prepend(QVariantMap{{QStringLiteral("time"), double(now)},
                                                    {QStringLiteral("market"), symbol},
                                                    {QStringLiteral("name"), name},
                                                    {QStringLiteral("text"), f},
                                                    {QStringLiteral("source"), QStringLiteral("market")}});
                logEvent(QStringLiteral("signal-change"), 0, f);
            }
            while (m_signalChanges.size() > 30)
                m_signalChanges.removeLast();
            if (!found.isEmpty())
                touched(QStringLiteral("market"), found.last());
            last = next;
        }
    }
    invalidate();
    if (!moves.empty())
        scheduleSave();
}

// "Close" scales with how much the market moves: half an ATR on the chart's
// market, else a quarter of a per cent (crypto) or a twentieth (forex).
double DeskEngine::nearPctFor(const QString &symbol) const
{
    double nearPct = m_engine->marketKind() == QLatin1String("forex") ? 0.05 : 0.25;
    const auto snap = m_snapshots.constFind(symbol);
    if (snap != m_snapshots.constEnd()) {
        const double atrPct = snap->readings.value(QStringLiteral("atrPct")).toDouble();
        if (atrPct > 0)
            nearPct = std::clamp(atrPct * 0.5, 0.02, 1.0);
    }
    return nearPct;
}

void DeskEngine::onSignal()
{
    const QVariantMap s = m_engine->signal();
    const QString state = s.value(QStringLiteral("state")).toString();
    if (state == QLatin1String("analyzing")) {
        if (m_teamSince == 0)
            m_teamSince = nowMs();
        const QVariantMap forming = s.value(QStringLiteral("provisional")).toMap();
        if (!forming.isEmpty())
            logEvent(QStringLiteral("team-forming"), 0,
                     QStringLiteral("%1/%2 %3").arg(forming.value(QStringLiteral("answered")).toInt())
                         .arg(forming.value(QStringLiteral("asked")).toInt())
                         .arg(forming.value(QStringLiteral("leaning")).toString()));
        taskChanged();
        return;
    }
    const bool wasTeam = m_teamSince != 0;
    m_teamSince = 0;
    if (state != QLatin1String("ready")) {
        if (wasTeam)
            taskChanged();
        return;
    }
    const double time = s.value(QStringLiteral("time")).toDouble();
    if (time == m_lastSignalTime) {
        if (wasTeam)
            taskChanged();
        return;
    }
    m_lastSignalTime = time;
    const QString market = s.value(QStringLiteral("symbol")).toString();
    const QString action = s.value(QStringLiteral("action")).toString();
    const QString name = s.value(QStringLiteral("symbolName")).toString();
    const double confidence = s.value(QStringLiteral("confidence")).toDouble();
    const qint64 now = nowMs();

    // The team's call is part of the market's analysis.
    std::vector<Change> changes;
    Bias b;
    b.side = action == QLatin1String("buy") ? Side::Bullish : action == QLatin1String("sell") ? Side::Bearish : Side::Neutral;
    b.source = Source::Team;
    b.by = ss(tr("analysis team"));
    b.why = ss(s.value(QStringLiteral("reasoning")).toString().simplified().left(160));
    b.time = now;
    if (m_state.find(ss(market)) || market == qs(m_state.current()))
        m_state.setBias(ss(market), b, &changes);
    m_state.record(changes);
    const QString text = action == QLatin1String("hold")
        ? tr("Analysis team on %1 %2: no trade (%3 analysts answered). %4")
              .arg(name, s.value(QStringLiteral("interval")).toString())
              .arg(s.value(QStringLiteral("voters")).toInt())
              .arg(s.value(QStringLiteral("note")).toString())
        : tr("Analysis team on %1 %2: %3, confidence %4 (%5 of %6 agree). %7")
              .arg(name, s.value(QStringLiteral("interval")).toString(), action.toUpper())
              .arg(confidence, 0, 'f', 1)
              .arg(s.value(QStringLiteral("agreeing")).toInt()).arg(s.value(QStringLiteral("voters")).toInt())
              .arg(s.value(QStringLiteral("outcome")).toString());
    m_signalChanges.prepend(QVariantMap{{QStringLiteral("time"), double(now)}, {QStringLiteral("market"), market},
                                        {QStringLiteral("name"), name},
                                        {QStringLiteral("text"), tr("Team: %1").arg(action.toUpper())},
                                        {QStringLiteral("source"), QStringLiteral("team")}});
    while (m_signalChanges.size() > 30)
        m_signalChanges.removeLast();
    // In the conversation when the desk asked for it, or when the call on the
    // market being discussed changed; a repeat of the same call is not news.
    const QString previous = m_teamCalls.value(market);
    m_teamCalls.insert(market, action);
    if ((m_teamAsked && m_teamMarket == market) || (market == qs(m_state.current()) && previous != action))
        note(QStringLiteral("team"), text.trimmed(), market);
    m_teamAsked = false;
    touched(QStringLiteral("team"), tr("Team: %1 on %2").arg(action.toUpper(), name));
    taskChanged();
    invalidate();
    scheduleSave();
}

// ---- notes, status -------------------------------------------------------------------

void DeskEngine::note(const QString &kind, const QString &text, const QString &market)
{
    DeskMessages::Message n;
    n.id = ++m_nextId;
    n.role = QStringLiteral("note");
    n.kind = kind;
    n.state = QStringLiteral("done");
    n.text = text;
    n.market = market;
    n.time = nowMs();
    m_messages->append(n);
    m_messages->trim(kMaxMessages);
    logEvent(QStringLiteral("note-") + kind, n.id, text);
    scheduleSave();
}

void DeskEngine::notes(const std::vector<Change> &changes)
{
    for (const Change &c : changes) {
        if (c.field == "price") {
            note(QStringLiteral("market"), qs(c.text), qs(c.market));
            touched(QStringLiteral("market"), qs(c.text));
        }
    }
}

void DeskEngine::touched(const QString &source, const QString &what)
{
    m_lastUpdate = nowMs();
    m_lastUpdateSource = source;
    m_lastUpdateText = what;
    invalidateStatus();
}

void DeskEngine::taskChanged()
{
    Q_EMIT tasksChanged();
    invalidateStatus();
    if (idle())
        Q_EMIT becameIdle();
}

bool DeskEngine::idle() const
{
    return m_tasks.isEmpty() && (m_unread.isEmpty() || !m_hasKey) && !m_readTimer->isActive()
        && !m_engine->analyzing();
}

bool DeskEngine::listed(const QString &symbol) const
{
    return m_config->watchlist().contains(symbol);
}

void DeskEngine::invalidateStatus()
{
    m_statusDirty = true;
    Q_EMIT statusChanged();
}

QVariantMap DeskEngine::status() const
{
    // Read by many bindings at once; worked out once per change.
    if (!m_statusDirty)
        return m_statusCache;
    m_statusCache = buildStatus();
    m_statusDirty = false;
    return m_statusCache;
}

QVariantMap DeskEngine::buildStatus() const
{
    const QString data = m_engine->dataState();
    const QString feed = !m_running ? QStringLiteral("off")
        : data == QLatin1String("error") ? QStringLiteral("error")
        : data != QLatin1String("live") ? QStringLiteral("waiting")
        : m_engine->streaming() ? QStringLiteral("stream") : QStringLiteral("polling");
    int read = 0, understood = 0;
    for (const DeskMessages::Message &m : m_messages->all()) {
        if (m.role != QLatin1String("you"))
            continue;
        read += m.state != QLatin1String("accepted");
        understood += m.state == QLatin1String("understood");
    }
    int answering = 0;
    for (const Task &t : m_tasks)
        answering += t.kind == QLatin1String("answer");
    const bool key = !!m_hasKey;
    const bool readError = !m_readError.isEmpty() && nowMs() - m_readErrorAt < 120000;
    return {
        {QStringLiteral("running"), m_running},
        {QStringLiteral("feed"), feed},
        {QStringLiteral("feedError"), m_engine->dataError()},
        {QStringLiteral("pollSeconds"), m_engine->pollSeconds()},
        {QStringLiteral("chartMarket"), m_engine->symbolName()},
        {QStringLiteral("read"), read},
        {QStringLiteral("understood"), understood},
        {QStringLiteral("lastRead"), double(m_lastRead)},
        {QStringLiteral("lastUnderstood"), double(m_lastUnderstood)},
        {QStringLiteral("lastUpdate"), double(m_lastUpdate)},
        {QStringLiteral("lastUpdateSource"), m_lastUpdateSource},
        {QStringLiteral("lastUpdateText"), m_lastUpdateText},
        {QStringLiteral("lastTick"), double(m_lastTick)},
        {QStringLiteral("modelReading"), !key ? QStringLiteral("no_key") : readError ? QStringLiteral("error")
                                                                                       : QStringLiteral("on")},
        {QStringLiteral("readError"), readError ? m_readError : QString()},
        {QStringLiteral("waitingToRead"), int(m_unread.size())},
        {QStringLiteral("answering"), answering},
        {QStringLiteral("teamWorking"), m_engine->analyzing()},
        {QStringLiteral("idle"), idle()},
    };
}

QVariantList DeskEngine::tasks() const
{
    QVariantList list;
    QList<quint64> ids = m_tasks.keys();
    std::sort(ids.begin(), ids.end());
    for (quint64 id : ids) {
        const Task &t = m_tasks.value(id);
        const QString model = TradingConfig::displayName(t.model);
        QString label;
        if (t.kind == QLatin1String("read"))
            label = tr("Reading %n new message(s)", nullptr, int(t.batch.size()));
        else
            label = tr("Answering");
        list.append(QVariantMap{{QStringLiteral("kind"), t.kind}, {QStringLiteral("label"), label},
                                {QStringLiteral("model"), model}, {QStringLiteral("since"), double(t.since)},
                                {QStringLiteral("stage"), t.stage}, {QStringLiteral("message"), double(t.message)},
                                {QStringLiteral("market"), TradingEngine::displayName(t.market)}});
    }
    if (m_engine->analyzing()) {
        const QVariantMap s = m_engine->signal();
        const QVariantMap forming = s.value(QStringLiteral("provisional")).toMap();
        QString stage = s.value(QStringLiteral("stage")).toString() == QLatin1String("review") ? tr("team review")
                                                                                                : tr("first calls");
        if (!forming.isEmpty())
            stage += tr(" · %1 of %2 answered · leaning %3")
                         .arg(forming.value(QStringLiteral("answered")).toInt())
                         .arg(forming.value(QStringLiteral("asked")).toInt())
                         .arg(forming.value(QStringLiteral("leaning")).toString().toUpper());
        list.append(QVariantMap{{QStringLiteral("kind"), QStringLiteral("team")},
                                {QStringLiteral("label"), tr("Analysis team")},
                                {QStringLiteral("model"), tr("%n analyst(s)", nullptr,
                                                             forming.value(QStringLiteral("asked")).toInt())},
                                {QStringLiteral("since"), double(m_teamSince)},
                                {QStringLiteral("stage"), stage},
                                {QStringLiteral("market"), m_engine->symbolName()}});
    }
    return list;
}

void DeskEngine::invalidate()
{
    m_analysisDirty = true;
    Q_EMIT analysisChanged();
}

QVariantMap DeskEngine::analysis() const
{
    // Read by many bindings at once; worked out once per change.
    if (!m_analysisDirty)
        return m_analysisCache;
    m_analysisCache = buildAnalysis();
    m_analysisDirty = false;
    return m_analysisCache;
}

QVariantMap DeskEngine::buildAnalysis() const
{
    const std::string current = m_state.current();
    const Book *book = m_state.find(current);
    Book empty;
    if (!book)
        book = &empty;
    QVariantList bullish, bearish, neutral, indicators, levels, conclusions;
    for (auto it = book->entries.rbegin(); it != book->entries.rend(); ++it) {
        const QVariantMap e = entryMap(*it);
        if (indicatorTopic(it->topic))
            indicators.append(e);
        if (it->side == Side::Bullish)
            bullish.append(e);
        else if (it->side == Side::Bearish)
            bearish.append(e);
        else
            neutral.append(e);
    }
    for (const Level &l : book->levels)
        levels.append(QVariantMap{{QStringLiteral("price"), l.price}, {QStringLiteral("text"), price(l.price)},
                                  {QStringLiteral("kind"), qs(l.kind)}, {QStringLiteral("relation"), qs(l.relation)},
                                  {QStringLiteral("source"), QLatin1String(sourceName(l.source))},
                                  {QStringLiteral("hasDistance"), l.hasDistance},
                                  {QStringLiteral("distancePct"), l.distancePct}, {QStringLiteral("near"), l.near},
                                  {QStringLiteral("above"), l.state == LevelState::Above}});
    for (auto it = book->conclusions.rbegin(); it != book->conclusions.rend(); ++it)
        conclusions.append(entryMap(*it));
    const Plan &p = book->plan;
    const Lean lean = leanOf(*book);
    QVariantMap live;
    double px = 0;
    const QString market = qs(current);
    if (livePrice(market, &px)) {
        live.insert(QStringLiteral("price"), px);
        live.insert(QStringLiteral("text"), price(px));
        if (market == m_engine->symbol()) {
            const auto snap = m_snapshots.constFind(market);
            if (snap != m_snapshots.constEnd() && snap->time > 0) {
                const QVariantMap &rd = snap->readings;
                live.insert(QStringLiteral("rsi14"), rd.value(QStringLiteral("rsi14")));
                live.insert(QStringLiteral("rsiZone"), snap->zoneRsi);
                live.insert(QStringLiteral("macdState"), snap->macd);
                live.insert(QStringLiteral("trend"), snap->trend);
                live.insert(QStringLiteral("structure"), rd.value(QStringLiteral("structureTrend")));
                live.insert(QStringLiteral("interval"), m_engine->interval());
                live.insert(QStringLiteral("readAt"), double(snap->time));
            }
        }
    }
    QVariantMap team;
    const QVariantMap s = m_engine->signal();
    if (s.value(QStringLiteral("symbol")).toString() == market && s.value(QStringLiteral("state")).toString() == QLatin1String("ready"))
        team = QVariantMap{{QStringLiteral("action"), s.value(QStringLiteral("action"))},
                           {QStringLiteral("confidence"), s.value(QStringLiteral("confidence"))},
                           {QStringLiteral("agreeing"), s.value(QStringLiteral("agreeing"))},
                           {QStringLiteral("voters"), s.value(QStringLiteral("voters"))},
                           {QStringLiteral("interval"), s.value(QStringLiteral("interval"))},
                           {QStringLiteral("time"), s.value(QStringLiteral("time"))}};
    QStringList others;
    for (const auto &[key, b] : m_state.books()) {
        if (!key.empty() && key != current && (!b.entries.empty() || !b.levels.empty()))
            others.append(qs(b.name.empty() ? key : b.name));
    }
    const double rr = planRewardRisk(p);
    return {
        {QStringLiteral("market"), market},
        {QStringLiteral("name"), qs(book->name.empty() ? current : book->name)},
        {QStringLiteral("onChart"), !market.isEmpty() && market == m_engine->symbol()},
        {QStringLiteral("listed"), listed(market)},
        {QStringLiteral("live"), live},
        {QStringLiteral("bias"), QVariantMap{{QStringLiteral("side"), QLatin1String(sideName(book->bias.side))},
                                             {QStringLiteral("source"), QLatin1String(sourceName(book->bias.source))},
                                             {QStringLiteral("by"), qs(book->bias.by)},
                                             {QStringLiteral("why"), qs(book->bias.why)},
                                             {QStringLiteral("time"), double(book->bias.time)}}},
        {QStringLiteral("lean"), QVariantMap{{QStringLiteral("side"), QLatin1String(sideName(lean.side))},
                                             {QStringLiteral("bullish"), lean.bullish},
                                             {QStringLiteral("bearish"), lean.bearish}}},
        {QStringLiteral("bullish"), bullish},
        {QStringLiteral("bearish"), bearish},
        {QStringLiteral("neutral"), neutral},
        {QStringLiteral("indicators"), indicators},
        {QStringLiteral("levels"), levels},
        {QStringLiteral("plan"), QVariantMap{{QStringLiteral("side"), qs(p.side)}, {QStringLiteral("entry"), p.entry},
                                             {QStringLiteral("stop"), p.stop}, {QStringLiteral("target"), p.target},
                                             {QStringLiteral("entryText"), p.entry > 0 ? price(p.entry) : QString()},
                                             {QStringLiteral("stopText"), p.stop > 0 ? price(p.stop) : QString()},
                                             {QStringLiteral("targetText"), p.target > 0 ? price(p.target) : QString()},
                                             {QStringLiteral("rewardRisk"), rr},
                                             {QStringLiteral("problem"), qs(planProblem(p))},
                                             {QStringLiteral("source"), QLatin1String(sourceName(p.source))},
                                             {QStringLiteral("by"), qs(p.by)}}},
        {QStringLiteral("conclusions"), conclusions},
        {QStringLiteral("summary"), qs(m_state.summary())},
        {QStringLiteral("team"), team},
        {QStringLiteral("otherMarkets"), others},
        {QStringLiteral("revision"), double(m_state.revision())},
    };
}

QVariantList DeskEngine::changes() const
{
    QVariantList list;
    const std::vector<Change> &all = m_state.changes();
    for (auto it = all.rbegin(); it != all.rend() && list.size() < 30; ++it)
        list.append(QVariantMap{{QStringLiteral("time"), double(it->time)}, {QStringLiteral("market"), qs(it->market)},
                                {QStringLiteral("name"), it->market.empty() ? QString() : TradingEngine::displayName(qs(it->market))},
                                {QStringLiteral("field"), qs(it->field)}, {QStringLiteral("text"), qs(it->text)},
                                {QStringLiteral("source"), QLatin1String(sourceName(it->source))}});
    return list;
}

QVariantList DeskEngine::signalChanges() const
{
    return m_signalChanges;
}

QVariantMap DeskEngine::settings() const
{
    QVariantList models;
    for (const TradingConfig::Member &m : m_config->panel())
        models.append(QVariantMap{{QStringLiteral("id"), m.model}, {QStringLiteral("name"), m_config->memberName(m.model)},
                                  {QStringLiteral("role"), m.role}});
    return {{QStringLiteral("fastModel"), m_config->deskFastModel()},
            {QStringLiteral("deepModel"), m_config->deskDeepModel()},
            {QStringLiteral("fastName"), TradingConfig::displayName(m_config->deskFastModel())},
            {QStringLiteral("deepName"), TradingConfig::displayName(m_config->deskDeepModel())},
            {QStringLiteral("followChart"), m_config->deskFollowChart()},
            {QStringLiteral("models"), models}};
}

// ---- saved state ---------------------------------------------------------------------------

void DeskEngine::scheduleSave()
{
    if (m_loaded && !m_saveTimer->isActive())
        m_saveTimer->start();
}

void DeskEngine::load()
{
    m_loaded = true;
    QFile file(m_path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    if (root.value(QLatin1String("version")).toInt() != 1) {
        // Kept aside, never silently overwritten: it is the user's conversation.
        const QString aside = file.fileName() + QStringLiteral(".unreadable");
        QFile::remove(aside);
        QFile::rename(file.fileName(), aside);
        return;
    }
    QList<DeskMessages::Message> rows;
    const QJsonArray saved = root.value(QLatin1String("messages")).toArray();
    for (const QJsonValue &v : saved) {
        DeskMessages::Message m = DeskMessages::fromJson(v.toObject());
        if (m.id <= 0)
            continue;
        if (m.role == QLatin1String("desk")
            && (m.state == QLatin1String("waiting") || m.state == QLatin1String("thinking")
                || m.state == QLatin1String("writing"))) {
            m.text = m.text.trimmed();
            m.state = QStringLiteral("interrupted");
            m.detail = tr("The application closed before this answer was complete.");
        }
        if (m.role == QLatin1String("you") && m.state == QLatin1String("read"))
            m_unread.append(m.id);
        if (m.role == QLatin1String("desk") && m.replyTo)
            m_answered.insert(m.replyTo);
        m_nextId = std::max(m_nextId, m.id);
        rows.append(m);
    }
    m_nextId = std::max<qint64>(m_nextId, root.value(QLatin1String("next_id")).toInteger());
    m_messages->reset(rows);

    const QJsonObject state = root.value(QLatin1String("state")).toObject();
    std::map<std::string, Book> books;
    const QJsonObject savedBooks = state.value(QLatin1String("books")).toObject();
    for (auto it = savedBooks.constBegin(); it != savedBooks.constEnd(); ++it) {
        const QJsonObject b = it.value().toObject();
        Book book;
        book.market = ss(it.key());
        book.name = ss(b.value(QLatin1String("name")).toString());
        for (const QJsonValue &v : b.value(QLatin1String("observations")).toArray())
            book.entries.push_back(entryFrom(v.toObject()));
        for (const QJsonValue &v : b.value(QLatin1String("levels")).toArray()) {
            const QJsonObject o = v.toObject();
            Level l;
            l.price = o.value(QLatin1String("price")).toDouble();
            l.kind = ss(o.value(QLatin1String("kind")).toString());
            l.relation = ss(o.value(QLatin1String("relation")).toString());
            l.source = sourceFromName(ss(o.value(QLatin1String("source")).toString()));
            l.message = o.value(QLatin1String("message")).toInteger();
            l.time = o.value(QLatin1String("time")).toInteger();
            if (l.price > 0)
                book.levels.push_back(l);
        }
        const QJsonObject p = b.value(QLatin1String("plan")).toObject();
        book.plan.side = ss(p.value(QLatin1String("side")).toString());
        book.plan.entry = p.value(QLatin1String("entry")).toDouble();
        book.plan.stop = p.value(QLatin1String("stop")).toDouble();
        book.plan.target = p.value(QLatin1String("target")).toDouble();
        book.plan.note = ss(p.value(QLatin1String("note")).toString());
        book.plan.source = sourceFromName(ss(p.value(QLatin1String("source")).toString()));
        book.plan.by = ss(p.value(QLatin1String("by")).toString());
        book.plan.time = p.value(QLatin1String("time")).toInteger();
        const QJsonObject bias = b.value(QLatin1String("assessed_bias")).toObject();
        book.bias.side = sideFromName(ss(bias.value(QLatin1String("side")).toString()));
        book.bias.source = sourceFromName(ss(bias.value(QLatin1String("source")).toString()));
        book.bias.by = ss(bias.value(QLatin1String("by")).toString());
        book.bias.why = ss(bias.value(QLatin1String("why")).toString());
        book.bias.time = bias.value(QLatin1String("time")).toInteger();
        for (const QJsonValue &v : b.value(QLatin1String("conclusions")).toArray())
            book.conclusions.push_back(entryFrom(v.toObject()));
        book.updated = b.value(QLatin1String("updated")).toInteger();
        books[book.market] = book;
    }
    std::vector<Change> changes;
    for (const QJsonValue &v : state.value(QLatin1String("changes")).toArray()) {
        const QJsonObject o = v.toObject();
        changes.push_back({o.value(QLatin1String("time")).toInteger(), ss(o.value(QLatin1String("market")).toString()),
                           ss(o.value(QLatin1String("field")).toString()), ss(o.value(QLatin1String("text")).toString()),
                           sourceFromName(ss(o.value(QLatin1String("source")).toString()))});
    }
    m_state.restore(ss(state.value(QLatin1String("current")).toString()), books, changes,
                    ss(state.value(QLatin1String("summary")).toString()));
    m_signalChanges = root.value(QLatin1String("signal_changes")).toArray().toVariantList();
    const QJsonObject calls = root.value(QLatin1String("team_calls")).toObject();
    for (auto it = calls.constBegin(); it != calls.constEnd(); ++it)
        m_teamCalls.insert(it.key(), it.value().toString());
    m_lastSignalTime = root.value(QLatin1String("last_signal_time")).toDouble();
    logEvent(QStringLiteral("loaded"), 0, QString::number(rows.size()));
}

void DeskEngine::save()
{
    if (!m_loaded)
        return;
    QJsonArray messages;
    for (const DeskMessages::Message &m : m_messages->all()) {
        DeskMessages::Message copy = m;
        // An answer under way is saved as far as it got.
        if (m.role == QLatin1String("desk") && (m.state == QLatin1String("writing") || m.state == QLatin1String("thinking")
                                                 || m.state == QLatin1String("waiting")))
            copy.text = visiblePart(m.raw.isEmpty() ? m.text : m.raw);
        messages.append(DeskMessages::toJson(copy));
    }
    QJsonObject books;
    for (const auto &[key, book] : m_state.books())
        books.insert(qs(key), bookJson(book, false));
    QJsonArray changes;
    for (const Change &c : m_state.changes())
        changes.append(QJsonObject{{QStringLiteral("time"), c.time}, {QStringLiteral("market"), qs(c.market)},
                                   {QStringLiteral("field"), qs(c.field)}, {QStringLiteral("text"), qs(c.text)},
                                   {QStringLiteral("source"), QLatin1String(sourceName(c.source))}});
    QJsonObject calls;
    for (auto it = m_teamCalls.cbegin(); it != m_teamCalls.cend(); ++it)
        calls.insert(it.key(), it.value());
    const QJsonObject root{
        {QStringLiteral("version"), 1},
        {QStringLiteral("next_id"), m_nextId},
        {QStringLiteral("messages"), messages},
        {QStringLiteral("state"), QJsonObject{{QStringLiteral("current"), qs(m_state.current())},
                                              {QStringLiteral("summary"), qs(m_state.summary())},
                                              {QStringLiteral("books"), books},
                                              {QStringLiteral("changes"), changes}}},
        {QStringLiteral("signal_changes"), QJsonArray::fromVariantList(m_signalChanges)},
        {QStringLiteral("team_calls"), calls},
        {QStringLiteral("last_signal_time"), m_lastSignalTime},
    };
    QSaveFile file(m_path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (file.commit())
        QFile::setPermissions(file.fileName(), QFile::ReadOwner | QFile::WriteOwner);
}

void DeskEngine::logEvent(const QString &event, qint64 id, const QString &detail)
{
    QJsonObject e{{QStringLiteral("t"), double(m_clock.nsecsElapsed()) / 1e6}, {QStringLiteral("event"), event}};
    if (id)
        e.insert(QStringLiteral("id"), id);
    if (!detail.isEmpty())
        e.insert(QStringLiteral("detail"), detail.left(300));
    m_log.append(e);
    while (m_log.size() > kMaxLog)
        m_log.removeFirst();
}

QJsonObject DeskEngine::dump() const
{
    QJsonArray messages;
    for (const DeskMessages::Message &m : m_messages->all())
        messages.append(DeskMessages::toJson(m));
    return {{QStringLiteral("messages"), messages},
            {QStringLiteral("analysis"), QJsonObject::fromVariantMap(analysis())},
            {QStringLiteral("status"), QJsonObject::fromVariantMap(status())},
            {QStringLiteral("tasks"), QJsonArray::fromVariantList(tasks())},
            {QStringLiteral("changes"), QJsonArray::fromVariantList(changes())},
            {QStringLiteral("signalChanges"), QJsonArray::fromVariantList(m_signalChanges)},
            {QStringLiteral("settings"), QJsonObject::fromVariantMap(settings())},
            {QStringLiteral("log"), m_log}};
}
