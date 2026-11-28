// SPDX-License-Identifier: GPL-3.0-or-later
//
// SBT Model 1 in the trading engine: the trader's strategy is where every
// trade comes from. The candles of the selected market go to the SBT engine
// (core/sbt.h) as they close; its setups are reviewed by the AI team against
// the strategy's rules while they wait for their retest; an entry is placed
// on the paper account at the next candle's open when the rules are met and,
// unless switched off, the team confirmed the setup.
#include "tradingengine.h"

#include "modelclient.h"
#include "tradingconfig.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>
#include <cmath>

using namespace Trading;

namespace {

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

QString qs(const std::string &s)
{
    return QString::fromStdString(s);
}

Action actionOf(const Sbt::Setup &s)
{
    return s.direction == Sbt::Direction::Bullish ? Action::Buy : Action::Sell;
}

bool open(const Sbt::Setup &s)
{
    return !Sbt::finished(s.status);
}

} // namespace

const Sbt::Setup *TradingEngine::sbtSetup(const QString &id) const
{
    for (const Sbt::Setup &s : m_sbt.setups()) {
        if (qs(s.id) == id)
            return &s;
    }
    return nullptr;
}

// The setup the desk is about now: one that entered, then one waiting for its
// entry, then the newest one waiting for its retest, then the newest finished one.
const Sbt::Setup *TradingEngine::currentSetup() const
{
    const std::vector<Sbt::Setup> &all = m_sbt.setups();
    for (Sbt::Status want : {Sbt::Status::Entered, Sbt::Status::Triggered, Sbt::Status::Armed}) {
        for (auto it = all.rbegin(); it != all.rend(); ++it) {
            if (it->status == want && !(want == Sbt::Status::Entered && it->historical))
                return &*it;
        }
    }
    return all.empty() ? nullptr : &all.back();
}

// The setup the AI team can be asked about: one that is still in play.
const Sbt::Setup *TradingEngine::reviewable() const
{
    const Sbt::Setup *s = currentSetup();
    return s && open(*s) && !s->historical ? s : nullptr;
}

bool TradingEngine::aiAvailable() const
{
    if (m_config->apiKey().isEmpty())
        return false;
    for (const TradingConfig::Member &m : m_config->panel()) {
        if (m.enabled && m_models->offered(m.model))
            return true;
    }
    return false;
}

void TradingEngine::feedSbt()
{
    if (m_candles.empty())
        return;
    std::vector<Sbt::Event> events;
    const QString key = m_config->symbol() + QLatin1Char(':') + m_config->interval();
    // Candles missing since the last one read (the computer slept, the
    // network was gone): what happened meanwhile is history, not a signal.
    const qint64 step = TradingConfig::intervalMs(m_config->interval());
    bool gap = false;
    if (key == m_sbtKey && m_sbt.lastTime() > 0) {
        for (const Candle &c : m_candles) {
            if (c.closed && c.openTime > m_sbt.lastTime()) {
                gap = c.openTime - m_sbt.lastTime() > step + step / 2;
                break;
            }
        }
    }
    const bool replay = key != m_sbtKey || gap;
    if (replay) {
        // Another market or interval (or a gap): its history is read once, as
        // history. Setup ids carry the market, and what was traded or
        // reviewed is kept, so reading a market again never enters a setup twice.
        m_sbt = Sbt::Engine(m_config->sbt());
        m_sbt.setIdPrefix((key + QLatin1Char(':')).toStdString());
        m_sbt.setLive(false);
        m_sbtKey = key;
        m_pendingEntries.clear();
        if (m_traded.size() > 2000) {
            m_traded.clear();
            m_placed.clear();
            m_outcomes.clear();
            m_reviews.clear();
        }
    }
    for (const Candle &c : m_candles) {
        if (c.closed && c.openTime > m_sbt.lastTime()) {
            const std::vector<Sbt::Event> e = m_sbt.close(c);
            events.insert(events.end(), e.begin(), e.end());
        }
    }
    if (replay) {
        m_sbt.setLive(true);
        if (gap)
            log(QStringLiteral("info"), tr("SBT Model 1: candles were missing on %1; what happened meanwhile was read "
                                           "as history and is not traded.").arg(symbolName()));
        else if (!m_sbt.setups().empty())
            log(QStringLiteral("info"),
                tr("SBT Model 1 read %1 %2: %n setup(s) in the history.", nullptr, int(m_sbt.setups().size()))
                    .arg(symbolName(), m_config->interval()));
    }
    const Candle &last = m_candles.back();
    if (!last.closed) {
        const std::vector<Sbt::Event> e = m_sbt.forming(last);
        events.insert(events.end(), e.begin(), e.end());
    }
    handleSbt(events);
    updateSbtHistory();
    // Level 3 moving, the intrabar note… change the setup without an event:
    // the window is told whenever what it shows of the setup changed.
    const QString shown = sbtSignature();
    if (replay || !events.empty() || shown != m_sbtShown)
        refreshSignal();
}

QString TradingEngine::sbtSignature() const
{
    const Sbt::Setup *s = currentSetup();
    // The gap pause shows whether or not there is a setup.
    const QString gap = m_sbt.gap().active ? QStringLiteral("gap:%1:%2|").arg(double(m_sbt.gap().time), 0, 'f', 0).arg(m_sbt.gap().left)
                                           : QString();
    if (!s)
        return gap;
    return gap + QStringLiteral("%1|%2|%3|%4|%5|%6|%7").arg(QString::fromStdString(s->id))
        .arg(QLatin1String(Sbt::statusName(s->status))).arg(s->level3.price, 0, 'g', 12)
        .arg(s->level3Confirmed).arg(s->intrabar).arg(double(s->updated), 0, 'f', 0).arg(m_sbt.pdArrays().size());
}

bool TradingEngine::sbtSettingsDiffer() const
{
    const Sbt::Settings a = m_sbt.settings(), b = m_config->sbt();
    return a.zoneMoveAtr != b.zoneMoveAtr || a.gapAtr != b.gapAtr
        || a.gapPauseCandles != b.gapPauseCandles || a.stopBufferAtr != b.stopBufferAtr
        || a.minRewardRisk != b.minRewardRisk || a.requirePdArray != b.requirePdArray
        || a.pdDistanceAtr != b.pdDistanceAtr || a.maxArmedCandles != b.maxArmedCandles;
}

void TradingEngine::handleSbt(const std::vector<Sbt::Event> &events)
{
    const QString name = symbolName() + QLatin1Char(' ') + m_config->interval();
    for (const Sbt::Event &e : events) {
        if (!e.live)
            continue; // history: shown, never acted on
        const Sbt::Setup &s = e.setup;
        const QString dir = s.direction == Sbt::Direction::Bullish ? tr("bullish") : tr("bearish");
        const QString id = qs(s.id);
        if (e.kind == "armed") {
            log(QStringLiteral("signal"),
                tr("SBT Model 1, %1 %2: Level 1 %3, Level 2 %4, BOS at %5. Waiting for the retest of the Level 2 "
                   "zone %6 – %7.")
                    .arg(name, dir).arg(s.level1.price, 0, 'g', 8).arg(s.level2.price, 0, 'g', 8)
                    .arg(s.bos.price, 0, 'g', 8).arg(s.zoneBottom, 0, 'g', 8).arg(s.zoneTop, 0, 'g', 8));
            // The team reviews it while it waits, so an entry need not wait for the team.
            if (m_config->autoAnalyze() && aiAvailable() && !m_analyzing)
                startReview(id);
        } else if (e.kind == "triggered") {
            log(QStringLiteral("signal"),
                tr("SBT Model 1, %1 %2: the zone was retested and the candle closed %3 it. Entry at the next "
                   "candle's open; target Level 3 %4.")
                    .arg(name, dir, s.direction == Sbt::Direction::Bullish ? tr("above") : tr("below"))
                    .arg(s.target, 0, 'g', 8));
        } else if (e.kind == "entered") {
            onSbtEntry(s);
        } else if (e.kind == "invalidated" || e.kind == "skipped" || e.kind == "expired") {
            m_pendingEntries.remove(id);
            log(QStringLiteral("info"), tr("SBT Model 1, %1 %2 setup %3: %4.")
                                            .arg(name, dir, e.kind == "invalidated" ? tr("invalidated")
                                                            : e.kind == "skipped" ? tr("not taken") : tr("expired"),
                                                 qs(s.note)));
        } else if ((e.kind == "target" || e.kind == "stopped") && !s.historical) {
            log(QStringLiteral("info"), tr("SBT Model 1, %1 %2 setup: %3.")
                                            .arg(name, dir, e.kind == "target" ? tr("Level 3 reached")
                                                                               : tr("the stop was reached")));
        }
    }
}

void TradingEngine::onSbtEntry(const Sbt::Setup &s)
{
    const QString id = qs(s.id);
    if (m_traded.contains(id) || s.historical)
        return; // never twice for one setup
    if (m_config->sbtAiConfirm() && aiAvailable()) {
        const Review r = m_reviews.value(id);
        if (!r.done) {
            m_pendingEntries.insert(id);
            log(QStringLiteral("info"), tr("SBT Model 1 entry on %1: waiting for the AI team to confirm the setup.")
                                            .arg(symbolName()));
            if (!r.running && !m_analyzing && !startReview(id)) {
                // The team cannot be asked (no price history to show it): not traded.
                m_pendingEntries.remove(id);
                m_traded.insert(id);
                m_outcomes.insert(id, tr("Not traded: the AI team could not be asked to review the setup."));
                log(QStringLiteral("risk"), m_outcomes.value(id));
            }
            refreshSignal();
            return;
        }
        if (!r.confirmed) {
            m_traded.insert(id);
            const Signal counted = signalFor(s);
            m_outcomes.insert(id, tr("Not traded: the AI team did not confirm the setup (%1 of %2 agree).")
                                      .arg(counted.agreeing).arg(counted.voters));
            log(QStringLiteral("risk"), m_outcomes.value(id));
            refreshSignal();
            return;
        }
    }
    executeSbt(id);
}

Signal TradingEngine::signalFor(const Sbt::Setup &s, bool rulesOnly) const
{
    Signal sig;
    const Review r = rulesOnly ? Review() : m_reviews.value(qs(s.id));
    const bool bull = s.direction == Sbt::Direction::Bullish;
    const bool signal = s.status == Sbt::Status::Triggered || s.status == Sbt::Status::Entered;
    sig.action = signal ? actionOf(s) : Action::Hold;
    sig.leaning = actionOf(s);
    // Before the retest the levels are the plan: entry at the zone, the stop
    // beyond it, the target at Level 3 as it stands.
    const double buffer = m_config->sbt().stopBufferAtr * m_sbt.atr();
    sig.stop = s.stop > 0 ? s.stop
                          : (bull ? std::min(s.zoneBottom, s.level2.price) - buffer : std::max(s.zoneTop, s.level2.price) + buffer);
    sig.takeProfit = s.target > 0 ? s.target : s.level3.price;
    sig.entry = s.entry > 0 ? s.entry : s.retest.index >= 0 ? s.retest.price : (bull ? s.zoneTop : s.zoneBottom);
    sig.rewardRisk = rewardRisk(actionOf(s), sig.entry, sig.stop, sig.takeProfit);
    if (r.done) {
        // Counted against the setup's direction, whatever side the team's consensus took.
        sig.confidence = r.result.action == actionOf(s) ? r.result.confidence : 0;
        sig.voters = 0;
        sig.agreeing = sig.opposing = sig.holding = 0;
        for (const Verdict &v : r.result.verdicts) {
            if (!v.ok)
                continue;
            ++sig.voters;
            if (v.action == actionOf(s))
                ++sig.agreeing;
            else if (v.action == Action::Hold)
                ++sig.holding;
            else
                ++sig.opposing;
        }
        sig.verdicts = r.result.verdicts;
        sig.lead = r.result.lead;
        sig.reasoning = r.result.reasoning;
    } else {
        // The rules alone: the strategy itself is the one voter.
        sig.confidence = signal ? 100 : 0;
        sig.voters = 1;
        sig.agreeing = 1;
        sig.lead = "sbt";
    }
    if (sig.reasoning.empty()) {
        std::string pd;
        for (const Sbt::PdArray &a : s.confirmations)
            pd += std::string(pd.empty() ? "" : ", ") + Sbt::pdName(a.kind, a.bullish);
        sig.reasoning = std::string("SBT Model 1, ") + Sbt::directionName(s.direction) + ": 1-2-3 with a BOS beyond Level 1, "
            + (s.retest.index >= 0 ? "the Level 2 zone retested" : "waiting for the retest of the Level 2 zone")
            + (pd.empty() ? std::string() : "; PD arrays: " + pd) + ".";
    }
    switch (s.status) {
    case Sbt::Status::Armed: sig.note = "Setup armed: waiting for the retest of the Level 2 zone."; break;
    case Sbt::Status::Triggered: sig.note = "Retest confirmed: entry at the next candle's open."; break;
    case Sbt::Status::Entered: break;
    case Sbt::Status::Target: sig.note = "Level 3 was reached."; break;
    case Sbt::Status::Stopped: sig.note = "The stop was reached."; break;
    default: sig.note = s.note; break;
    }
    return sig;
}

void TradingEngine::executeSbt(const QString &id)
{
    const Sbt::Setup *found = sbtSetup(id);
    if (!found || m_traded.contains(id))
        return;
    const Sbt::Setup s = *found;
    m_traded.insert(id);
    if (s.status != Sbt::Status::Entered) {
        m_outcomes.insert(id, tr("Not traded: the setup is %1 by now.").arg(QLatin1String(Sbt::statusName(s.status))));
        log(QStringLiteral("risk"), m_outcomes.value(id));
        refreshSignal();
        return;
    }
    const qint64 now = nowMs();
    const RiskSettings risk = m_config->risk();
    m_analysisSymbol = m_config->symbol();
    m_analysisInterval = m_config->interval();
    // With "AI team confirms" off the strategy decides alone, whatever a review said.
    Signal signal = signalFor(s, !m_config->sbtAiConfirm());
    // The order is filled at the price now: at the open when it goes in at once.
    const double price = m_candles.back().close;
    signal.entry = price;
    signal.rewardRisk = rewardRisk(signal.action, signal.entry, signal.stop, signal.takeProfit);
    QString outcome;
    bool traded = false;
    // An open position the other way is closed, but only for a trade the
    // rules allow: the check is made as if it were already closed.
    const Position *opposite = m_account.positionOn(m_analysisSymbol.toStdString());
    if (opposite && opposite->isLong == (signal.action == Action::Buy))
        opposite = nullptr;
    AccountView view = accountView(m_analysisSymbol, now);
    if (opposite) {
        view.positionOnSymbol = false;
        view.openPositions = std::max(0, view.openPositions - 1);
    }
    RiskDecision decision = evaluate(signal, view, risk, now);
    if (decision.allowed && opposite && m_config->autoTrade()) {
        Trade closed;
        if (m_account.close(opposite->id, price, now, "signal_flip", &closed)) {
            recordClosed({closed});
            afterAccountChange(true);
        }
        decision = evaluate(signal, accountView(m_analysisSymbol, now), risk, now);
    }
    log(QStringLiteral("signal"),
        tr("SBT Model 1 %1 on %2 %3: entry %4, stop %5, target Level 3 %6, reward/risk %7.")
            .arg(signal.action == Action::Buy ? tr("BUY") : tr("SELL"), symbolName(), m_analysisInterval)
            .arg(signal.entry, 0, 'g', 8).arg(signal.stop, 0, 'g', 8).arg(signal.takeProfit, 0, 'g', 8)
            .arg(signal.rewardRisk, 0, 'f', 2));
    if (!decision.allowed) {
        outcome = tr("Not traded. %1").arg(qs(decision.reason));
        log(QStringLiteral("risk"), outcome);
    } else if (m_config->autoTrade()) {
        const QString error = execute(signal, decision);
        traded = error.isEmpty();
        outcome = traded ? tr("Placed on the paper account.") : tr("Not placed: %1").arg(error);
        if (traded)
            decision = evaluate(signal, accountView(m_analysisSymbol, now), risk, now);
    } else {
        outcome = tr("Passes the risk rules. Auto trade is off, so nothing was placed.");
        m_traded.remove(id); // it can still be placed by hand, once
    }
    m_lastSignal = signal;
    m_lastDecision = decision;
    m_lastDecisionFor = id;
    m_lastSignalAt = now;
    m_outcomes.insert(id, outcome);
    if (traded)
        m_placed.insert(id);
    save(); // handled setups survive a restart
    refreshSignal();
}

QVariantMap TradingEngine::setupMap(const Sbt::Setup &s) const
{
    const auto point = [](const Sbt::Point &p) {
        return p.index < 0 && p.time == 0 ? QVariantMap()
                                          : QVariantMap{{QStringLiteral("price"), p.price}, {QStringLiteral("time"), double(p.time)}};
    };
    QVariantList pd;
    for (const Sbt::PdArray &a : s.confirmations)
        pd.append(QVariantMap{{QStringLiteral("kind"), QLatin1String(Sbt::pdName(a.kind, a.bullish))},
                              {QStringLiteral("top"), a.top}, {QStringLiteral("bottom"), a.bottom},
                              {QStringLiteral("time"), double(a.time)}});
    const QString id = qs(s.id);
    const Review r = m_reviews.value(id);
    QVariantMap review{{QStringLiteral("running"), r.running}, {QStringLiteral("done"), r.done},
                       {QStringLiteral("confirmed"), r.confirmed}};
    if (r.done) {
        const Signal counted = signalFor(s);
        review.insert(QStringLiteral("agreeing"), counted.agreeing);
        review.insert(QStringLiteral("voters"), counted.voters);
        review.insert(QStringLiteral("confidence"), r.result.action == actionOf(s) ? r.result.confidence : 0.0);
        review.insert(QStringLiteral("time"), double(r.time));
    }
    return {
        {QStringLiteral("id"), id},
        {QStringLiteral("direction"), QLatin1String(Sbt::directionName(s.direction))},
        {QStringLiteral("status"), QLatin1String(Sbt::statusName(s.status))},
        {QStringLiteral("note"), qs(s.note)},
        {QStringLiteral("level1"), point(s.level1)},
        {QStringLiteral("level2"), point(s.level2)},
        {QStringLiteral("bos"), point(s.bos)},
        {QStringLiteral("level3"), point(s.level3)},
        {QStringLiteral("level3Confirmed"), s.level3Confirmed},
        // The previous low Level 2 is the Higher Low above (bearish: the previous high, Level 2 the Lower High below).
        {QStringLiteral("previous"), s.previous.index < 0 ? QVariantMap()
             : QVariantMap{{QStringLiteral("price"), s.previous.price}, {QStringLiteral("time"), double(s.previous.time)}}},
        {QStringLiteral("zone"), QVariantMap{{QStringLiteral("top"), s.zoneTop}, {QStringLiteral("bottom"), s.zoneBottom},
                                             {QStringLiteral("time"), double(s.zoneTime)},
                                             // Rules 3/4: the zone candle is the order block and the demand (supply) zone.
                                             {QStringLiteral("orderBlock"), s.orderBlock},
                                             {QStringLiteral("demand"), s.demand}}},
        {QStringLiteral("retest"), point(s.retest)},
        {QStringLiteral("intrabar"), s.intrabar},
        {QStringLiteral("entry"), s.entry},
        {QStringLiteral("entryTime"), double(s.entryTime)},
        {QStringLiteral("stop"), s.stop},
        {QStringLiteral("target"), s.target > 0 ? s.target : s.level3.price},
        {QStringLiteral("rewardRisk"), s.rewardRisk},
        {QStringLiteral("historical"), s.historical},
        {QStringLiteral("confirmations"), pd},
        {QStringLiteral("review"), review},
        {QStringLiteral("traded"), m_placed.contains(id)},
        {QStringLiteral("pending"), m_pendingEntries.contains(id)},
        {QStringLiteral("outcome"), m_outcomes.value(id)},
        {QStringLiteral("updated"), double(s.updated)},
    };
}

QJsonObject TradingEngine::setupJson(const Sbt::Setup &s) const
{
    QJsonObject o = QJsonObject::fromVariantMap(setupMap(s));
    // What the models need, not the desk's bookkeeping.
    for (const char *key : {"review", "traded", "pending", "outcome", "historical", "updated", "id", "intrabar"})
        o.remove(QLatin1String(key));
    o.insert(QStringLiteral("strategy"), QStringLiteral("SBT Model 1"));
    o.insert(QStringLiteral("atr"), m_sbt.atr());
    if (s.stop <= 0) {
        const Signal planned = signalFor(s);
        o.insert(QStringLiteral("planned_stop"), planned.stop);
        o.insert(QStringLiteral("planned_entry"), planned.entry);
    }
    return o;
}

QVariantMap TradingEngine::sbt() const
{
    QVariantList setups;
    const std::vector<Sbt::Setup> &all = m_sbt.setups();
    for (auto it = all.rbegin(); it != all.rend() && setups.size() < 10; ++it)
        setups.append(setupMap(*it));
    QVariantList arrays;
    for (const Sbt::PdArray &a : m_sbt.pdArrays()) {
        if (a.broken || (a.kind == Sbt::PdKind::FairValueGap && a.filled))
            continue;
        arrays.append(QVariantMap{{QStringLiteral("kind"), QLatin1String(Sbt::pdName(a.kind, a.bullish))},
                                  {QStringLiteral("bullish"), a.bullish}, {QStringLiteral("top"), a.top},
                                  {QStringLiteral("bottom"), a.bottom}, {QStringLiteral("time"), double(a.time)},
                                  {QStringLiteral("touched"), a.touched}});
    }
    const auto candidate = [this](Sbt::Direction d) {
        const Sbt::Point p = m_sbt.candidate(d);
        return p.index < 0 ? QVariantMap()
                           : QVariantMap{{QStringLiteral("price"), p.price}, {QStringLiteral("time"), double(p.time)}};
    };
    const Sbt::Setup *current = currentSetup();
    return {
        {QStringLiteral("strategy"), QStringLiteral("SBT Model 1")},
        {QStringLiteral("market"), m_sbtKey},
        {QStringLiteral("candles"), m_sbt.closedCount()},
        {QStringLiteral("atr"), m_sbt.atr()},
        {QStringLiteral("current"), current ? setupMap(*current) : QVariantMap()},
        {QStringLiteral("setups"), setups},
        {QStringLiteral("pdArrays"), arrays},
        {QStringLiteral("watching"), QVariantMap{{QStringLiteral("bullish"), candidate(Sbt::Direction::Bullish)},
                                                 {QStringLiteral("bearish"), candidate(Sbt::Direction::Bearish)}}},
        {QStringLiteral("aiConfirm"), m_config->sbtAiConfirm()},
        {QStringLiteral("aiAvailable"), aiAvailable()},
        // The confirmed highs and lows, newest last, labelled HH, LH, HL or LL.
        {QStringLiteral("structure"), [this] {
             QVariantList points;
             const std::vector<Sbt::StructurePoint> &all = m_sbt.structure();
             for (size_t i = all.size() > 12 ? all.size() - 12 : 0; i < all.size(); ++i)
                 points.append(QVariantMap{{QStringLiteral("price"), all[i].point.price},
                                           {QStringLiteral("time"), double(all[i].point.time)},
                                           {QStringLiteral("high"), all[i].high},
                                           {QStringLiteral("label"), QString::fromStdString(all[i].label)}});
             return points;
         }()},
        // Rule 5: a gap pauses the strategy.
        {QStringLiteral("gap"), QVariantMap{{QStringLiteral("active"), m_sbt.gap().active},
                                            {QStringLiteral("up"), m_sbt.gap().up},
                                            {QStringLiteral("time"), double(m_sbt.gap().time)},
                                            {QStringLiteral("fill"), m_sbt.gap().fill},
                                            {QStringLiteral("left"), m_sbt.gap().left}}},
    };
}

QVariantMap TradingEngine::sbtSettings() const
{
    const Sbt::Settings s = m_config->sbt();
    return {{QStringLiteral("ai_confirm"), m_config->sbtAiConfirm()},
            {QStringLiteral("require_pd_array"), s.requirePdArray},
            {QStringLiteral("zone_move_atr"), s.zoneMoveAtr},
            {QStringLiteral("gap_atr"), s.gapAtr},
            {QStringLiteral("gap_pause_candles"), s.gapPauseCandles},
            {QStringLiteral("stop_buffer_atr"), s.stopBufferAtr},
            {QStringLiteral("pd_distance_atr"), s.pdDistanceAtr},
            {QStringLiteral("max_wait_candles"), s.maxArmedCandles}};
}

bool TradingEngine::setSbtSetting(const QString &name, const QVariant &value)
{
    if (!m_config->setSbt(name, value))
        return false;
    // The detection rules changed: the history is read again with them.
    if (name != QLatin1String("ai_confirm")) {
        m_sbtKey.clear();
        feedSbt();
    }
    refreshSignal();
    return true;
}

// The signal panel's data: the current SBT setup, the AI team's review of it,
// and the risk rules applied to it.
void TradingEngine::refreshSignal()
{
    const QVariantMap before = m_signal;
    const qint64 now = nowMs();
    const Sbt::Setup *s = currentSetup();
    QVariantMap map;
    if (!s) {
        Signal none;
        none.note = "No SBT Model 1 setup yet: watching for a 1-2-3 with a break of structure.";
        map = signalMap(none, RiskDecision());
        map.insert(QStringLiteral("state"), m_sbtKey.isEmpty() ? QStringLiteral("none") : QStringLiteral("ready"));
    } else {
        const Signal sig = signalFor(*s);
        const QString id = qs(s->id);
        const RiskDecision decision = m_lastDecisionFor == id ? m_lastDecision
                                                              : evaluate(sig, accountView(m_config->symbol(), now), m_config->risk(), now);
        map = signalMap(m_lastDecisionFor == id ? m_lastSignal : sig, decision);
        map.insert(QStringLiteral("state"), QStringLiteral("ready"));
        map.insert(QStringLiteral("sbt"), setupMap(*s));
        const bool placed = m_placed.contains(id);
        map.insert(QStringLiteral("traded"), placed);
        map.insert(QStringLiteral("outcome"), m_outcomes.value(id));
        map.insert(QStringLiteral("placeable"), s->status == Sbt::Status::Entered && !placed && !m_traded.contains(id)
                                                    && m_lastDecisionFor == id && decision.allowed);
    }
    map.insert(QStringLiteral("strategy"), QStringLiteral("SBT Model 1"));
    map.insert(QStringLiteral("time"), double(m_lastSignalAt > 0 ? m_lastSignalAt : now));
    map.insert(QStringLiteral("symbol"), m_config->symbol());
    map.insert(QStringLiteral("symbolName"), symbolName());
    map.insert(QStringLiteral("interval"), m_config->interval());
    if (m_analyzing) {
        map.insert(QStringLiteral("state"), QStringLiteral("analyzing"));
        for (const char *key : {"stage", "provisional"}) {
            if (before.contains(QLatin1String(key)))
                map.insert(QLatin1String(key), before.value(QLatin1String(key)));
        }
    }
    m_signal = map;
    m_sbtShown = sbtSignature();
    Q_EMIT signalChanged();
}

// ---- the strategy over the chart's history -------------------------------------

void TradingEngine::updateSbtHistory()
{
    if (m_chart.empty())
        return;
    const QString key = m_config->symbol() + QLatin1Char(':') + m_config->interval();
    const long long first = m_chart.front().openTime;
    // Older candles in front (the chart was moved into the past), another
    // market or other rules: read everything again. Reading 20,000 candles
    // takes a few milliseconds; otherwise only the new ones are read.
    const bool rebuild = key != m_sbtHistoryKey || first != m_sbtHistoryFirst
        || m_sbtHistory.settings().zoneMoveAtr != m_config->sbt().zoneMoveAtr
        || m_sbtHistory.settings().gapAtr != m_config->sbt().gapAtr
        || m_sbtHistory.settings().gapPauseCandles != m_config->sbt().gapPauseCandles
        || m_sbtHistory.settings().requirePdArray != m_config->sbt().requirePdArray
        || m_sbtHistory.settings().stopBufferAtr != m_config->sbt().stopBufferAtr
        || m_sbtHistory.settings().minRewardRisk != m_config->sbt().minRewardRisk
        || m_sbtHistory.settings().pdDistanceAtr != m_config->sbt().pdDistanceAtr;
    if (rebuild) {
        Sbt::Settings settings = m_config->sbt();
        settings.keepFinished = 5000;
        m_sbtHistory = Sbt::Engine(settings);
        m_sbtHistory.setIdPrefix((QStringLiteral("history:") + key + QLatin1Char(':')).toStdString());
        m_sbtHistory.setLive(false);
        m_sbtHistoryKey = key;
        m_sbtHistoryFirst = first;
    }
    // The newest closed candles not read yet (from the end: usually one or none).
    auto from = m_chart.end();
    while (from != m_chart.begin() && (from - 1)->openTime > m_sbtHistory.lastTime())
        --from;
    for (auto it = from; it != m_chart.end(); ++it) {
        if (it->closed)
            m_sbtHistory.close(*it);
    }
    if (!m_chart.back().closed)
        m_sbtHistory.forming(m_chart.back());
    // Told only when something it shows changed.
    QString shown = QString::number(m_sbtHistory.setups().size());
    for (const Sbt::Setup &s : m_sbtHistory.setups()) {
        if (s.entryTime > 0)
            shown += QLatin1Char('|') + QString::number(int(s.status)) + QLatin1Char(':') + QString::number(s.entryTime);
    }
    shown += QLatin1Char('#') + QString::number(m_account.trades().size()) + QLatin1Char('/')
        + QString::number(m_account.positions().size());
    if (rebuild || shown != m_sbtHistoryShown) {
        m_sbtHistoryShown = shown;
        m_sbtHistoryDirty = true;
        Q_EMIT sbtHistoryChanged();
    }
}

QVariantMap TradingEngine::sbtHistory() const
{
    if (!m_sbtHistoryDirty)
        return m_sbtHistoryCache;
    m_sbtHistoryCache = buildSbtHistory();
    m_sbtHistoryDirty = false;
    return m_sbtHistoryCache;
}

QVariantMap TradingEngine::buildSbtHistory() const
{
    QVariantList setups;
    int targets = 0, stops = 0, open = 0, missed = 0;
    double totalR = 0;
    const double price = m_candles.empty() ? 0 : m_candles.back().close;
    for (const Sbt::Setup &s : m_sbtHistory.setups()) {
        if (s.entryTime <= 0) {
            missed += s.status == Sbt::Status::Invalidated || s.status == Sbt::Status::Skipped
                    || s.status == Sbt::Status::Expired ? 1 : 0;
            continue;
        }
        const bool bull = s.direction == Sbt::Direction::Bullish;
        const double risk = bull ? s.entry - s.stop : s.stop - s.entry;
        double r = 0, exit = 0;
        QString result;
        if (s.status == Sbt::Status::Target) {
            exit = s.target;
            r = s.rewardRisk;
            result = QStringLiteral("target");
            ++targets;
        } else if (s.status == Sbt::Status::Stopped) {
            exit = s.stop;
            r = -1;
            result = QStringLiteral("stop");
            ++stops;
        } else {
            // Still open: where it stands at the price now.
            r = risk > 0 && price > 0 ? (bull ? price - s.entry : s.entry - price) / risk : 0;
            result = QStringLiteral("open");
            ++open;
        }
        if (result != QLatin1String("open"))
            totalR += r;
        QVariantList pd;
        for (const Sbt::PdArray &a : s.confirmations)
            pd.append(QLatin1String(Sbt::pdName(a.kind, a.bullish)));
        setups.append(QVariantMap{
            {QStringLiteral("direction"), QLatin1String(Sbt::directionName(s.direction))},
            {QStringLiteral("result"), result},
            {QStringLiteral("entry"), s.entry}, {QStringLiteral("entryTime"), double(s.entryTime)},
            {QStringLiteral("stop"), s.stop}, {QStringLiteral("target"), s.target},
            {QStringLiteral("exit"), exit}, {QStringLiteral("exitTime"), double(s.outcomeTime)},
            {QStringLiteral("r"), r}, {QStringLiteral("rewardRisk"), s.rewardRisk},
            {QStringLiteral("level1"), s.level1.price}, {QStringLiteral("level1Time"), double(s.level1.time)},
            {QStringLiteral("bosTime"), double(s.bos.time)},
            {QStringLiteral("zoneTop"), s.zoneTop}, {QStringLiteral("zoneBottom"), s.zoneBottom},
            {QStringLiteral("zoneTime"), double(s.zoneTime)}, {QStringLiteral("retestTime"), double(s.retest.time)},
            {QStringLiteral("confirmations"), pd}});
    }
    // The paper account's own trades on this market: closed, and the open one.
    QVariantList trades;
    const std::string symbol = m_config->symbol().toStdString();
    for (const Trade &t : m_account.trades()) {
        if (t.symbol != symbol)
            continue;
        trades.append(QVariantMap{{QStringLiteral("side"), t.isLong ? QStringLiteral("buy") : QStringLiteral("sell")},
                                  {QStringLiteral("entry"), t.entry}, {QStringLiteral("entryTime"), double(t.openedAt)},
                                  {QStringLiteral("stop"), t.stop}, {QStringLiteral("target"), t.takeProfit},
                                  {QStringLiteral("exit"), t.exit}, {QStringLiteral("exitTime"), double(t.closedAt)},
                                  {QStringLiteral("pnl"), t.pnl}, {QStringLiteral("reason"), qs(t.reason)},
                                  {QStringLiteral("open"), false}});
    }
    for (const Position &p : m_account.positions()) {
        if (p.symbol != symbol)
            continue;
        trades.append(QVariantMap{{QStringLiteral("side"), p.isLong ? QStringLiteral("buy") : QStringLiteral("sell")},
                                  {QStringLiteral("entry"), p.entry}, {QStringLiteral("entryTime"), double(p.openedAt)},
                                  {QStringLiteral("stop"), p.stop}, {QStringLiteral("target"), p.takeProfit},
                                  {QStringLiteral("open"), true}});
    }
    const int closed = targets + stops;
    return {
        {QStringLiteral("market"), m_sbtHistoryKey},
        {QStringLiteral("candles"), m_sbtHistory.closedCount()},
        {QStringLiteral("from"), double(m_sbtHistoryFirst)},
        {QStringLiteral("setups"), setups},
        {QStringLiteral("trades"), trades},
        {QStringLiteral("stats"), QVariantMap{
            {QStringLiteral("entries"), int(setups.size())}, {QStringLiteral("targets"), targets},
            {QStringLiteral("stops"), stops}, {QStringLiteral("open"), open}, {QStringLiteral("missed"), missed},
            {QStringLiteral("winRate"), closed > 0 ? double(targets) / closed * 100.0 : 0.0},
            {QStringLiteral("totalR"), totalR},
            {QStringLiteral("averageR"), closed > 0 ? totalR / closed : 0.0}}},
    };
}
