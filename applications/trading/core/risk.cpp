// SPDX-License-Identifier: GPL-3.0-or-later
#include "risk.h"

#include "paper.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace Trading {

namespace {

// A number with a fixed count of decimals.
std::string num(double value, int decimals)
{
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.*f", decimals, value);
    return buffer;
}

} // namespace

double drawdownPct(double equity, double peakEquity)
{
    if (!(peakEquity > 0) || !std::isfinite(equity))
        return 0;
    return std::max(0.0, (peakEquity - equity) / peakEquity * 100.0);
}

bool dailyLossReached(const AccountView &account, const RiskSettings &settings)
{
    if (!(account.dayStartEquity > 0))
        return false;
    // Open losses count too: equity already includes them.
    const double lossPct = (account.dayStartEquity - account.equity) / account.dayStartEquity * 100.0;
    return lossPct >= settings.maxDailyLossPct;
}

bool drawdownReached(const AccountView &account, const RiskSettings &settings)
{
    return drawdownPct(account.equity, account.peakEquity) >= settings.maxDrawdownPct;
}

RiskDecision evaluate(const Signal &signal, const AccountView &account, const RiskSettings &settings, long long nowMs)
{
    RiskDecision decision;
    const auto check = [&decision](const char *key, const char *label, bool pass, const std::string &detail) {
        decision.checks.push_back({key, label, pass, detail});
        if (!pass && decision.reason.empty())
            decision.reason = std::string(label) + ": " + detail;
        return pass;
    };

    const bool directional = signal.action != Action::Hold;
    check("signal", "Signal", directional,
          directional ? std::string(signal.action == Action::Buy ? "Buy" : "Sell")
                      : (signal.note.empty() ? std::string("No trade signal") : signal.note));

    check("confidence", "Confidence", signal.confidence >= settings.minConfidence,
          num(signal.confidence, 1) + ", needs " + num(settings.minConfidence, 0));

    const int required = std::max(1, std::min(settings.minAgreeing, signal.voters));
    check("agreement", "Analysts agreeing", signal.agreeing >= required && signal.voters > 0,
          num(signal.agreeing, 0) + " agree, " + num(required, 0) + " needed");

    const bool levels = levelsValid(signal.action, signal.entry, signal.stop, signal.takeProfit);
    const double stopPct = levels ? std::fabs(signal.entry - signal.stop) / signal.entry * 100.0 : 0;
    check("stop", "Stop loss", levels && stopPct >= settings.minStopPct && stopPct <= settings.maxStopPct,
          !levels ? std::string("No valid stop and target for this direction")
                  : num(stopPct, 2) + " % from entry (allowed " + num(settings.minStopPct, 2) + " to "
                        + num(settings.maxStopPct, 0) + " %)");

    decision.rewardRisk = rewardRisk(signal.action, signal.entry, signal.stop, signal.takeProfit);
    check("reward", "Reward to risk", decision.rewardRisk >= settings.minRewardRisk,
          num(decision.rewardRisk, 2) + ", needs " + num(settings.minRewardRisk, 2));

    check("positions", "Open positions",
          !account.positionOnSymbol && account.openPositions < settings.maxOpenPositions,
          account.positionOnSymbol ? std::string("Already in a position on this market")
                                   : num(account.openPositions, 0) + " of " + num(settings.maxOpenPositions, 0));

    const double dayLossPct = account.dayStartEquity > 0
        ? (account.dayStartEquity - account.equity) / account.dayStartEquity * 100.0 : 0;
    check("daily_loss", "Daily loss limit", !dailyLossReached(account, settings),
          num(std::max(0.0, dayLossPct), 2) + " % lost today, limit " + num(settings.maxDailyLossPct, 1) + " %");

    const double drawdown = drawdownPct(account.equity, account.peakEquity);
    check("drawdown", "Max drawdown", !account.halted && !drawdownReached(account, settings),
          account.halted ? std::string("Trading is halted until you resume it")
                         : num(drawdown, 2) + " %, limit " + num(settings.maxDrawdownPct, 1) + " %");

    const long long cooldownMs = static_cast<long long>(settings.cooldownMinutes) * 60000;
    const long long since = account.lastLossOnSymbolMs > 0 ? nowMs - account.lastLossOnSymbolMs : cooldownMs;
    const bool cooled = account.lastLossOnSymbolMs <= 0 || since >= cooldownMs;
    check("cooldown", "Cooldown after a loss", cooled,
          cooled ? std::string("Clear")
                 : num(std::ceil(static_cast<double>(cooldownMs - since) / 60000.0), 0) + " min left");

    // Size: lose riskPerTradePct of equity if the stop is hit, but never
    // commit more than the balance (times leverage) allows.
    bool sized = false;
    std::string sizeDetail = "Needs a valid signal";
    if (levels && account.equity > 0) {
        const double riskBudget = account.equity * settings.riskPerTradePct / 100.0;
        // What one unit of the base loses, in the account's currency, when
        // the stop is hit; and what one unit is worth.
        const double perUnit = std::fabs(moveValue(account.inverse, signal.entry, signal.stop, 1.0));
        const double unitValue = notionalOf(account.inverse, signal.entry, 1.0);
        double quantity = riskBudget / perUnit;
        // Two ceilings. What the balance covers: half a percent is kept
        // back so that the fee and the slippage of the fill still fit. And
        // this position's share of the account: with N positions allowed, one
        // of them never takes more than 1/N, so a tight stop cannot put the
        // whole account into a single trade and the others can still open.
        const double share = account.equity * settings.maxLeverage / std::max(1, settings.maxOpenPositions);
        const double maxNotional = std::min(std::max(0.0, account.available) * settings.maxLeverage * 0.995, share);
        if (quantity * unitValue > maxNotional) {
            quantity = maxNotional / unitValue;
            decision.reducedByBalance = true;
        }
        decision.quantity = quantity;
        decision.notional = quantity * unitValue;
        decision.riskAmount = quantity * perUnit;
        sized = decision.notional >= settings.minNotional;
        sizeDetail = sized
            ? num(decision.riskAmount, 2) + " at risk (" + num(decision.riskAmount / account.equity * 100.0, 2)
                  + " % of equity)"
                  + (decision.reducedByBalance ? std::string(", size limited by the account") : std::string())
            : "Order value " + num(decision.notional, 2) + " is below the minimum " + num(settings.minNotional, 2);
    }
    check("size", "Position size", sized, sizeDetail);

    decision.allowed = decision.reason.empty();
    if (!decision.allowed) {
        decision.quantity = 0;
        decision.notional = 0;
        decision.riskAmount = 0;
    }
    return decision;
}

} // namespace Trading
