// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "signal.h"

#include <string>
#include <vector>

// The "Risk Engine" skill. Every order passes through evaluate(); no model
// can talk its way past it, because no model takes part in it: these are
// fixed arithmetic rules on the signal and the account.
namespace Trading {

struct RiskSettings
{
    double riskPerTradePct = 1.0;   // of equity, lost if the stop is hit (less when the size is capped)
    double maxDailyLossPct = 3.0;   // of the day's starting equity; then no new trades today
    double maxDrawdownPct = 10.0;   // from peak equity; then trading halts until the user resumes
    double minRewardRisk = 1.5;
    double minConfidence = 60.0;    // consensus score, 0..100
    int minAgreeing = 2;            // analysts that must agree (at most those that answered)
    int maxOpenPositions = 3;       // also: one position uses at most 1/N of the account
    int cooldownMinutes = 30;       // after a losing trade on the same symbol
    double maxLeverage = 1.0;       // position value / available balance
    double minNotional = 10.0;      // smallest order worth placing, in quote currency
    double minStopPct = 0.05;       // a stop closer than this to the entry is noise
    double maxStopPct = 10.0;       // a stop further than this is not a stop
};

// What the risk rules need to know about the account.
struct AccountView
{
    double equity = 0;
    double available = 0;       // balance not tied up in open positions
    double dayStartEquity = 0;
    double peakEquity = 0;
    double realizedToday = 0;   // net profit of the trades closed today
    int openPositions = 0;
    bool positionOnSymbol = false;
    long long lastLossOnSymbolMs = 0; // close time of the last losing trade on this symbol, 0 = none
    bool halted = false;        // the drawdown limit was hit and the user has not resumed
    // The market of the signal has the account's currency as its base
    // (USD/CHF on a dollar account): see notionalOf() in paper.h.
    bool inverse = false;
};

struct RiskCheck
{
    std::string key;    // stable id, e.g. "confidence"
    std::string label;  // shown to the user
    bool pass = false;
    std::string detail; // the numbers behind the result
};

struct RiskDecision
{
    bool allowed = false;
    std::string reason;   // the first failed check, for a one-line explanation
    double quantity = 0;  // base-currency amount to trade
    double notional = 0;  // the order's value in the account's currency
    double riskAmount = 0; // lost if the stop is hit (before fees)
    double rewardRisk = 0;
    // The size was cut to what the balance covers or to this position's share
    // of the account (equity / maxOpenPositions), so less is at risk than
    // riskPerTradePct.
    bool reducedByBalance = false;
    std::vector<RiskCheck> checks; // every rule, passed or not, in a fixed order
};

double drawdownPct(double equity, double peakEquity);
// True when the day's loss limit is reached (new trades are refused until the
// next UTC day).
bool dailyLossReached(const AccountView &account, const RiskSettings &settings);
// True when the drawdown limit is reached (the caller halts trading).
bool drawdownReached(const AccountView &account, const RiskSettings &settings);

RiskDecision evaluate(const Signal &signal, const AccountView &account, const RiskSettings &settings, long long nowMs);

} // namespace Trading
