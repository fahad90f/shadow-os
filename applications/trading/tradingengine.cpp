// SPDX-License-Identifier: GPL-3.0-or-later
#include "tradingengine.h"

#include "chartrenderer.h"
#include "modelclient.h"
#include "tradingconfig.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimeZone>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <cmath>

using namespace Trading;

namespace {

const int kHistory = 300;        // candles kept of the selected market
const int kRecentCandles = 20;   // how far back a liquidity sweep is still worth mentioning
const int kPollMs = 5000;
const int kMaxActivity = 200;
const int kMaxCurve = 500;
const int kMaxStoredTrades = 2000;

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

QString qs(const std::string &s)
{
    return QString::fromStdString(s);
}

// Eight significant digits: enough for any price, short enough for a prompt.
double rounded(double v)
{
    if (!std::isfinite(v) || v == 0)
        return 0;
    const double scale = std::pow(10.0, 7 - std::floor(std::log10(std::fabs(v))));
    return std::round(v * scale) / scale;
}

QJsonValue numberOrNull(double v)
{
    return std::isfinite(v) ? QJsonValue(rounded(v)) : QJsonValue();
}

int closedCount(const Candles &candles)
{
    return int(candles.size()) - (!candles.empty() && !candles.back().closed ? 1 : 0);
}

QJsonObject positionJson(const Position &p)
{
    return {{QStringLiteral("id"), qint64(p.id)}, {QStringLiteral("symbol"), qs(p.symbol)},
            {QStringLiteral("long"), p.isLong}, {QStringLiteral("quantity"), p.quantity},
            {QStringLiteral("entry"), p.entry}, {QStringLiteral("stop"), p.stop},
            {QStringLiteral("take_profit"), p.takeProfit}, {QStringLiteral("opened_at"), qint64(p.openedAt)},
            {QStringLiteral("open_fee"), p.openFee}, {QStringLiteral("last_price"), p.lastPrice},
            {QStringLiteral("confidence"), p.confidence}, {QStringLiteral("lead"), qs(p.lead)},
            {QStringLiteral("reasoning"), qs(p.reasoning)}};
}

Position positionFrom(const QJsonObject &o)
{
    Position p;
    p.id = o.value(QLatin1String("id")).toInteger();
    p.symbol = o.value(QLatin1String("symbol")).toString().toStdString();
    p.isLong = o.value(QLatin1String("long")).toBool(true);
    p.quantity = o.value(QLatin1String("quantity")).toDouble();
    p.entry = o.value(QLatin1String("entry")).toDouble();
    p.stop = o.value(QLatin1String("stop")).toDouble();
    p.takeProfit = o.value(QLatin1String("take_profit")).toDouble();
    p.openedAt = o.value(QLatin1String("opened_at")).toInteger();
    p.openFee = o.value(QLatin1String("open_fee")).toDouble();
    p.lastPrice = o.value(QLatin1String("last_price")).toDouble();
    p.confidence = o.value(QLatin1String("confidence")).toDouble();
    p.lead = o.value(QLatin1String("lead")).toString().toStdString();
    p.reasoning = o.value(QLatin1String("reasoning")).toString().toStdString();
    return p;
}

QJsonObject tradeJson(const Trade &t)
{
    return {{QStringLiteral("id"), qint64(t.id)}, {QStringLiteral("symbol"), qs(t.symbol)},
            {QStringLiteral("long"), t.isLong}, {QStringLiteral("quantity"), t.quantity},
            {QStringLiteral("entry"), t.entry}, {QStringLiteral("exit"), t.exit},
            {QStringLiteral("opened_at"), qint64(t.openedAt)}, {QStringLiteral("closed_at"), qint64(t.closedAt)},
            {QStringLiteral("fees"), t.fees}, {QStringLiteral("pnl"), t.pnl}, {QStringLiteral("pnl_pct"), t.pnlPct},
            {QStringLiteral("reason"), qs(t.reason)}, {QStringLiteral("confidence"), t.confidence},
            {QStringLiteral("lead"), qs(t.lead)}, {QStringLiteral("reasoning"), qs(t.reasoning)}};
}

Trade tradeFrom(const QJsonObject &o)
{
    Trade t;
    t.id = o.value(QLatin1String("id")).toInteger();
    t.symbol = o.value(QLatin1String("symbol")).toString().toStdString();
    t.isLong = o.value(QLatin1String("long")).toBool(true);
    t.quantity = o.value(QLatin1String("quantity")).toDouble();
    t.entry = o.value(QLatin1String("entry")).toDouble();
    t.exit = o.value(QLatin1String("exit")).toDouble();
    t.openedAt = o.value(QLatin1String("opened_at")).toInteger();
    t.closedAt = o.value(QLatin1String("closed_at")).toInteger();
    t.fees = o.value(QLatin1String("fees")).toDouble();
    t.pnl = o.value(QLatin1String("pnl")).toDouble();
    t.pnlPct = o.value(QLatin1String("pnl_pct")).toDouble();
    t.reason = o.value(QLatin1String("reason")).toString().toStdString();
    t.confidence = o.value(QLatin1String("confidence")).toDouble();
    t.lead = o.value(QLatin1String("lead")).toString().toStdString();
    t.reasoning = o.value(QLatin1String("reasoning")).toString().toStdString();
    return t;
}

// A number a model wrote either as a number or as "92520.5".
double looseNumber(const QJsonValue &v)
{
    if (v.isDouble())
        return v.toDouble();
    if (v.isString()) {
        bool ok = false;
        QString s = v.toString().trimmed();
        s.remove(QLatin1Char(','));
        s.remove(QLatin1Char('%'));
        const double d = s.toDouble(&ok);
        return ok ? d : std::nan("");
    }
    return std::nan("");
}

} // namespace

TradingEngine::TradingEngine(TradingConfig *config, QObject *parent)
    : QObject(parent)
    , m_config(config)
    , m_net(new QNetworkAccessManager(this))
    , m_data(new MarketData(m_net, this))
    , m_models(new ModelClient(config, this))
    , m_timer(new QTimer(this))
    , m_account(config->paper())
{
    qRegisterMetaType<Trading::Candles>("Trading::Candles");
    qRegisterMetaType<QList<MarketData::Ticker>>("QList<MarketData::Ticker>");
    m_data->setBaseUrl(config->dataUrl());
    m_timer->setInterval(kPollMs);
    connect(m_timer, &QTimer::timeout, this, &TradingEngine::poll);
    connect(m_data, &MarketData::candles, this, &TradingEngine::onCandles);
    connect(m_data, &MarketData::tickers, this, &TradingEngine::onTickers);
    connect(m_data, &MarketData::failed, this, &TradingEngine::onDataFailed);
    connect(m_data, &MarketData::unlisted, this, &TradingEngine::onUnlisted);
    connect(m_models, &ModelClient::answered, this, &TradingEngine::onModelAnswered);
    connect(m_models, &ModelClient::failed, this, &TradingEngine::onModelFailed);
    connect(m_models, &ModelClient::modelsListed, this, [this](const QStringList &, const QString &error) {
        m_catalogError = error;
        Q_EMIT catalogChanged();
        Q_EMIT analystsChanged(); // a model the service no longer offers shows as unavailable
    });
    connect(m_config, &TradingConfig::changed, this, [this] {
        m_account.setCosts(m_config->paper().feePct, m_config->paper().slippagePct);
        Q_EMIT settingsChanged();
        Q_EMIT analystsChanged();
    });
    connect(m_config, &TradingConfig::keysChanged, this, [this] {
        // A new key starts with a clean slate.
        m_analystState.clear();
        Q_EMIT analystsChanged();
    });
    m_signal.insert(QStringLiteral("state"), QStringLiteral("none"));
    load();
    m_account.setCosts(m_config->paper().feePct, m_config->paper().slippagePct);
}

TradingEngine::~TradingEngine()
{
    save();
}

// ---- selection ---------------------------------------------------------------

QString TradingEngine::symbol() const { return m_config->symbol(); }
QString TradingEngine::interval() const { return m_config->interval(); }
QStringList TradingEngine::intervals() const { return TradingConfig::intervals(); }
QString TradingEngine::dataSource() const { return QUrl(m_config->dataUrl()).host(); }
bool TradingEngine::autoTrade() const { return m_config->autoTrade(); }
bool TradingEngine::autoAnalyze() const { return m_config->autoAnalyze(); }

QString TradingEngine::displayName(const QString &symbol)
{
    static const QStringList quotes = {QStringLiteral("USDT"), QStringLiteral("USDC"), QStringLiteral("FDUSD"),
                                       QStringLiteral("BTC"), QStringLiteral("ETH"), QStringLiteral("EUR")};
    for (const QString &quote : quotes) {
        if (symbol.endsWith(quote) && symbol.size() > quote.size())
            return symbol.left(symbol.size() - quote.size()) + QLatin1Char('/') + quote;
    }
    return symbol;
}

void TradingEngine::resetMarket()
{
    // The candles, the readings and the signal all belong to the market that
    // was selected; none of them says anything about the new one.
    ++m_generation;
    m_models->cancelAll();
    for (const QString &id : std::as_const(m_testing))
        Q_EMIT modelTested(id, false, tr("Cancelled."));
    m_testing.clear();
    m_waiting.clear();
    for (auto it = m_analystState.begin(); it != m_analystState.end(); ++it) {
        if (it->state == QLatin1String("working"))
            it->state = QStringLiteral("ready");
    }
    m_analyzing = false;
    m_candles.clear();
    m_higherCandles.clear();
    m_higherFor.clear();
    m_lastClosedTime = 0;
    m_seenTime = 0;
    m_lastSignal = Signal();
    m_lastDecision = RiskDecision();
    m_signal = QVariantMap{{QStringLiteral("state"), QStringLiteral("none")}};
    m_marketReadySent = false;
    // An error about the candles was about the market that was on the chart.
    if (!m_candleError.isEmpty() && setDataError(QStringLiteral("candles"), QString()))
        Q_EMIT marketChanged();
    ++m_chartRevision;
    Q_EMIT chartChanged();
    Q_EMIT signalChanged();
    Q_EMIT analystsChanged();
    if (m_running)
        poll();
}

void TradingEngine::setSymbol(const QString &symbol)
{
    if (symbol == m_config->symbol() || !m_config->watchlist().contains(symbol))
        return;
    m_config->setSymbol(symbol);
    resetMarket();
    Q_EMIT selectionChanged();
    Q_EMIT marketChanged();
}

void TradingEngine::setInterval(const QString &interval)
{
    if (interval == m_config->interval() || !TradingConfig::intervals().contains(interval))
        return;
    m_config->setInterval(interval);
    resetMarket();
    Q_EMIT selectionChanged();
}

void TradingEngine::setAutoTrade(bool on)
{
    if (on == m_config->autoTrade())
        return;
    m_config->setAutoTrade(on);
    log(QStringLiteral("info"), on ? tr("Auto trade turned on: signals that pass the risk rules are placed on the paper account.")
                                   : tr("Auto trade turned off: signals are shown, nothing is placed."));
}

void TradingEngine::setAutoAnalyze(bool on)
{
    m_config->setAutoAnalyze(on);
}

// ---- market data -----------------------------------------------------------------

void TradingEngine::start()
{
    if (m_running)
        return;
    m_running = true;
    m_pollCount = 0;
    m_timer->start();
    m_models->listModels(); // which of the panel's models the service offers
    poll();
}

void TradingEngine::stop()
{
    m_running = false;
    m_timer->stop();
}

void TradingEngine::poll()
{
    const QString symbol = m_config->symbol();
    const QString interval = m_config->interval();
    m_data->fetchTickers(m_config->watchlist());
    // The whole history once, then only the newest candles.
    m_data->fetchCandles(symbol, interval, m_candles.size() < 100 ? kHistory : 3);
    const QString higher = TradingConfig::higherInterval(interval);
    const QString key = symbol + QLatin1Char(':') + higher;
    if (!higher.isEmpty() && (m_higherFor != key || m_pollCount % 12 == 0))
        m_data->fetchCandles(symbol, higher, 250, QStringLiteral("higher"));
    // The watchlist's 24-hour lines: at the start and every ten minutes.
    if (m_pollCount % 120 == 0) {
        const QStringList symbols = m_config->watchlist();
        for (const QString &listed : symbols)
            m_data->fetchCandles(listed, QStringLiteral("1h"), 24, QStringLiteral("spark"));
    }
    ++m_pollCount;
}

bool TradingEngine::setDataError(const QString &what, const QString &message)
{
    (what == QLatin1String("tickers") ? m_tickerError : m_candleError) = message;
    // The chart's candles are what the analysis runs on, so their error is
    // the one shown when both sources have something to say. Keeping the two
    // apart also keeps a working source from clearing the other's error
    // every few seconds.
    const QString error = m_candleError.isEmpty() ? m_tickerError : m_candleError;
    const QString state = error.isEmpty() ? QStringLiteral("live") : QStringLiteral("error");
    if (state == m_dataState && error == m_dataError)
        return false;
    m_dataState = state;
    m_dataError = error;
    if (!error.isEmpty())
        log(QStringLiteral("error"), error);
    return true;
}

void TradingEngine::onDataFailed(const QString &what, const QString &message)
{
    if (setDataError(what, message))
        Q_EMIT marketChanged();
    if (!m_marketReadySent) {
        m_marketReadySent = true;
        Q_EMIT marketReady();
    }
}

void TradingEngine::onUnlisted(const QString &symbol)
{
    // For the market on the chart the failed candles say the same thing.
    if (symbol != m_config->symbol()) {
        log(QStringLiteral("error"),
            tr("%1 is not a market the data service lists. Check the watchlist in the settings file.").arg(symbol));
    }
    m_tickers.remove(symbol);
    Q_EMIT marketChanged();
}

void TradingEngine::onTickers(const QList<MarketData::Ticker> &list)
{
    const QString selected = m_config->symbol();
    for (const MarketData::Ticker &t : list) {
        m_tickers.insert(t.symbol, t);
        // Positions on markets that are not on the chart follow the ticker.
        if (t.symbol != selected && m_account.positionOn(t.symbol.toStdString()))
            priceUpdate(t.symbol, t.price, t.price, t.price);
    }
    setDataError(QStringLiteral("tickers"), QString());
    Q_EMIT marketChanged();
}

void TradingEngine::onCandles(const QString &symbol, const QString &interval, const Candles &series,
                              const QString &purpose)
{
    if (purpose == QLatin1String("spark")) {
        QVariantList closes;
        for (const Candle &c : series)
            closes.append(c.close);
        m_sparks.insert(symbol, closes);
        Q_EMIT marketChanged();
        return;
    }
    if (symbol != m_config->symbol())
        return; // an answer for the market that was selected before
    if (purpose == QLatin1String("higher")) {
        if (interval == TradingConfig::higherInterval(m_config->interval())) {
            m_higherCandles = series;
            m_higherFor = symbol + QLatin1Char(':') + interval;
        }
        return;
    }
    if (interval != m_config->interval())
        return; // the interval was changed while this was on its way

    if (series.size() >= 100 || m_candles.empty()) {
        m_candles = series;
    } else {
        // Merge the newest candles into the tail by their open time.
        for (const Candle &c : series) {
            if (c.openTime > m_candles.back().openTime) {
                m_candles.push_back(c);
            } else {
                for (auto it = m_candles.rbegin(); it != m_candles.rend() && it->openTime >= c.openTime; ++it) {
                    if (it->openTime == c.openTime) {
                        *it = c;
                        break;
                    }
                }
            }
        }
        if (m_candles.size() > size_t(kHistory))
            m_candles.erase(m_candles.begin(), m_candles.end() - kHistory);
    }

    // The price extremes since the last update, for stops and targets. What
    // an already-seen candle did before we last looked has been applied.
    const Candle &last = m_candles.back();
    double high = last.close, low = last.close;
    if (m_seenTime != 0) {
        for (auto it = m_candles.rbegin(); it != m_candles.rend() && it->openTime >= m_seenTime; ++it) {
            if (it->openTime == m_seenTime) {
                if (it->high > m_seenHigh)
                    high = std::max(high, it->high);
                if (it->low < m_seenLow)
                    low = std::min(low, it->low);
            } else {
                high = std::max(high, it->high);
                low = std::min(low, it->low);
            }
        }
    }
    m_seenTime = last.openTime;
    m_seenHigh = last.high;
    m_seenLow = last.low;
    priceUpdate(symbol, last.close, high, low);

    if (setDataError(QStringLiteral("candles"), QString()))
        Q_EMIT marketChanged();
    ++m_chartRevision;
    Q_EMIT chartChanged();
    if (!m_marketReadySent) {
        m_marketReadySent = true;
        Q_EMIT marketReady();
    }

    // A candle just closed: that is when the analysts are asked.
    const int closed = closedCount(m_candles);
    const long long newest = closed > 0 ? m_candles[size_t(closed - 1)].openTime : 0;
    const bool first = m_lastClosedTime == 0;
    if (newest != m_lastClosedTime) {
        m_lastClosedTime = newest;
        // The skills' state depends on how much history there is.
        Q_EMIT analystsChanged();
        if (!first && m_config->autoAnalyze() && !m_analyzing)
            analyzeNow();
    }
}

void TradingEngine::priceUpdate(const QString &symbol, double last, double high, double low)
{
    const std::vector<Trade> closed = m_account.onPrice(symbol.toStdString(), last, high, low, nowMs());
    if (!closed.empty())
        recordClosed(closed);
    afterAccountChange(!closed.empty());
}

void TradingEngine::recordClosed(const std::vector<Trade> &closed)
{
    for (const Trade &t : closed) {
        const QString reason = t.reason == "take_profit" ? tr("target reached")
            : t.reason == "stop_loss" ? tr("stopped out")
            : t.reason == "signal_flip" ? tr("signal reversed")
            : t.reason == "risk_halt" ? tr("risk halt") : tr("closed by you");
        log(QStringLiteral("trade"),
            tr("Closed %1 %2 at %3 (%4): %5%6 USDT")
                .arg(t.isLong ? tr("long") : tr("short"), displayName(qs(t.symbol)))
                .arg(t.exit, 0, 'g', 8).arg(reason)
                .arg(t.pnl >= 0 ? QStringLiteral("+") : QString()).arg(t.pnl, 0, 'f', 2));
    }
}

void TradingEngine::afterAccountChange(bool tradeClosed)
{
    const qint64 now = nowMs();
    m_account.mark(now);
    const AccountView view = accountView(m_config->symbol(), now);
    if (!m_halted && drawdownReached(view, m_config->risk())) {
        m_halted = true;
        log(QStringLiteral("risk"),
            tr("Trading halted: the account is %1 % below its peak (limit %2 %). Open positions keep their stops; "
               "no new trade is placed until you resume.")
                .arg(drawdownPct(view.equity, view.peakEquity), 0, 'f', 1)
                .arg(m_config->risk().maxDrawdownPct, 0, 'f', 0));
        tradeClosed = true; // save now
    }
    // A point on the equity curve at every closed trade and every five minutes.
    if (tradeClosed || now - m_lastCurvePoint >= 300000) {
        m_lastCurvePoint = now;
        m_equityCurve.append(QVariantMap{{QStringLiteral("t"), double(now)}, {QStringLiteral("v"), m_account.equity()}});
        while (m_equityCurve.size() > kMaxCurve)
            m_equityCurve.removeFirst();
        save();
    }
    Q_EMIT accountChanged();
}

// ---- analysis ----------------------------------------------------------------------

QStringList TradingEngine::skillsOf(const QString &role)
{
    // Everyone sets levels, so everyone gets the risk brief; the rest follows
    // what the role looks at.
    if (role == QLatin1String("analysis"))
        return {QStringLiteral("ta"), QStringLiteral("smc"), QStringLiteral("risk")};
    if (role == QLatin1String("reasoning"))
        return {QStringLiteral("smc"), QStringLiteral("ta"), QStringLiteral("risk")};
    if (role == QLatin1String("vision"))
        return {QStringLiteral("chart"), QStringLiteral("smc"), QStringLiteral("risk")};
    return {QStringLiteral("ta"), QStringLiteral("risk")};
}

QString TradingEngine::systemPrompt(const QString &role, double minRewardRisk)
{
    QString focus;
    if (role == QLatin1String("analysis"))
        focus = QStringLiteral("weigh trend, momentum and volatility together into one overall read.");
    else if (role == QLatin1String("reasoning"))
        focus = QStringLiteral("argue the case against the obvious trade before you accept it, and name what would "
                               "prove it wrong.");
    else if (role == QLatin1String("vision"))
        focus = QStringLiteral("you also get a picture of the chart. Read its shape and check that the picture "
                               "agrees with the numbers.");
    else
        focus = QStringLiteral("work the numbers: distances to the moving averages and bands in ATR units, and where "
                               "a stop and a target would fall relative to recent swings and gaps.");
    QString prompt = QStringLiteral(
        "You are one analyst on a panel that reviews one market at a time. You receive a JSON snapshot: recent "
        "candles, indicator readings and market-structure readings, all computed from exchange data. Decide whether "
        "the snapshot shows a trade worth taking right now. An order would be placed at the snapshot's price.\n\n"
        "- Use only the snapshot. Do not assume news, events or prices that are not in it.\n"
        "- Most snapshots show no clear edge. Answer hold unless the evidence for one direction is clear and the "
        "evidence against it is weak.\n"
        "- For a buy, stop_loss is below the price and take_profit above it; for a sell the opposite. The target "
        "must be at least %1 times as far from the price as the stop.\n"
        "- confidence is 0 to 100: how strongly the snapshot supports your action. It is not a probability of "
        "profit. Use 0 for hold.\n"
        "- reasoning: at most 60 words of plain language naming the two or three readings that decided it.\n\n"
        "Your focus on the panel: %2\n\n"
        "Answer with one JSON object and nothing else:\n"
        "{\"action\": \"buy\" | \"sell\" | \"hold\", \"confidence\": number, \"stop_loss\": number, "
        "\"take_profit\": number, \"reasoning\": string}\n"
        "For hold, stop_loss and take_profit are 0.")
        .arg(minRewardRisk, 0, 'g', 3).arg(focus);
    // The skill briefs are compiled into the binary (prompts/skill-*.md).
    const QStringList skills = skillsOf(role);
    for (const QString &skill : skills) {
        QFile brief(QStringLiteral(":/qt/qml/org/shadow/trading/prompts/skill-%1.md").arg(skill));
        if (brief.open(QIODevice::ReadOnly))
            prompt += QStringLiteral("\n\n") + QString::fromUtf8(brief.readAll()).trimmed();
    }
    return prompt;
}

QJsonObject TradingEngine::snapshot(const Indicators::Summary &ta, const Indicators::Summary *higher,
                                    const Smc::Analysis &smc) const
{
    const int closed = closedCount(m_candles);
    QJsonArray rows;
    for (int i = std::max(0, closed - 40); i < closed; ++i) {
        const Candle &c = m_candles[size_t(i)];
        rows.append(QJsonArray{QDateTime::fromMSecsSinceEpoch(c.openTime, QTimeZone::UTC).toString(Qt::ISODate),
                               rounded(c.open), rounded(c.high), rounded(c.low), rounded(c.close),
                               rounded(c.volume)});
    }
    const auto ago = [closed](int index) { return closed - 1 - index; };

    QJsonObject indicators{
        {QStringLiteral("price"), rounded(ta.price)},
        {QStringLiteral("ema20"), numberOrNull(ta.ema20)}, {QStringLiteral("ema50"), numberOrNull(ta.ema50)},
        {QStringLiteral("ema200"), numberOrNull(ta.ema200)}, {QStringLiteral("trend"), qs(ta.trend)},
        {QStringLiteral("rsi14"), numberOrNull(ta.rsi14)}, {QStringLiteral("momentum"), qs(ta.momentum)},
        {QStringLiteral("macd"), numberOrNull(ta.macd)}, {QStringLiteral("macd_signal"), numberOrNull(ta.macdSignal)},
        {QStringLiteral("macd_histogram"), numberOrNull(ta.macdHistogram)},
        {QStringLiteral("macd_state"), qs(ta.macdState)},
        {QStringLiteral("bollinger_upper"), numberOrNull(ta.bandUpper)},
        {QStringLiteral("bollinger_middle"), numberOrNull(ta.bandMiddle)},
        {QStringLiteral("bollinger_lower"), numberOrNull(ta.bandLower)},
        {QStringLiteral("bollinger_position"), numberOrNull(ta.bandPosition)},
        {QStringLiteral("bollinger_width_pct"), numberOrNull(ta.bandWidthPct)},
        {QStringLiteral("atr14"), numberOrNull(ta.atr14)}, {QStringLiteral("atr_pct"), numberOrNull(ta.atrPct)},
        {QStringLiteral("volume_ratio"), numberOrNull(ta.volumeRatio)},
        {QStringLiteral("volume_state"), qs(ta.volumeState)},
        {QStringLiteral("change_24_candles_pct"), numberOrNull(ta.change24Pct)},
    };

    QJsonObject structure{{QStringLiteral("available"), smc.valid}};
    if (smc.valid) {
        structure.insert(QStringLiteral("trend"), QLatin1String(Smc::trendName(smc.trend)));
        structure.insert(QStringLiteral("zone"), qs(smc.zone));
        structure.insert(QStringLiteral("range_high"), rounded(smc.rangeHigh));
        structure.insert(QStringLiteral("range_low"), rounded(smc.rangeLow));
        structure.insert(QStringLiteral("equilibrium"), rounded(smc.equilibrium));
        const auto direction = [](bool bullish) { return bullish ? QStringLiteral("bullish") : QStringLiteral("bearish"); };
        QJsonArray events, blocks, gaps, sweeps;
        for (size_t i = smc.events.size() > 4 ? smc.events.size() - 4 : 0; i < smc.events.size(); ++i) {
            const Smc::StructureEvent &e = smc.events[i];
            events.append(QJsonObject{{QStringLiteral("type"), e.changeOfCharacter ? QStringLiteral("change_of_character")
                                                                                   : QStringLiteral("break_of_structure")},
                                      {QStringLiteral("direction"), direction(e.bullish)},
                                      {QStringLiteral("level"), rounded(e.level)},
                                      {QStringLiteral("candles_ago"), ago(e.index)}});
        }
        for (auto it = smc.orderBlocks.rbegin(); it != smc.orderBlocks.rend() && blocks.size() < 4; ++it) {
            if (it->mitigated)
                continue;
            blocks.append(QJsonObject{{QStringLiteral("direction"), direction(it->bullish)},
                                      {QStringLiteral("top"), rounded(it->top)},
                                      {QStringLiteral("bottom"), rounded(it->bottom)},
                                      {QStringLiteral("candles_ago"), ago(it->index)}});
        }
        for (auto it = smc.gaps.rbegin(); it != smc.gaps.rend() && gaps.size() < 4; ++it) {
            if (it->filled)
                continue;
            gaps.append(QJsonObject{{QStringLiteral("direction"), direction(it->bullish)},
                                    {QStringLiteral("top"), rounded(it->top)},
                                    {QStringLiteral("bottom"), rounded(it->bottom)},
                                    {QStringLiteral("candles_ago"), ago(it->index)}});
        }
        // Only fresh sweeps, newest last: on a real market old levels are
        // wicked through all the time.
        const size_t fresh = size_t(std::min(Smc::recentSweeps(smc, kRecentCandles), 3));
        for (size_t i = smc.sweeps.size() - fresh; i < smc.sweeps.size(); ++i) {
            const Smc::Sweep &s = smc.sweeps[i];
            sweeps.append(QJsonObject{{QStringLiteral("direction"), direction(s.bullish)},
                                      {QStringLiteral("level"), rounded(s.level)},
                                      {QStringLiteral("candles_ago"), ago(s.index)}});
        }
        structure.insert(QStringLiteral("events"), events);
        structure.insert(QStringLiteral("unmitigated_order_blocks"), blocks);
        structure.insert(QStringLiteral("unfilled_fair_value_gaps"), gaps);
        structure.insert(QStringLiteral("liquidity_sweeps"), sweeps);
    }

    QJsonObject root{
        {QStringLiteral("symbol"), m_config->symbol()},
        {QStringLiteral("interval"), m_config->interval()},
        {QStringLiteral("price"), rounded(m_candles.empty() ? 0 : m_candles.back().close)},
        {QStringLiteral("candles"), QJsonObject{{QStringLiteral("columns"), QJsonArray{QStringLiteral("time_utc"),
                                                    QStringLiteral("open"), QStringLiteral("high"), QStringLiteral("low"),
                                                    QStringLiteral("close"), QStringLiteral("volume")}},
                                                {QStringLiteral("rows"), rows}}},
        {QStringLiteral("indicators"), indicators},
        {QStringLiteral("structure"), structure},
        {QStringLiteral("min_reward_risk"), m_config->risk().minRewardRisk},
    };
    if (higher && higher->valid) {
        root.insert(QStringLiteral("higher_timeframe"), QJsonObject{
            {QStringLiteral("interval"), TradingConfig::higherInterval(m_config->interval())},
            {QStringLiteral("trend"), qs(higher->trend)}, {QStringLiteral("rsi14"), numberOrNull(higher->rsi14)},
            {QStringLiteral("ema20"), numberOrNull(higher->ema20)}, {QStringLiteral("ema50"), numberOrNull(higher->ema50)},
            {QStringLiteral("macd_state"), qs(higher->macdState)}});
    }
    if (const Position *p = m_account.positionOn(m_config->symbol().toStdString())) {
        root.insert(QStringLiteral("open_position"), QJsonObject{
            {QStringLiteral("side"), p->isLong ? QStringLiteral("long") : QStringLiteral("short")},
            {QStringLiteral("entry"), rounded(p->entry)}, {QStringLiteral("stop_loss"), rounded(p->stop)},
            {QStringLiteral("take_profit"), rounded(p->takeProfit)}});
    }
    return root;
}

Verdict TradingEngine::parseVerdict(const QString &source, const QString &text)
{
    Verdict v;
    v.source = source.toStdString();
    // A reasoning model may think aloud in <think> tags first; that is not
    // the answer and can contain braces of its own.
    QString answer = text;
    static const QRegularExpression thinking(QStringLiteral("<think>.*?</think>"),
                                             QRegularExpression::DotMatchesEverythingOption
                                                 | QRegularExpression::CaseInsensitiveOption);
    answer.remove(thinking);
    const qsizetype unclosed = answer.indexOf(QLatin1String("<think>"), 0, Qt::CaseInsensitive);
    if (unclosed >= 0)
        answer.truncate(unclosed); // thinking that never ended: nothing after it is an answer
    // Models wrap the object in prose or a code fence often enough: take what
    // is between the first '{' and the last '}'.
    const qsizetype from = answer.indexOf(QLatin1Char('{'));
    const qsizetype to = answer.lastIndexOf(QLatin1Char('}'));
    const QJsonDocument doc = from >= 0 && to > from ? QJsonDocument::fromJson(answer.mid(from, to - from + 1).toUtf8())
                                                     : QJsonDocument();
    if (!doc.isObject()) {
        v.error = "The answer was not the JSON object that was asked for.";
        return v;
    }
    const QJsonObject o = doc.object();
    const QString action = o.value(QLatin1String("action")).toString().trimmed().toLower();
    static const QStringList holds = {QStringLiteral("hold"), QStringLiteral("wait"), QStringLiteral("none"),
                                      QStringLiteral("neutral"), QStringLiteral("no trade"), QStringLiteral("flat")};
    v.action = actionFromName(action.toStdString());
    if (v.action == Action::Hold && !holds.contains(action)) {
        v.error = "The answer names no action (buy, sell or hold).";
        return v;
    }
    const double confidence = looseNumber(o.value(QLatin1String("confidence")));
    v.confidence = std::isfinite(confidence) ? std::clamp(confidence, 0.0, 100.0) : 0.0;
    const double stop = looseNumber(o.value(QLatin1String("stop_loss")));
    const double target = looseNumber(o.value(QLatin1String("take_profit")));
    v.stop = std::isfinite(stop) && stop > 0 ? stop : 0;
    v.takeProfit = std::isfinite(target) && target > 0 ? target : 0;
    if (v.action == Action::Hold) {
        v.confidence = 0;
        v.stop = 0;
        v.takeProfit = 0;
    }
    QString reasoning = o.value(QLatin1String("reasoning")).toString().simplified();
    if (reasoning.size() > 600)
        reasoning = reasoning.left(600) + QStringLiteral("…");
    v.reasoning = reasoning.toStdString();
    v.ok = true;
    return v;
}

bool TradingEngine::analyzeNow()
{
    if (m_analyzing)
        return false;
    m_ta = Indicators::summarize(m_candles);
    if (!m_ta.valid) {
        log(QStringLiteral("info"), tr("Not enough price history to analyze %1 yet.").arg(symbolName()));
        return false;
    }
    m_smc = Smc::analyze(m_candles);
    const QString key = m_config->symbol() + QLatin1Char(':') + TradingConfig::higherInterval(m_config->interval());
    Indicators::Summary higher;
    const bool haveHigher = m_higherFor == key;
    if (haveHigher)
        higher = Indicators::summarize(m_higherCandles);

    ++m_generation;
    m_analyzing = true;
    m_analysisSymbol = m_config->symbol();
    m_analysisInterval = m_config->interval();
    m_verdicts.clear();
    m_waiting.clear();
    m_verdicts.push_back(rulesVerdict(m_ta, haveHigher ? &higher : nullptr, m_smc, m_config->risk().minRewardRisk));

    const QString user = QStringLiteral("Market snapshot:\n")
        + QString::fromUtf8(QJsonDocument(snapshot(m_ta, haveHigher ? &higher : nullptr, m_smc))
                                .toJson(QJsonDocument::Compact));
    QByteArray picture; // rendered once, and only if a model that reads pictures takes part
    const bool haveKey = !m_config->apiKey().isEmpty();
    const QList<TradingConfig::Member> panel = m_config->panel();
    for (const TradingConfig::Member &member : panel) {
        if (!haveKey || !member.enabled || !m_models->offered(member.model))
            continue;
        if (m_models->busy(member.model)) { // being tested from the Model Panel
            Verdict v;
            v.source = member.model.toStdString();
            v.error = "Busy with a connection test.";
            m_verdicts.push_back(v);
            continue;
        }
        if (member.vision && picture.isEmpty())
            picture = ChartRenderer::renderPng(m_candles, displayName(m_analysisSymbol) + QLatin1Char(' ')
                                                              + m_analysisInterval);
        ModelClient::Question q;
        q.model = member.model;
        q.system = systemPrompt(member.role, m_config->risk().minRewardRisk);
        q.user = user;
        if (member.vision)
            q.imagePng = picture;
        m_waiting.insert(member.model);
        m_analystState[member.model] = {QStringLiteral("working"), QString(), 0};
        m_models->ask(q);
    }

    m_signal.insert(QStringLiteral("state"), QStringLiteral("analyzing"));
    Q_EMIT signalChanged();
    Q_EMIT analystsChanged();
    if (m_waiting.isEmpty()) {
        const int generation = m_generation;
        QTimer::singleShot(0, this, [this, generation] {
            if (generation == m_generation && m_analyzing)
                finishAnalysis();
        });
    }
    return true;
}

void TradingEngine::onModelAnswered(const QString &id, const QString &text, qint64 ms)
{
    if (m_testing.remove(id)) {
        m_analystState[id] = {QStringLiteral("active"), QString(), ms};
        Q_EMIT modelTested(id, true, tr("Answered in %1 s.").arg(double(ms) / 1000.0, 0, 'f', 1));
        Q_EMIT analystsChanged();
        return;
    }
    if (!m_analyzing || !m_waiting.contains(id))
        return;
    Verdict v = parseVerdict(id, text);
    v.ms = ms;
    m_analystState[id] = {v.ok ? QStringLiteral("active") : QStringLiteral("error"), qs(v.error), ms};
    collect(v);
}

void TradingEngine::onModelFailed(const QString &id, const QString &code, const QString &message, qint64 ms)
{
    if (m_testing.remove(id)) {
        m_analystState[id] = {QStringLiteral("error"), message, ms};
        Q_EMIT modelTested(id, false, message);
        Q_EMIT analystsChanged();
        return;
    }
    if (!m_analyzing || !m_waiting.contains(id))
        return;
    Q_UNUSED(code)
    Verdict v;
    v.source = id.toStdString();
    v.error = message.toStdString();
    v.ms = ms;
    m_analystState[id] = {QStringLiteral("error"), message, ms};
    collect(v);
}

void TradingEngine::collect(const Verdict &verdict)
{
    m_waiting.remove(qs(verdict.source));
    m_verdicts.push_back(verdict);
    Q_EMIT analystsChanged();
    if (m_waiting.isEmpty())
        finishAnalysis();
}

AccountView TradingEngine::accountView(const QString &symbol, qint64 now) const
{
    AccountView view;
    view.equity = m_account.equity();
    view.available = m_account.available();
    view.dayStartEquity = m_account.dayStartEquity();
    view.peakEquity = m_account.peakEquity();
    view.realizedToday = m_account.realizedOn(PaperAccount::dayNumber(now));
    view.openPositions = int(m_account.positions().size());
    view.positionOnSymbol = m_account.positionOn(symbol.toStdString()) != nullptr;
    view.lastLossOnSymbolMs = m_account.lastLossOn(symbol.toStdString());
    view.halted = m_halted;
    return view;
}

void TradingEngine::finishAnalysis()
{
    m_analyzing = false;
    if (m_candles.empty() || m_analysisSymbol != m_config->symbol()) {
        Q_EMIT signalChanged();
        return;
    }
    const RiskSettings risk = m_config->risk();
    const qint64 now = nowMs();
    ConsensusInput input;
    input.price = m_candles.back().close; // the price now, not when the models were asked
    input.atr = m_ta.atr14;
    input.minRewardRisk = risk.minRewardRisk;
    input.minAgreeing = risk.minAgreeing;
    Signal signal = consensus(m_verdicts, input);
    const std::string symbol = m_analysisSymbol.toStdString();

    QString outcome;
    bool traded = false;
    // An open position that the analysts now clearly oppose is closed.
    if (m_config->autoTrade() && signal.action != Action::Hold && signal.confidence >= risk.minConfidence) {
        const Position *open = m_account.positionOn(symbol);
        if (open && open->isLong != (signal.action == Action::Buy)) {
            Trade closed;
            if (m_account.close(open->id, input.price, now, "signal_flip", &closed)) {
                recordClosed({closed});
                afterAccountChange(true);
            }
        }
    }
    RiskDecision decision = evaluate(signal, accountView(m_analysisSymbol, now), risk, now);

    const QString headline = signal.action == Action::Hold
        ? tr("%1 %2: no trade. %3").arg(symbolName(), m_analysisInterval, qs(signal.note))
        : tr("%1 %2: %3 signal, confidence %4 (%5 of %6 analysts).")
              .arg(symbolName(), m_analysisInterval, signal.action == Action::Buy ? tr("BUY") : tr("SELL"))
              .arg(signal.confidence, 0, 'f', 1).arg(signal.agreeing).arg(signal.voters);
    log(QStringLiteral("signal"), headline);

    if (signal.action != Action::Hold) {
        if (!decision.allowed) {
            outcome = tr("Not traded. %1").arg(qs(decision.reason));
            log(QStringLiteral("risk"), outcome);
        } else if (m_config->autoTrade()) {
            const QString error = execute(signal, decision);
            traded = error.isEmpty();
            outcome = traded ? tr("Placed on the paper account.") : tr("Not placed: %1").arg(error);
            if (traded) // the signal now shows the account as it is with the trade in it
                decision = evaluate(signal, accountView(m_analysisSymbol, now), risk, now);
        } else {
            outcome = tr("Passes the risk rules. Auto trade is off, so nothing was placed.");
        }
    }

    m_lastSignal = signal;
    m_lastDecision = decision;
    m_signal = signalMap(signal, decision);
    m_signal.insert(QStringLiteral("state"), QStringLiteral("ready"));
    m_signal.insert(QStringLiteral("time"), double(now));
    m_signal.insert(QStringLiteral("symbol"), m_analysisSymbol);
    m_signal.insert(QStringLiteral("symbolName"), displayName(m_analysisSymbol));
    m_signal.insert(QStringLiteral("interval"), m_analysisInterval);
    m_signal.insert(QStringLiteral("traded"), traded);
    m_signal.insert(QStringLiteral("outcome"), outcome);
    // Whether the "place this trade" button makes sense.
    m_signal.insert(QStringLiteral("placeable"), signal.action != Action::Hold && !traded && decision.allowed);
    Q_EMIT signalChanged();
    Q_EMIT analystsChanged();
    Q_EMIT analysisFinished();
}

QString TradingEngine::execute(const Signal &signal, const RiskDecision &decision)
{
    const qint64 now = nowMs();
    const PaperAccount::OpenResult result = m_account.open(
        m_analysisSymbol.toStdString(), signal.action == Action::Buy, decision.quantity, signal.entry, signal.stop,
        signal.takeProfit, now, signal.confidence, signal.lead, signal.reasoning);
    if (!result.ok) {
        log(QStringLiteral("error"), tr("Order refused: %1").arg(qs(result.error)));
        return qs(result.error);
    }
    const Position &p = result.position;
    log(QStringLiteral("trade"),
        tr("Opened %1 %2: %3 at %4, stop %5, target %6 (paper).")
            .arg(p.isLong ? tr("long") : tr("short"), displayName(qs(p.symbol)))
            .arg(p.quantity, 0, 'g', 6).arg(p.entry, 0, 'g', 8).arg(p.stop, 0, 'g', 8).arg(p.takeProfit, 0, 'g', 8));
    afterAccountChange(true);
    return {};
}

QString TradingEngine::placeSuggestedTrade()
{
    if (m_analyzing)
        return tr("An analysis is running.");
    if (m_lastSignal.action == Action::Hold || m_analysisSymbol != m_config->symbol() || m_candles.empty())
        return tr("There is no trade signal for this market.");
    const qint64 now = nowMs();
    const qint64 age = now - qint64(m_signal.value(QStringLiteral("time")).toDouble());
    if (age > 2 * TradingConfig::intervalMs(m_analysisInterval))
        return tr("The signal is out of date. Analyze again.");
    // The order goes in at the price now, so the rules are applied to it.
    Signal signal = m_lastSignal;
    signal.entry = m_candles.back().close;
    signal.rewardRisk = rewardRisk(signal.action, signal.entry, signal.stop, signal.takeProfit);
    const RiskDecision decision = evaluate(signal, accountView(m_analysisSymbol, now), m_config->risk(), now);
    if (!decision.allowed)
        return qs(decision.reason);
    const QString error = execute(signal, decision);
    if (error.isEmpty()) {
        m_signal.insert(QStringLiteral("traded"), true);
        m_signal.insert(QStringLiteral("placeable"), false);
        m_signal.insert(QStringLiteral("outcome"), tr("Placed on the paper account."));
        Q_EMIT signalChanged();
    }
    return error;
}

bool TradingEngine::closePosition(qlonglong id)
{
    for (const Position &p : m_account.positions()) {
        if (p.id != id)
            continue;
        const QString symbol = qs(p.symbol);
        double price = p.lastPrice;
        if (symbol == m_config->symbol() && !m_candles.empty())
            price = m_candles.back().close;
        else if (m_tickers.contains(symbol))
            price = m_tickers.value(symbol).price;
        Trade closed;
        if (!m_account.close(id, price, nowMs(), "manual", &closed))
            return false;
        recordClosed({closed});
        afterAccountChange(true);
        return true;
    }
    return false;
}

void TradingEngine::resetPaperAccount()
{
    m_account.reset(m_config->paper());
    m_halted = false;
    m_equityCurve.clear();
    m_lastCurvePoint = 0;
    log(QStringLiteral("info"), tr("Paper account reset to %1 USDT.").arg(m_config->paper().startingBalance, 0, 'f', 2));
    afterAccountChange(true);
}

void TradingEngine::resumeTrading()
{
    if (!m_halted)
        return;
    m_halted = false;
    // The peak starts again from here; otherwise the limit would halt
    // trading again on the very next price.
    PaperAccount::State state = m_account.state();
    state.peakEquity = m_account.equity();
    m_account.restore(state);
    log(QStringLiteral("risk"), tr("Trading resumed by you. Drawdown is measured from the current equity."));
    save();
    Q_EMIT accountChanged();
}

// ---- settings and providers -------------------------------------------------------

bool TradingEngine::setRiskSetting(const QString &name, double value)
{
    return m_config->setRisk(name, value);
}

bool TradingEngine::setPaperSetting(const QString &name, double value)
{
    return m_config->setPaper(name, value);
}

void TradingEngine::setModelEnabled(const QString &model, bool enabled)
{
    m_config->setMemberEnabled(model, enabled);
}

QString TradingEngine::addModel(const QString &model, const QString &role)
{
    return m_config->addMember(model, role);
}

void TradingEngine::removeModel(const QString &model)
{
    m_config->removeMember(model);
    m_analystState.remove(model);
    Q_EMIT catalogChanged();
}

void TradingEngine::resetPanel()
{
    m_config->resetPanel();
    Q_EMIT catalogChanged();
}

QString TradingEngine::saveKey(const QString &key)
{
    const QString error = m_config->saveKey(key);
    if (error.isEmpty())
        m_models->listModels();
    return error;
}

void TradingEngine::removeKey()
{
    m_config->removeKey();
}

void TradingEngine::testModel(const QString &model)
{
    if (m_config->member(model).model.isEmpty())
        return;
    if (m_models->busy(model) || m_testing.contains(model)) {
        Q_EMIT modelTested(model, false, tr("Still busy with the previous question."));
        return;
    }
    ModelClient::Question q;
    q.model = model;
    q.system = QStringLiteral("This is a connection test. Answer with exactly this JSON object and nothing else: "
                              "{\"action\": \"hold\", \"confidence\": 0, \"stop_loss\": 0, \"take_profit\": 0, "
                              "\"reasoning\": \"ok\"}");
    q.user = QStringLiteral("Connection test.");
    q.timeoutMs = 60000;
    m_testing.insert(model);
    m_analystState[model] = {QStringLiteral("working"), QString(), 0};
    Q_EMIT analystsChanged();
    m_models->ask(q);
}

void TradingEngine::testAll()
{
    const QList<TradingConfig::Member> panel = m_config->panel();
    for (const TradingConfig::Member &member : panel) {
        if (member.enabled && m_models->offered(member.model) && !m_models->busy(member.model))
            testModel(member.model);
    }
}

void TradingEngine::refreshCatalog()
{
    m_models->listModels();
}

QStringList TradingEngine::catalog() const
{
    // What could be added: the service's list without what is on the panel and
    // without the models that are plainly not chat models.
    static const QRegularExpression notChat(QStringLiteral("embed|guard|safety|clip|detector|translate|gliner|"
                                                           "calibration|starcoder|retriev"));
    QStringList list;
    const QStringList all = m_models->catalog();
    for (const QString &id : all) {
        if (m_config->member(id).model.isEmpty() && !notChat.match(id).hasMatch())
            list.append(id);
    }
    return list;
}

QVariantMap TradingEngine::key() const
{
    return {{QStringLiteral("hasKey"), !m_config->apiKey().isEmpty()},
            {QStringLiteral("hint"), m_config->keyHint()},
            {QStringLiteral("source"), m_config->keySource()}};
}

QVariantMap TradingEngine::riskSettings() const
{
    const RiskSettings r = m_config->risk();
    return {{QStringLiteral("risk_per_trade_pct"), r.riskPerTradePct},
            {QStringLiteral("max_daily_loss_pct"), r.maxDailyLossPct},
            {QStringLiteral("max_drawdown_pct"), r.maxDrawdownPct},
            {QStringLiteral("min_reward_risk"), r.minRewardRisk},
            {QStringLiteral("min_confidence"), r.minConfidence},
            {QStringLiteral("min_agreeing"), r.minAgreeing},
            {QStringLiteral("max_open_positions"), r.maxOpenPositions},
            {QStringLiteral("cooldown_minutes"), r.cooldownMinutes}};
}

QVariantMap TradingEngine::paperSettings() const
{
    const PaperSettings p = m_config->paper();
    return {{QStringLiteral("starting_balance"), p.startingBalance},
            {QStringLiteral("fee_pct"), p.feePct},
            {QStringLiteral("slippage_pct"), p.slippagePct}};
}

QVariantList TradingEngine::analysts() const
{
    // Three kinds of entry: every model of the panel ("model"), one summary
    // per role for the desk ("role"), and the four built-in skills ("skill").
    QVariantList list;
    const bool hasKey = !m_config->apiKey().isEmpty();
    const QList<TradingConfig::Member> panel = m_config->panel();
    struct RoleCount { int total = 0, enabled = 0, active = 0, working = 0, failed = 0; QString lead; QStringList lines; };
    QHash<QString, RoleCount> counts;
    for (const TradingConfig::Member &member : panel) {
        const AnalystState known = m_analystState.value(member.model);
        const bool offered = m_models->offered(member.model);
        const QString state = !member.enabled ? QStringLiteral("off") : !hasKey ? QStringLiteral("no_key")
            : !offered ? QStringLiteral("unavailable")
            : known.state.isEmpty() ? QStringLiteral("ready") : known.state;
        const QString detail = !offered ? tr("NVIDIA does not list this model (any more).") : known.detail;
        list.append(QVariantMap{
            {QStringLiteral("kind"), QStringLiteral("model")}, {QStringLiteral("id"), member.model},
            {QStringLiteral("model"), member.model}, {QStringLiteral("name"), TradingConfig::displayName(member.model)},
            {QStringLiteral("shortName"), TradingConfig::shortName(member.model)},
            {QStringLiteral("publisher"), TradingConfig::publisher(member.model)},
            {QStringLiteral("role"), member.role}, {QStringLiteral("vision"), member.vision},
            {QStringLiteral("enabled"), member.enabled}, {QStringLiteral("hasKey"), hasKey},
            {QStringLiteral("state"), state}, {QStringLiteral("detail"), detail},
            {QStringLiteral("seconds"), double(known.ms) / 1000.0}});
        RoleCount &count = counts[member.role];
        ++count.total;
        if (count.lead.isEmpty() && member.enabled)
            count.lead = TradingConfig::shortName(member.model);
        if (member.enabled && offered) {
            ++count.enabled;
            count.active += state == QLatin1String("active") ? 1 : 0;
            count.working += state == QLatin1String("working") ? 1 : 0;
            count.failed += state == QLatin1String("error") ? 1 : 0;
        }
        count.lines.append(TradingConfig::displayName(member.model) + QStringLiteral(": ") + state
                           + (detail.isEmpty() ? QString() : QStringLiteral(" (") + detail + QLatin1Char(')')));
    }
    const QStringList roleNames = TradingConfig::roles();
    for (const QString &role : roleNames) {
        const RoleCount count = counts.value(role);
        // "ready": a key and models, nothing asked yet. "active": at least one answered last time.
        const QString state = count.enabled == 0 ? QStringLiteral("off") : !hasKey ? QStringLiteral("no_key")
            : count.working > 0 ? QStringLiteral("working")
            : count.active > 0 ? QStringLiteral("active")
            : count.failed > 0 ? QStringLiteral("error") : QStringLiteral("ready");
        list.append(QVariantMap{
            {QStringLiteral("kind"), QStringLiteral("role")}, {QStringLiteral("id"), role},
            {QStringLiteral("role"), role}, {QStringLiteral("lead"), count.lead},
            {QStringLiteral("models"), count.enabled}, {QStringLiteral("total"), count.total},
            {QStringLiteral("answered"), count.active}, {QStringLiteral("failed"), count.failed},
            {QStringLiteral("state"), state}, {QStringLiteral("detail"), count.lines.join(QLatin1Char('\n'))},
            {QStringLiteral("skills"), skillsOf(role)}});
    }
    const int closed = closedCount(m_candles);
    const auto skill = [&list](const char *id, const QString &state, const QString &detail) {
        list.append(QVariantMap{{QStringLiteral("id"), QLatin1String(id)}, {QStringLiteral("kind"), QStringLiteral("skill")},
                                {QStringLiteral("state"), state}, {QStringLiteral("detail"), detail}});
    };
    const QString waiting = tr("Waiting for price history (%1 candles so far).").arg(closed);
    skill("ta", closed >= 60 ? QStringLiteral("active") : QStringLiteral("waiting"), closed >= 60 ? QString() : waiting);
    skill("smc", closed >= 30 ? QStringLiteral("active") : QStringLiteral("waiting"), closed >= 30 ? QString() : waiting);
    skill("risk", m_halted ? QStringLiteral("halted") : QStringLiteral("active"),
          m_halted ? tr("Trading is halted by the drawdown limit.") : QString());
    skill("execution", QStringLiteral("active"), tr("Paper account"));
    return list;
}

// ---- what QML reads ------------------------------------------------------------------

QVariantList TradingEngine::watchlist() const
{
    QVariantList list;
    const QStringList symbols = m_config->watchlist();
    for (const QString &symbol : symbols) {
        const bool known = m_tickers.contains(symbol);
        const MarketData::Ticker t = m_tickers.value(symbol);
        list.append(QVariantMap{{QStringLiteral("symbol"), symbol}, {QStringLiteral("name"), displayName(symbol)},
                                {QStringLiteral("hasPrice"), known}, {QStringLiteral("price"), t.price},
                                {QStringLiteral("listed"), !m_data->isUnlisted(symbol)},
                                {QStringLiteral("changePct"), t.changePct},
                                {QStringLiteral("spark"), m_sparks.value(symbol)},
                                {QStringLiteral("selected"), symbol == m_config->symbol()}});
    }
    return list;
}

QVariantMap TradingEngine::quote() const
{
    if (m_candles.empty())
        return {};
    const Candle &c = m_candles.back();
    return {{QStringLiteral("time"), double(c.openTime)}, {QStringLiteral("open"), c.open},
            {QStringLiteral("high"), c.high}, {QStringLiteral("low"), c.low}, {QStringLiteral("close"), c.close},
            {QStringLiteral("change"), c.close - c.open},
            {QStringLiteral("changePct"), c.open > 0 ? (c.close - c.open) / c.open * 100.0 : 0.0},
            {QStringLiteral("closed"), c.closed}, {QStringLiteral("candles"), int(m_candles.size())}};
}

QVariantList TradingEngine::closes() const
{
    QVariantList list;
    const size_t count = std::min<size_t>(m_candles.size(), 48);
    for (size_t i = m_candles.size() - count; i < m_candles.size(); ++i)
        list.append(m_candles[i].close);
    return list;
}

QVariantMap TradingEngine::readings() const
{
    const Indicators::Summary ta = Indicators::summarize(m_candles);
    const Smc::Analysis smc = Smc::analyze(m_candles);
    QVariantMap map{{QStringLiteral("taValid"), ta.valid}, {QStringLiteral("taReason"), qs(ta.reason)},
                    {QStringLiteral("smcValid"), smc.valid}, {QStringLiteral("smcReason"), qs(smc.reason)}};
    const auto value = [](double v) { return std::isfinite(v) ? QVariant(v) : QVariant(); };
    if (ta.valid) {
        map.insert(QStringLiteral("price"), ta.price);
        map.insert(QStringLiteral("ema20"), value(ta.ema20));
        map.insert(QStringLiteral("ema50"), value(ta.ema50));
        map.insert(QStringLiteral("ema200"), value(ta.ema200));
        map.insert(QStringLiteral("trend"), qs(ta.trend));
        map.insert(QStringLiteral("rsi14"), value(ta.rsi14));
        map.insert(QStringLiteral("momentum"), qs(ta.momentum));
        map.insert(QStringLiteral("macdHistogram"), value(ta.macdHistogram));
        map.insert(QStringLiteral("macdState"), qs(ta.macdState));
        map.insert(QStringLiteral("bandPosition"), value(ta.bandPosition));
        map.insert(QStringLiteral("bandWidthPct"), value(ta.bandWidthPct));
        map.insert(QStringLiteral("atr14"), value(ta.atr14));
        map.insert(QStringLiteral("atrPct"), value(ta.atrPct));
        map.insert(QStringLiteral("volumeRatio"), value(ta.volumeRatio));
        map.insert(QStringLiteral("volumeState"), qs(ta.volumeState));
    }
    if (smc.valid) {
        map.insert(QStringLiteral("structureTrend"), QLatin1String(Smc::trendName(smc.trend)));
        map.insert(QStringLiteral("zone"), qs(smc.zone));
        map.insert(QStringLiteral("rangeHigh"), smc.rangeHigh);
        map.insert(QStringLiteral("rangeLow"), smc.rangeLow);
        int openBlocks = 0, openGaps = 0;
        for (const Smc::OrderBlock &b : smc.orderBlocks)
            openBlocks += b.mitigated ? 0 : 1;
        for (const Smc::FairValueGap &g : smc.gaps)
            openGaps += g.filled ? 0 : 1;
        map.insert(QStringLiteral("orderBlocks"), openBlocks);
        map.insert(QStringLiteral("fairValueGaps"), openGaps);
        map.insert(QStringLiteral("sweeps"), Smc::recentSweeps(smc, kRecentCandles));
        if (!smc.events.empty()) {
            const Smc::StructureEvent &e = smc.events.back();
            map.insert(QStringLiteral("lastEvent"), e.changeOfCharacter ? QStringLiteral("choch") : QStringLiteral("bos"));
            map.insert(QStringLiteral("lastEventBullish"), e.bullish);
            map.insert(QStringLiteral("lastEventLevel"), e.level);
        }
    }
    return map;
}

QVariantMap TradingEngine::signalMap(const Signal &signal, const RiskDecision &decision) const
{
    QVariantList verdicts;
    for (const Verdict &v : signal.verdicts) {
        const QString id = qs(v.source);
        const bool rules = id == QLatin1String("rules");
        verdicts.append(QVariantMap{
            {QStringLiteral("source"), id},
            {QStringLiteral("name"), rules ? tr("Rules") : TradingConfig::displayName(id)},
            {QStringLiteral("role"), rules ? QStringLiteral("rules") : m_config->member(id).role},
            {QStringLiteral("ok"), v.ok}, {QStringLiteral("error"), qs(v.error)},
            {QStringLiteral("action"), QLatin1String(actionName(v.action))},
            {QStringLiteral("confidence"), v.confidence}, {QStringLiteral("stop"), v.stop},
            {QStringLiteral("takeProfit"), v.takeProfit}, {QStringLiteral("reasoning"), qs(v.reasoning)},
            {QStringLiteral("seconds"), double(v.ms) / 1000.0}});
    }
    QVariantList checks;
    for (const RiskCheck &c : decision.checks) {
        checks.append(QVariantMap{{QStringLiteral("key"), qs(c.key)}, {QStringLiteral("label"), qs(c.label)},
                                  {QStringLiteral("pass"), c.pass}, {QStringLiteral("detail"), qs(c.detail)}});
    }
    return {
        {QStringLiteral("action"), QLatin1String(actionName(signal.action))},
        {QStringLiteral("leaning"), QLatin1String(actionName(signal.leaning))},
        {QStringLiteral("confidence"), signal.confidence},
        {QStringLiteral("entry"), signal.entry}, {QStringLiteral("stop"), signal.stop},
        {QStringLiteral("takeProfit"), signal.takeProfit}, {QStringLiteral("rewardRisk"), signal.rewardRisk},
        {QStringLiteral("voters"), signal.voters}, {QStringLiteral("agreeing"), signal.agreeing},
        {QStringLiteral("opposing"), signal.opposing}, {QStringLiteral("holding"), signal.holding},
        {QStringLiteral("lead"), qs(signal.lead)},
        {QStringLiteral("leadName"), signal.lead.empty() ? QString()
                                         : signal.lead == "rules" ? tr("Rules") : TradingConfig::displayName(qs(signal.lead))},
        {QStringLiteral("reasoning"), qs(signal.reasoning)}, {QStringLiteral("note"), qs(signal.note)},
        {QStringLiteral("verdicts"), verdicts},
        {QStringLiteral("risk"), QVariantMap{
            {QStringLiteral("allowed"), decision.allowed}, {QStringLiteral("reason"), qs(decision.reason)},
            {QStringLiteral("quantity"), decision.quantity}, {QStringLiteral("notional"), decision.notional},
            {QStringLiteral("riskAmount"), decision.riskAmount},
            {QStringLiteral("reducedByBalance"), decision.reducedByBalance}, {QStringLiteral("checks"), checks}}},
    };
}

QVariantMap TradingEngine::account() const
{
    const PaperStats s = m_account.stats();
    const double equity = m_account.equity();
    const double starting = m_account.settings().startingBalance;
    const AccountView view = accountView(m_config->symbol(), nowMs());
    double openRisk = 0;
    for (const Position &p : m_account.positions())
        openRisk += std::fabs(p.entry - p.stop) * p.quantity;
    const double dayStart = m_account.dayStartEquity();
    return {
        {QStringLiteral("equity"), equity}, {QStringLiteral("balance"), m_account.balance()},
        {QStringLiteral("available"), m_account.available()}, {QStringLiteral("unrealized"), m_account.unrealized()},
        {QStringLiteral("startingBalance"), starting},
        {QStringLiteral("totalPnl"), equity - starting},
        {QStringLiteral("totalPnlPct"), starting > 0 ? (equity - starting) / starting * 100.0 : 0.0},
        {QStringLiteral("realizedPnl"), s.totalPnl},
        {QStringLiteral("todayPnl"), equity - dayStart},
        {QStringLiteral("todayPnlPct"), dayStart > 0 ? (equity - dayStart) / dayStart * 100.0 : 0.0},
        {QStringLiteral("trades"), s.trades}, {QStringLiteral("wins"), s.wins}, {QStringLiteral("losses"), s.losses},
        {QStringLiteral("winRatePct"), s.winRatePct}, {QStringLiteral("profitFactor"), s.profitFactor},
        {QStringLiteral("bestTrade"), s.bestTrade}, {QStringLiteral("worstTrade"), s.worstTrade},
        {QStringLiteral("averageWin"), s.averageWin}, {QStringLiteral("averageLoss"), s.averageLoss},
        {QStringLiteral("drawdownPct"), drawdownPct(equity, m_account.peakEquity())},
        {QStringLiteral("maxDrawdownPct"), s.maxDrawdownPct},
        {QStringLiteral("peakEquity"), m_account.peakEquity()},
        {QStringLiteral("openPositions"), int(m_account.positions().size())},
        {QStringLiteral("openRisk"), openRisk},
        {QStringLiteral("openRiskPct"), equity > 0 ? openRisk / equity * 100.0 : 0.0},
        {QStringLiteral("halted"), m_halted},
        {QStringLiteral("dailyLossReached"), dailyLossReached(view, m_config->risk())},
    };
}

QVariantList TradingEngine::positions() const
{
    QVariantList list;
    for (const Position &p : m_account.positions()) {
        const double price = p.lastPrice > 0 ? p.lastPrice : p.entry;
        const double pnl = (p.isLong ? price - p.entry : p.entry - price) * p.quantity;
        list.append(QVariantMap{
            {QStringLiteral("id"), qlonglong(p.id)}, {QStringLiteral("symbol"), qs(p.symbol)},
            {QStringLiteral("name"), displayName(qs(p.symbol))},
            {QStringLiteral("side"), p.isLong ? QStringLiteral("buy") : QStringLiteral("sell")},
            {QStringLiteral("quantity"), p.quantity}, {QStringLiteral("entry"), p.entry},
            {QStringLiteral("stop"), p.stop}, {QStringLiteral("takeProfit"), p.takeProfit},
            {QStringLiteral("price"), price}, {QStringLiteral("pnl"), pnl},
            {QStringLiteral("pnlPct"), pnl / (p.entry * p.quantity) * 100.0},
            {QStringLiteral("openedAt"), double(p.openedAt)}, {QStringLiteral("confidence"), p.confidence},
            {QStringLiteral("lead"), qs(p.lead)}, {QStringLiteral("reasoning"), qs(p.reasoning)}});
    }
    return list;
}

QVariantList TradingEngine::trades() const
{
    QVariantList list;
    const std::vector<Trade> &all = m_account.trades();
    for (auto it = all.rbegin(); it != all.rend(); ++it) {
        const Trade &t = *it;
        list.append(QVariantMap{
            {QStringLiteral("id"), qlonglong(t.id)}, {QStringLiteral("symbol"), qs(t.symbol)},
            {QStringLiteral("name"), displayName(qs(t.symbol))},
            {QStringLiteral("side"), t.isLong ? QStringLiteral("buy") : QStringLiteral("sell")},
            {QStringLiteral("quantity"), t.quantity}, {QStringLiteral("entry"), t.entry},
            {QStringLiteral("exit"), t.exit}, {QStringLiteral("openedAt"), double(t.openedAt)},
            {QStringLiteral("closedAt"), double(t.closedAt)}, {QStringLiteral("fees"), t.fees},
            {QStringLiteral("pnl"), t.pnl}, {QStringLiteral("pnlPct"), t.pnlPct},
            {QStringLiteral("reason"), qs(t.reason)}, {QStringLiteral("confidence"), t.confidence},
            {QStringLiteral("lead"), qs(t.lead)}, {QStringLiteral("reasoning"), qs(t.reasoning)}});
    }
    return list;
}

QVariantList TradingEngine::equityCurve() const
{
    // The stored points and the value right now.
    QVariantList curve = m_equityCurve;
    curve.append(QVariantMap{{QStringLiteral("t"), double(nowMs())}, {QStringLiteral("v"), m_account.equity()}});
    return curve;
}

void TradingEngine::log(const QString &kind, const QString &text)
{
    m_activity.prepend(QVariantMap{{QStringLiteral("time"), double(nowMs())}, {QStringLiteral("kind"), kind},
                                   {QStringLiteral("text"), text}});
    while (m_activity.size() > kMaxActivity)
        m_activity.removeLast();
    Q_EMIT activityChanged();
}

QString TradingEngine::tradingViewPage(bool dark, const QString &offlineText) const
{
    // The symbol comes from the validated watchlist (letters and digits only)
    // and the interval from a fixed table: nothing here is free text.
    static const QHash<QString, QString> minutes = {
        {QStringLiteral("1m"), QStringLiteral("1")}, {QStringLiteral("5m"), QStringLiteral("5")},
        {QStringLiteral("15m"), QStringLiteral("15")}, {QStringLiteral("1h"), QStringLiteral("60")},
        {QStringLiteral("4h"), QStringLiteral("240")}, {QStringLiteral("1d"), QStringLiteral("D")}};
    const QJsonObject options{
        {QStringLiteral("autosize"), true},
        {QStringLiteral("symbol"), QStringLiteral("BINANCE:") + m_config->symbol()},
        {QStringLiteral("interval"), minutes.value(m_config->interval(), QStringLiteral("15"))},
        {QStringLiteral("timezone"), QStringLiteral("Etc/UTC")},
        {QStringLiteral("theme"), dark ? QStringLiteral("dark") : QStringLiteral("light")},
        {QStringLiteral("style"), QStringLiteral("1")},
        {QStringLiteral("locale"), QStringLiteral("en")},
        {QStringLiteral("allow_symbol_change"), false},
        {QStringLiteral("support_host"), QStringLiteral("https://www.tradingview.com")},
    };
    // The message is hidden unless TradingView's script fails to load.
    return QStringLiteral(
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<style>html,body{height:100%;margin:0;background:%1;color:%2;overflow:hidden;"
        "font:14px system-ui,sans-serif}"
        ".tradingview-widget-container,.tradingview-widget-container__widget{height:100%;width:100%}"
        "#offline{display:none;height:100%;align-items:center;justify-content:center;text-align:center;"
        "padding:0 32px}</style>"
        "</head><body><div id=\"offline\">%3</div><div class=\"tradingview-widget-container\">"
        "<div class=\"tradingview-widget-container__widget\"></div>"
        "<script type=\"text/javascript\" "
        "src=\"https://s3.tradingview.com/external-embedding/embed-widget-advanced-chart.js\" async "
        "onerror=\"document.getElementById('offline').style.display='flex'\">%4</script>"
        "</div></body></html>")
        .arg(dark ? QStringLiteral("#0f1117") : QStringLiteral("#ffffff"),
             dark ? QStringLiteral("#a9b1c2") : QStringLiteral("#4a5060"),
             offlineText.toHtmlEscaped(),
             QString::fromUtf8(QJsonDocument(options).toJson(QJsonDocument::Compact)));
}

// ---- persistence ----------------------------------------------------------------------

void TradingEngine::load()
{
    const QString path = m_config->dataDir() + QStringLiteral("/paper.json");
    QFile file(path);
    if (!file.exists())
        return;
    if (!file.open(QIODevice::ReadOnly)) {
        log(QStringLiteral("error"), tr("The saved paper account could not be opened; starting a fresh one."));
        return;
    }
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    PaperAccount::State state;
    const QJsonObject settings = root.value(QLatin1String("settings")).toObject();
    state.settings.startingBalance = settings.value(QLatin1String("starting_balance")).toDouble();
    state.settings.feePct = m_config->paper().feePct;
    state.settings.slippagePct = m_config->paper().slippagePct;
    state.balance = root.value(QLatin1String("balance")).toDouble(-1);
    state.peakEquity = root.value(QLatin1String("peak_equity")).toDouble();
    state.dayStartEquity = root.value(QLatin1String("day_start_equity")).toDouble();
    state.day = root.value(QLatin1String("day")).toInteger();
    state.maxDrawdownPct = root.value(QLatin1String("max_drawdown_pct")).toDouble();
    state.nextId = root.value(QLatin1String("next_id")).toInteger(1);
    const QJsonArray positions = root.value(QLatin1String("positions")).toArray();
    for (const QJsonValue &v : positions)
        state.positions.push_back(positionFrom(v.toObject()));
    const QJsonArray trades = root.value(QLatin1String("trades")).toArray();
    for (const QJsonValue &v : trades)
        state.trades.push_back(tradeFrom(v.toObject()));
    if (root.isEmpty() || !m_account.restore(state)) {
        // Kept aside, never silently overwritten: it is the user's history.
        const QString aside = path + QStringLiteral(".unreadable");
        QFile::remove(aside);
        QFile::rename(path, aside);
        log(QStringLiteral("error"),
            tr("The saved paper account could not be read and was moved to %1; starting a fresh one.").arg(aside));
        return;
    }
    m_halted = root.value(QLatin1String("halted")).toBool();
    const QJsonArray curve = root.value(QLatin1String("equity_curve")).toArray();
    for (const QJsonValue &v : curve) {
        const QJsonArray point = v.toArray();
        if (point.size() == 2 && point.at(1).toDouble() > 0)
            m_equityCurve.append(QVariantMap{{QStringLiteral("t"), point.at(0).toDouble()},
                                             {QStringLiteral("v"), point.at(1).toDouble()}});
    }
    if (!m_equityCurve.isEmpty())
        m_lastCurvePoint = qint64(m_equityCurve.last().toMap().value(QStringLiteral("t")).toDouble());
}

void TradingEngine::save()
{
    const PaperAccount::State state = m_account.state();
    QJsonArray positions, trades, curve;
    for (const Position &p : state.positions)
        positions.append(positionJson(p));
    const size_t first = state.trades.size() > size_t(kMaxStoredTrades) ? state.trades.size() - kMaxStoredTrades : 0;
    for (size_t i = first; i < state.trades.size(); ++i)
        trades.append(tradeJson(state.trades[i]));
    for (const QVariant &v : m_equityCurve) {
        const QVariantMap point = v.toMap();
        curve.append(QJsonArray{point.value(QStringLiteral("t")).toDouble(), point.value(QStringLiteral("v")).toDouble()});
    }
    const QJsonObject root{
        {QStringLiteral("version"), 1},
        {QStringLiteral("settings"), QJsonObject{{QStringLiteral("starting_balance"), state.settings.startingBalance}}},
        {QStringLiteral("balance"), state.balance},
        {QStringLiteral("peak_equity"), state.peakEquity},
        {QStringLiteral("day_start_equity"), state.dayStartEquity},
        {QStringLiteral("day"), qint64(state.day)},
        {QStringLiteral("max_drawdown_pct"), state.maxDrawdownPct},
        {QStringLiteral("next_id"), qint64(state.nextId)},
        {QStringLiteral("halted"), m_halted},
        {QStringLiteral("positions"), positions},
        {QStringLiteral("trades"), trades},
        {QStringLiteral("equity_curve"), curve},
    };
    QSaveFile file(m_config->dataDir() + QStringLiteral("/paper.json"));
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (file.commit())
        QFile::setPermissions(file.fileName(), QFile::ReadOwner | QFile::WriteOwner);
}

// ---- diagnostics -----------------------------------------------------------------------

QJsonObject TradingEngine::dumpConfig() const
{
    QJsonArray panel;
    for (const QVariant &v : analysts()) {
        const QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("kind")).toString() == QLatin1String("model"))
            panel.append(QJsonObject::fromVariantMap(m));
    }
    return {{QStringLiteral("mode"), mode()},
            {QStringLiteral("dataSource"), m_config->dataUrl()},
            {QStringLiteral("modelService"), m_config->baseUrl()},
            {QStringLiteral("key"), QJsonObject::fromVariantMap(key())},
            {QStringLiteral("watchlist"), QJsonArray::fromStringList(m_config->watchlist())},
            {QStringLiteral("symbol"), symbol()}, {QStringLiteral("interval"), interval()},
            {QStringLiteral("autoTrade"), autoTrade()}, {QStringLiteral("autoAnalyze"), autoAnalyze()},
            {QStringLiteral("risk"), QJsonObject::fromVariantMap(riskSettings())},
            {QStringLiteral("paper"), QJsonObject::fromVariantMap(paperSettings())},
            {QStringLiteral("panel"), panel},
            {QStringLiteral("webViewAvailable"), m_webViewAvailable}};
}

QJsonObject TradingEngine::dumpAccount() const
{
    return {{QStringLiteral("account"), QJsonObject::fromVariantMap(account())},
            {QStringLiteral("positions"), QJsonArray::fromVariantList(positions())},
            {QStringLiteral("trades"), QJsonArray::fromVariantList(trades())},
            {QStringLiteral("activity"), QJsonArray::fromVariantList(m_activity)}};
}

QJsonObject TradingEngine::dumpMarket() const
{
    return {{QStringLiteral("dataState"), m_dataState}, {QStringLiteral("dataError"), m_dataError},
            {QStringLiteral("watchlist"), QJsonArray::fromVariantList(watchlist())},
            {QStringLiteral("quote"), QJsonObject::fromVariantMap(quote())},
            {QStringLiteral("readings"), QJsonObject::fromVariantMap(readings())}};
}
