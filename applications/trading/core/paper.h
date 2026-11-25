// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

// The paper account: orders are simulated against live prices, with fees and
// slippage, and no order ever leaves the computer. It is the "Execution
// Engine" behind the Paper Trading mode. Plain C++17, no Qt.
//
// The account works like a margin account at 1x: opening a position (long or
// short) reserves its value from the available balance; profit and loss are
// settled into the balance when the position closes.
namespace Trading {

struct PaperSettings
{
    double startingBalance = 10000.0; // quote currency (USDT)
    double feePct = 0.10;      // per side, of the order value
    double slippagePct = 0.02; // market and stop orders fill this much worse than the price
};

// What a quantity of the base currency is worth, and what a price move earns,
// in the account's currency. Two kinds of market:
//  - linear: the price is in the account's currency (BTC/USDT on a USDT
//    account, EUR/USD on a dollar account). Value = price x quantity.
//  - inverse: the account's currency is the base, and the price is in
//    another currency (USD/CHF on a dollar account). The quantity is already
//    in the account's currency, and a profit in the quote currency is turned
//    back at the price it is taken at.
inline double notionalOf(bool inverse, double price, double quantity)
{
    return inverse ? quantity : price * quantity;
}
// For a long position; a short earns the opposite.
inline double moveValue(bool inverse, double entry, double price, double quantity)
{
    return inverse ? (price - entry) * quantity / price : (price - entry) * quantity;
}

struct Position
{
    long long id = 0;
    std::string symbol;
    bool isLong = true;
    bool inverse = false;   // see notionalOf()
    double quantity = 0;
    double entry = 0;       // fill price
    double stop = 0;
    double takeProfit = 0;
    long long openedAt = 0; // ms since the epoch
    double openFee = 0;
    double lastPrice = 0;   // most recent price seen, for unrealized profit
    double confidence = 0;  // of the signal that opened it
    std::string lead;       // the analyst that led that signal
    std::string reasoning;
};

struct Trade
{
    long long id = 0;
    std::string symbol;
    bool isLong = true;
    bool inverse = false;
    double quantity = 0;
    double entry = 0;
    double exit = 0;
    long long openedAt = 0;
    long long closedAt = 0;
    double fees = 0;     // both sides
    double pnl = 0;      // net of fees
    double pnlPct = 0;   // pnl / the order's value * 100
    std::string reason;  // "take_profit" | "stop_loss" | "manual" | "signal_flip" | "risk_halt"
    double confidence = 0;
    std::string lead;
    std::string reasoning;
};

struct PaperStats
{
    int trades = 0;
    int wins = 0;
    int losses = 0;        // pnl <= 0
    double winRatePct = 0; // 0 with no trades
    double totalPnl = 0;   // realized, net
    double grossProfit = 0;
    double grossLoss = 0;  // positive number
    double profitFactor = 0; // grossProfit / grossLoss; 0 when there is no loss yet
    double bestTrade = 0;
    double worstTrade = 0;
    double averageWin = 0;
    double averageLoss = 0; // positive number
    double maxDrawdownPct = 0;
};

class PaperAccount
{
public:
    explicit PaperAccount(const PaperSettings &settings = {});

    const PaperSettings &settings() const { return m_settings; }
    // Fees and slippage of future orders; the balance is untouched.
    void setCosts(double feePct, double slippagePct);
    // Start over with a fresh balance; positions and history are discarded.
    void reset(const PaperSettings &settings);

    double balance() const { return m_balance; }
    double marginInUse() const;
    double available() const;   // balance - margin, never below 0
    double unrealized() const;  // at the last price seen for each position
    double equity() const { return m_balance + unrealized(); }
    double peakEquity() const { return m_peakEquity; }
    double dayStartEquity() const { return m_dayStartEquity; }
    double realizedOn(long long dayNumber) const; // net profit of trades closed on that UTC day
    static long long dayNumber(long long ms) { return ms / 86400000LL; }

    const std::vector<Position> &positions() const { return m_positions; }
    const std::vector<Trade> &trades() const { return m_trades; } // oldest first
    const Position *positionOn(const std::string &symbol) const;
    // Close time of the last losing trade on 'symbol', 0 when there is none.
    long long lastLossOn(const std::string &symbol) const;
    PaperStats stats() const;

    struct OpenResult
    {
        bool ok = false;
        std::string error;
        Position position;
    };
    // A market order at 'marketPrice'. Refused when the quantity or price is
    // not positive, the levels are on the wrong side of the fill price, a
    // position on the symbol exists, or the balance does not cover the order
    // value plus its fee.
    OpenResult open(const std::string &symbol, bool isLong, double quantity, double marketPrice, double stop,
                    double takeProfit, long long nowMs, double confidence = 0, const std::string &lead = {},
                    const std::string &reasoning = {}, bool inverse = false);

    // A new price for 'symbol': 'last' is the current price, 'high' and 'low'
    // the extremes since the previous update. Closes the position on the
    // symbol if its stop or target was reached, the stop first when both were
    // (the cautious assumption), and returns the trades this produced.
    std::vector<Trade> onPrice(const std::string &symbol, double last, double high, double low, long long nowMs);

    // Close at the market. False when no such position exists or the price is
    // not usable.
    bool close(long long positionId, double marketPrice, long long nowMs, const std::string &reason,
               Trade *closed = nullptr);

    // Roll the day and track the equity peak and the deepest drawdown. Call
    // after prices change.
    void mark(long long nowMs);

    // Everything needed to continue after a restart.
    struct State
    {
        PaperSettings settings;
        double balance = 0;
        double peakEquity = 0;
        double dayStartEquity = 0;
        long long day = 0;
        double maxDrawdownPct = 0;
        long long nextId = 1;
        std::vector<Position> positions;
        std::vector<Trade> trades;
    };
    State state() const;
    // False (and the account is left as it was) when the state is not sane:
    // a negative or non-finite balance, or a position without quantity.
    bool restore(const State &state);

private:
    Trade settle(size_t index, double fillPrice, long long nowMs, const std::string &reason);

    PaperSettings m_settings;
    double m_balance = 0;
    double m_peakEquity = 0;
    double m_dayStartEquity = 0;
    long long m_day = 0;
    double m_maxDrawdownPct = 0;
    long long m_nextId = 1;
    std::vector<Position> m_positions;
    std::vector<Trade> m_trades;
};

} // namespace Trading
