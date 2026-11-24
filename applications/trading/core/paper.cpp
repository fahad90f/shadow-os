// SPDX-License-Identifier: GPL-3.0-or-later
#include "paper.h"

#include <algorithm>
#include <cmath>

namespace Trading {

namespace {

bool usable(double v)
{
    return std::isfinite(v) && v > 0;
}

double grossPnl(const Position &p, double price)
{
    return (p.isLong ? price - p.entry : p.entry - price) * p.quantity;
}

} // namespace

PaperAccount::PaperAccount(const PaperSettings &settings)
{
    reset(settings);
}

void PaperAccount::setCosts(double feePct, double slippagePct)
{
    if (std::isfinite(feePct) && feePct >= 0)
        m_settings.feePct = feePct;
    if (std::isfinite(slippagePct) && slippagePct >= 0)
        m_settings.slippagePct = slippagePct;
}

void PaperAccount::reset(const PaperSettings &settings)
{
    m_settings = settings;
    if (!usable(m_settings.startingBalance))
        m_settings.startingBalance = PaperSettings().startingBalance;
    m_balance = m_settings.startingBalance;
    m_peakEquity = m_balance;
    m_dayStartEquity = m_balance;
    m_day = 0;
    m_maxDrawdownPct = 0;
    m_nextId = 1;
    m_positions.clear();
    m_trades.clear();
}

double PaperAccount::marginInUse() const
{
    double margin = 0;
    for (const Position &p : m_positions)
        margin += p.entry * p.quantity;
    return margin;
}

double PaperAccount::available() const
{
    return std::max(0.0, m_balance - marginInUse());
}

double PaperAccount::unrealized() const
{
    double total = 0;
    for (const Position &p : m_positions)
        total += grossPnl(p, usable(p.lastPrice) ? p.lastPrice : p.entry);
    return total;
}

double PaperAccount::realizedOn(long long day) const
{
    double total = 0;
    for (const Trade &t : m_trades) {
        if (dayNumber(t.closedAt) == day)
            total += t.pnl;
    }
    return total;
}

const Position *PaperAccount::positionOn(const std::string &symbol) const
{
    for (const Position &p : m_positions) {
        if (p.symbol == symbol)
            return &p;
    }
    return nullptr;
}

long long PaperAccount::lastLossOn(const std::string &symbol) const
{
    for (auto it = m_trades.rbegin(); it != m_trades.rend(); ++it) {
        if (it->symbol == symbol && it->pnl < 0)
            return it->closedAt;
    }
    return 0;
}

PaperStats PaperAccount::stats() const
{
    PaperStats s;
    s.maxDrawdownPct = m_maxDrawdownPct;
    for (const Trade &t : m_trades) {
        ++s.trades;
        s.totalPnl += t.pnl;
        if (t.pnl > 0) {
            ++s.wins;
            s.grossProfit += t.pnl;
        } else {
            ++s.losses;
            s.grossLoss += -t.pnl;
        }
        s.bestTrade = s.trades == 1 ? t.pnl : std::max(s.bestTrade, t.pnl);
        s.worstTrade = s.trades == 1 ? t.pnl : std::min(s.worstTrade, t.pnl);
    }
    if (s.trades > 0)
        s.winRatePct = 100.0 * s.wins / s.trades;
    if (s.wins > 0)
        s.averageWin = s.grossProfit / s.wins;
    if (s.losses > 0)
        s.averageLoss = s.grossLoss / s.losses;
    if (s.grossLoss > 0)
        s.profitFactor = s.grossProfit / s.grossLoss;
    return s;
}

PaperAccount::OpenResult PaperAccount::open(const std::string &symbol, bool isLong, double quantity,
                                            double marketPrice, double stop, double takeProfit, long long nowMs,
                                            double confidence, const std::string &lead,
                                            const std::string &reasoning)
{
    OpenResult result;
    if (symbol.empty() || !usable(quantity) || !usable(marketPrice)) {
        result.error = "The order has no usable symbol, quantity or price.";
        return result;
    }
    if (positionOn(symbol)) {
        result.error = "There is already a position on " + symbol + ".";
        return result;
    }
    // A buy fills above the price, a sell below it.
    const double slip = m_settings.slippagePct / 100.0;
    const double fill = marketPrice * (isLong ? 1.0 + slip : 1.0 - slip);
    const bool levels = usable(stop) && usable(takeProfit)
        && (isLong ? stop < fill && takeProfit > fill : stop > fill && takeProfit < fill);
    if (!levels) {
        result.error = "The stop and target are not on the right sides of the fill price.";
        return result;
    }
    const double notional = fill * quantity;
    const double fee = notional * m_settings.feePct / 100.0;
    if (notional + fee > available() + 1e-9) {
        result.error = "The balance does not cover this order.";
        return result;
    }

    Position p;
    p.id = m_nextId++;
    p.symbol = symbol;
    p.isLong = isLong;
    p.quantity = quantity;
    p.entry = fill;
    p.stop = stop;
    p.takeProfit = takeProfit;
    p.openedAt = nowMs;
    p.openFee = fee;
    p.lastPrice = marketPrice;
    p.confidence = confidence;
    p.lead = lead;
    p.reasoning = reasoning;
    m_balance -= fee;
    m_positions.push_back(p);
    mark(nowMs);
    result.ok = true;
    result.position = p;
    return result;
}

Trade PaperAccount::settle(size_t index, double fillPrice, long long nowMs, const std::string &reason)
{
    const Position p = m_positions[index];
    m_positions.erase(m_positions.begin() + static_cast<std::ptrdiff_t>(index));

    const double closeFee = fillPrice * p.quantity * m_settings.feePct / 100.0;
    const double gross = grossPnl(p, fillPrice);
    m_balance += gross - closeFee;

    Trade t;
    t.id = p.id;
    t.symbol = p.symbol;
    t.isLong = p.isLong;
    t.quantity = p.quantity;
    t.entry = p.entry;
    t.exit = fillPrice;
    t.openedAt = p.openedAt;
    t.closedAt = nowMs;
    t.fees = p.openFee + closeFee;
    t.pnl = gross - t.fees; // the opening fee left the balance when the position opened
    t.pnlPct = t.pnl / (p.entry * p.quantity) * 100.0;
    t.reason = reason;
    t.confidence = p.confidence;
    t.lead = p.lead;
    t.reasoning = p.reasoning;
    m_trades.push_back(t);
    return t;
}

std::vector<Trade> PaperAccount::onPrice(const std::string &symbol, double last, double high, double low,
                                         long long nowMs)
{
    std::vector<Trade> closed;
    if (!usable(last))
        return closed;
    if (!usable(high) || high < last)
        high = last;
    if (!usable(low) || low > last)
        low = last;

    const double slip = m_settings.slippagePct / 100.0;
    for (size_t i = 0; i < m_positions.size(); ++i) {
        Position &p = m_positions[i];
        if (p.symbol != symbol)
            continue;
        p.lastPrice = last;
        const bool stopHit = p.isLong ? low <= p.stop : high >= p.stop;
        const bool targetHit = p.isLong ? high >= p.takeProfit : low <= p.takeProfit;
        if (stopHit) {
            // A stop becomes a market order: it fills at the stop or, when
            // the price has already gone past it, at the price, and then
            // slips further.
            const double trigger = p.isLong ? std::min(p.stop, last) : std::max(p.stop, last);
            const double fill = trigger * (p.isLong ? 1.0 - slip : 1.0 + slip);
            closed.push_back(settle(i, fill, nowMs, "stop_loss"));
        } else if (targetHit) {
            // A target is a resting limit order: it fills at its price.
            closed.push_back(settle(i, p.takeProfit, nowMs, "take_profit"));
        }
        break; // one position per symbol
    }
    mark(nowMs);
    return closed;
}

bool PaperAccount::close(long long positionId, double marketPrice, long long nowMs, const std::string &reason,
                         Trade *closed)
{
    if (!usable(marketPrice))
        return false;
    const double slip = m_settings.slippagePct / 100.0;
    for (size_t i = 0; i < m_positions.size(); ++i) {
        if (m_positions[i].id != positionId)
            continue;
        const bool isLong = m_positions[i].isLong;
        const Trade t = settle(i, marketPrice * (isLong ? 1.0 - slip : 1.0 + slip), nowMs, reason);
        if (closed)
            *closed = t;
        mark(nowMs);
        return true;
    }
    return false;
}

void PaperAccount::mark(long long nowMs)
{
    const double value = equity();
    const long long today = dayNumber(nowMs);
    if (today != m_day) {
        m_day = today;
        m_dayStartEquity = value;
    }
    if (value > m_peakEquity)
        m_peakEquity = value;
    if (m_peakEquity > 0)
        m_maxDrawdownPct = std::max(m_maxDrawdownPct, (m_peakEquity - value) / m_peakEquity * 100.0);
}

PaperAccount::State PaperAccount::state() const
{
    State s;
    s.settings = m_settings;
    s.balance = m_balance;
    s.peakEquity = m_peakEquity;
    s.dayStartEquity = m_dayStartEquity;
    s.day = m_day;
    s.maxDrawdownPct = m_maxDrawdownPct;
    s.nextId = m_nextId;
    s.positions = m_positions;
    s.trades = m_trades;
    return s;
}

bool PaperAccount::restore(const State &s)
{
    if (!std::isfinite(s.balance) || s.balance < 0 || !usable(s.settings.startingBalance))
        return false;
    long long highestId = 0;
    for (const Position &p : s.positions) {
        if (p.symbol.empty() || !usable(p.quantity) || !usable(p.entry) || !usable(p.stop) || !usable(p.takeProfit))
            return false;
        highestId = std::max(highestId, p.id);
    }
    for (const Trade &t : s.trades) {
        if (!std::isfinite(t.pnl))
            return false;
        highestId = std::max(highestId, t.id);
    }
    m_settings = s.settings;
    m_balance = s.balance;
    m_positions = s.positions;
    m_trades = s.trades;
    m_peakEquity = usable(s.peakEquity) ? std::max(s.peakEquity, equity()) : equity();
    m_dayStartEquity = usable(s.dayStartEquity) ? s.dayStartEquity : equity();
    m_day = s.day;
    m_maxDrawdownPct = std::isfinite(s.maxDrawdownPct) ? std::max(0.0, s.maxDrawdownPct) : 0;
    // Never hand out an id twice, whatever the file said.
    m_nextId = std::max(s.nextId, highestId + 1);
    return true;
}

} // namespace Trading
