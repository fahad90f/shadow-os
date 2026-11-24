// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../skills/indicators.h"
#include "../skills/smc.h"

#include <string>
#include <vector>

// The decision core: what each analyst said (Verdict), what they say together
// (Signal), and the rule-based analyst that needs no model at all. Plain
// C++17, no Qt, so every rule here is unit-tested with nothing but a compiler.
namespace Trading {

enum class Action { Hold, Buy, Sell };
const char *actionName(Action action); // "hold" | "buy" | "sell"
// Lenient: any letter case, surrounding spaces, and "long"/"short" as used by
// some models. Anything else is Hold.
Action actionFromName(const std::string &name);

// One analyst's opinion on one market snapshot.
struct Verdict
{
    std::string source;    // "rules", "openai", "anthropic", "gemini", "nvidia"
    bool ok = false;       // false: this analyst gave no usable answer
    std::string error;     // why not, when !ok
    Action action = Action::Hold;
    double confidence = 0; // 0..100: how strongly its evidence supports the action
    double stop = 0;       // price levels; 0 = not given
    double takeProfit = 0;
    std::string reasoning;
    long long ms = 0;      // how long the analyst took
};

// A stop on the losing side of the entry and a target on the winning side,
// all positive and finite.
bool levelsValid(Action action, double entry, double stop, double takeProfit);
// Reward per unit of risk; 0 when the levels are not valid.
double rewardRisk(Action action, double entry, double stop, double takeProfit);
// Levels from volatility alone: the stop 1.5 ATR from the entry, the target
// 'ratio' times that distance on the other side. False when atr <= 0.
bool volatilityLevels(Action action, double entry, double atr, double ratio, double *stop, double *takeProfit);

// What the analysts say together.
struct Signal
{
    Action action = Action::Hold;
    // (confidence of the agreeing analysts - confidence of the opposing ones)
    // / number of analysts, 0..100. It measures how strongly and how
    // unanimously the analysts agree. It is NOT a probability of profit.
    double confidence = 0;
    double entry = 0;      // the market price the decision was made at
    double stop = 0;
    double takeProfit = 0;
    double rewardRisk = 0;
    int voters = 0;        // analysts that answered; agreeing + opposing + holding
    int agreeing = 0;      // on the leaning side (on an exact tie: the buyers)
    int opposing = 0;      // on the other side (on an exact tie: the sellers)
    int holding = 0;
    Action leaning = Action::Hold; // the side with more weight, even when the action is Hold
    std::string lead;      // the most confident agreeing analyst
    std::string reasoning; // its reasoning
    std::string note;      // for a Hold: why there is no trade
    std::vector<Verdict> verdicts; // every analyst, the failed ones included
};

struct ConsensusInput
{
    double price = 0;          // current market price
    double atr = 0;            // for levels when no agreeing analyst gave valid ones
    double minRewardRisk = 1.5;
    int minAgreeing = 2;       // at most the number of analysts that answered
};
Signal consensus(const std::vector<Verdict> &verdicts, const ConsensusInput &input);

// The rule-based analyst: a fixed scoring of the indicator and structure
// readings. It always answers, so the application works without any model
// key. 'higher' is the same reading on a higher timeframe, or null.
Verdict rulesVerdict(const Indicators::Summary &ta, const Indicators::Summary *higher, const Smc::Analysis &smc,
                     double minRewardRisk);

} // namespace Trading
