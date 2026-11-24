// SPDX-License-Identifier: GPL-3.0-or-later
#include "signal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace Trading {

namespace {

// How long structure findings count for the rule-based analyst, in closed
// candles: a change of character within the last 20, a sweep within the last 3.
const int kFreshEvent = 20;
const int kFreshSweep = 3;

bool usable(double v)
{
    return std::isfinite(v) && v > 0;
}

double median(std::vector<double> values)
{
    std::sort(values.begin(), values.end());
    const size_t n = values.size();
    return n % 2 ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2.0;
}

std::string whole(double value)
{
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.0f", value);
    return buffer;
}

std::string signedWhole(double value)
{
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%+.0f", value);
    return buffer;
}

} // namespace

const char *actionName(Action action)
{
    switch (action) {
    case Action::Buy: return "buy";
    case Action::Sell: return "sell";
    case Action::Hold: break;
    }
    return "hold";
}

Action actionFromName(const std::string &name)
{
    std::string s;
    for (const char c : name) {
        if (!std::isspace(static_cast<unsigned char>(c)))
            s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (s == "buy" || s == "long")
        return Action::Buy;
    if (s == "sell" || s == "short")
        return Action::Sell;
    return Action::Hold;
}

bool levelsValid(Action action, double entry, double stop, double takeProfit)
{
    if (!usable(entry) || !usable(stop) || !usable(takeProfit))
        return false;
    if (action == Action::Buy)
        return stop < entry && takeProfit > entry;
    if (action == Action::Sell)
        return stop > entry && takeProfit < entry;
    return false;
}

double rewardRisk(Action action, double entry, double stop, double takeProfit)
{
    if (!levelsValid(action, entry, stop, takeProfit))
        return 0;
    return std::fabs(takeProfit - entry) / std::fabs(entry - stop);
}

bool volatilityLevels(Action action, double entry, double atr, double ratio, double *stop, double *takeProfit)
{
    if (!usable(entry) || !usable(atr) || !(ratio > 0) || action == Action::Hold)
        return false;
    const double risk = 1.5 * atr;
    const double direction = action == Action::Buy ? 1.0 : -1.0;
    const double s = entry - direction * risk;
    const double t = entry + direction * risk * ratio;
    if (!levelsValid(action, entry, s, t))
        return false; // an ATR larger than the price itself: no sane level exists
    *stop = s;
    *takeProfit = t;
    return true;
}

Signal consensus(const std::vector<Verdict> &verdicts, const ConsensusInput &input)
{
    Signal signal;
    signal.verdicts = verdicts;
    signal.entry = input.price;

    double buyScore = 0, sellScore = 0;
    int buyers = 0, sellers = 0;
    for (const Verdict &v : verdicts) {
        if (!v.ok)
            continue;
        ++signal.voters;
        const double confidence = std::clamp(std::isfinite(v.confidence) ? v.confidence : 0.0, 0.0, 100.0);
        if (v.action == Action::Buy) {
            buyScore += confidence;
            ++buyers;
        } else if (v.action == Action::Sell) {
            sellScore += confidence;
            ++sellers;
        } else {
            ++signal.holding;
        }
    }
    if (signal.voters == 0) {
        signal.note = "No analyst produced a verdict.";
        return signal;
    }
    if (buyScore == sellScore) {
        // No side to agree with: the counts still say who stood where
        // (buyers as "agreeing", sellers as "opposing"), so they add up.
        signal.agreeing = buyers;
        signal.opposing = sellers;
        signal.note = buyers + sellers == 0 ? "Every analyst says hold." : "The analysts are evenly split.";
        return signal;
    }

    const Action direction = buyScore > sellScore ? Action::Buy : Action::Sell;
    signal.leaning = direction;
    signal.agreeing = direction == Action::Buy ? buyers : sellers;
    signal.opposing = direction == Action::Buy ? sellers : buyers;
    const double net = std::fabs(buyScore - sellScore);
    signal.confidence = std::clamp(net / signal.voters, 0.0, 100.0);

    // The most confident agreeing analyst speaks for the group.
    const Verdict *lead = nullptr;
    std::vector<double> stops, targets;
    for (const Verdict &v : verdicts) {
        if (!v.ok || v.action != direction)
            continue;
        if (!lead || v.confidence > lead->confidence)
            lead = &v;
        if (levelsValid(direction, input.price, v.stop, v.takeProfit)) {
            stops.push_back(v.stop);
            targets.push_back(v.takeProfit);
        }
    }
    signal.lead = lead->source;
    signal.reasoning = lead->reasoning;

    const int required = std::max(1, std::min(input.minAgreeing, signal.voters));
    if (signal.agreeing < required) {
        signal.note = "Only " + whole(signal.agreeing) + " of " + whole(signal.voters) + " analysts agree.";
        return signal;
    }

    // Levels: the middle of what the agreeing analysts proposed, each of
    // which is valid against the current price, so the medians are too.
    if (!stops.empty()) {
        signal.stop = median(stops);
        signal.takeProfit = median(targets);
    } else if (!volatilityLevels(direction, input.price, input.atr, std::max(input.minRewardRisk, 2.0), &signal.stop,
                                 &signal.takeProfit)) {
        signal.note = "No analyst gave a usable stop and target, and volatility is unknown.";
        return signal;
    }
    signal.rewardRisk = rewardRisk(direction, input.price, signal.stop, signal.takeProfit);
    signal.action = direction;
    return signal;
}

Verdict rulesVerdict(const Indicators::Summary &ta, const Indicators::Summary *higher, const Smc::Analysis &smc,
                     double minRewardRisk)
{
    Verdict verdict;
    verdict.source = "rules";
    if (!ta.valid) {
        verdict.error = ta.reason.empty() ? "Not enough price history." : ta.reason;
        return verdict;
    }
    verdict.ok = true;

    // Each reading adds to or takes from one score; the reasons are kept so
    // the user can see exactly what the rule-based analyst looked at.
    double score = 0;
    std::vector<std::string> reasons;
    const auto add = [&](double points, const char *why) {
        score += points;
        reasons.emplace_back(std::string(points > 0 ? "+ " : "- ") + why);
    };

    if (ta.trend == "bullish")
        add(2, "price above EMA20 above EMA50");
    else if (ta.trend == "bearish")
        add(-2, "price below EMA20 below EMA50");
    if (higher && higher->valid) {
        if (higher->trend == "bullish")
            add(1, "higher timeframe trend up");
        else if (higher->trend == "bearish")
            add(-1, "higher timeframe trend down");
    }
    if (ta.macdState == "rising" && ta.macdHistogram > 0)
        add(1, "MACD histogram positive and rising");
    else if (ta.macdState == "falling" && ta.macdHistogram < 0)
        add(-1, "MACD histogram negative and falling");
    if (ta.momentum == "overbought")
        add(-1, "RSI overbought");
    else if (ta.momentum == "oversold")
        add(1, "RSI oversold");

    if (smc.valid) {
        if (smc.trend == Smc::Trend::Bullish)
            add(1, "market structure bullish");
        else if (smc.trend == Smc::Trend::Bearish)
            add(-1, "market structure bearish");
        // A change of character counts while it is the latest event and
        // recent; on a real market the last one can be days old.
        if (!smc.events.empty()) {
            const Smc::StructureEvent &last = smc.events.back();
            if (last.changeOfCharacter && Smc::age(smc, last.index) < kFreshEvent)
                add(last.bullish ? 1 : -1, last.bullish ? "recent bullish change of character"
                                                         : "recent bearish change of character");
        }
        if (smc.zone == "discount" && smc.trend == Smc::Trend::Bullish)
            add(1, "price in the discount half of a bullish range");
        else if (smc.zone == "premium" && smc.trend == Smc::Trend::Bearish)
            add(-1, "price in the premium half of a bearish range");
        // Wicks through old levels are common, so a sweep counts only on the
        // newest candles, and not at all when both sides were just taken.
        int lowsTaken = 0, highsTaken = 0; // the bullish and the bearish sweeps
        Smc::recentSweeps(smc, kFreshSweep, &lowsTaken, &highsTaken);
        if (lowsTaken > 0 && highsTaken == 0)
            add(1, "sell-side liquidity just swept");
        else if (highsTaken > 0 && lowsTaken == 0)
            add(-1, "buy-side liquidity just swept");
    }

    const double threshold = 3;
    if (score >= threshold)
        verdict.action = Action::Buy;
    else if (score <= -threshold)
        verdict.action = Action::Sell;
    // 3 points is the weakest signal that trades (55); 8 points or more is
    // the strongest (95).
    verdict.confidence = verdict.action == Action::Hold
        ? 0.0 : std::min(95.0, 55.0 + (std::fabs(score) - threshold) * 8.0);

    if (verdict.action != Action::Hold
        && !volatilityLevels(verdict.action, ta.price, ta.atr14, std::max(minRewardRisk, 2.0), &verdict.stop,
                             &verdict.takeProfit)) {
        verdict.action = Action::Hold;
        verdict.confidence = 0;
        reasons.emplace_back("volatility unknown, no levels");
    }

    verdict.reasoning = "Rule score " + signedWhole(score) + " (trades at +/-" + whole(threshold) + ").";
    for (const std::string &reason : reasons)
        verdict.reasoning += " " + reason + ";";
    if (!verdict.reasoning.empty() && verdict.reasoning.back() == ';')
        verdict.reasoning.back() = '.';
    return verdict;
}

} // namespace Trading
