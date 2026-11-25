// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests of the decision core (consensus, rule-based analyst, risk rules,
// paper account). No framework; from applications/trading:
//
//   g++ -std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Wshadow -Werror -fsanitize=address,undefined
//       core/signal.cpp core/risk.cpp core/paper.cpp tests/test_core.cpp -o /tmp/test_core
//   /tmp/test_core
#include "../core/paper.h"
#include "../core/risk.h"
#include "../core/signal.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

using namespace Trading;

static int g_checks = 0;
static int g_failed = 0;

#define CHECK(cond) \
    do { \
        ++g_checks; \
        if (!(cond)) { \
            ++g_failed; \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

#define NEAR(a, b, eps) \
    do { \
        ++g_checks; \
        const double near_a = (a), near_b = (b); \
        if (!(std::fabs(near_a - near_b) <= (eps))) { \
            ++g_failed; \
            std::printf("FAIL %s:%d: %s = %.10g, expected %.10g\n", __FILE__, __LINE__, #a, near_a, near_b); \
        } \
    } while (0)

static Verdict vote(const char *source, Action action, double confidence, double stop = 0, double target = 0)
{
    Verdict v;
    v.source = source;
    v.ok = true;
    v.action = action;
    v.confidence = confidence;
    v.stop = stop;
    v.takeProfit = target;
    v.reasoning = std::string("because ") + source;
    return v;
}

static Verdict failed(const char *source)
{
    Verdict v;
    v.source = source;
    v.error = "no key";
    return v;
}

static void testNamesAndLevels()
{
    CHECK(actionFromName("BUY") == Action::Buy);
    CHECK(actionFromName(" Long ") == Action::Buy);
    CHECK(actionFromName("sell") == Action::Sell);
    CHECK(actionFromName("SHORT") == Action::Sell);
    CHECK(actionFromName("hold") == Action::Hold);
    CHECK(actionFromName("") == Action::Hold);
    CHECK(actionFromName("buy now!") == Action::Hold);
    CHECK(std::string(actionName(Action::Buy)) == "buy");
    CHECK(std::string(actionName(Action::Sell)) == "sell");
    CHECK(std::string(actionName(Action::Hold)) == "hold");

    CHECK(levelsValid(Action::Buy, 100, 95, 110));
    CHECK(!levelsValid(Action::Buy, 100, 105, 110)); // stop above a long entry
    CHECK(!levelsValid(Action::Buy, 100, 95, 99));   // target below a long entry
    CHECK(!levelsValid(Action::Buy, 100, 100, 110)); // stop at the entry
    CHECK(levelsValid(Action::Sell, 100, 105, 90));
    CHECK(!levelsValid(Action::Sell, 100, 95, 90));
    CHECK(!levelsValid(Action::Sell, 100, 105, 101));
    CHECK(!levelsValid(Action::Hold, 100, 95, 110));
    CHECK(!levelsValid(Action::Buy, 100, 0, 110));
    CHECK(!levelsValid(Action::Buy, 100, -5, 110));
    CHECK(!levelsValid(Action::Buy, 100, std::numeric_limits<double>::quiet_NaN(), 110));
    CHECK(!levelsValid(Action::Buy, 100, 95, std::numeric_limits<double>::infinity()));

    NEAR(rewardRisk(Action::Buy, 100, 95, 110), 2.0, 1e-12);
    NEAR(rewardRisk(Action::Sell, 100, 104, 94), 1.5, 1e-12);
    NEAR(rewardRisk(Action::Buy, 100, 105, 110), 0.0, 0);

    double stop = 0, target = 0;
    CHECK(volatilityLevels(Action::Buy, 100, 2, 2.0, &stop, &target));
    NEAR(stop, 97, 1e-12);
    NEAR(target, 106, 1e-12);
    CHECK(volatilityLevels(Action::Sell, 100, 2, 3.0, &stop, &target));
    NEAR(stop, 103, 1e-12);
    NEAR(target, 91, 1e-12);
    CHECK(!volatilityLevels(Action::Hold, 100, 2, 2.0, &stop, &target));
    CHECK(!volatilityLevels(Action::Buy, 100, 0, 2.0, &stop, &target));
    CHECK(!volatilityLevels(Action::Buy, 100, 80, 2.0, &stop, &target)); // stop would be negative
    CHECK(!volatilityLevels(Action::Sell, 100, 40, 2.0, &stop, &target)); // target would be negative
}

static void testConsensus()
{
    ConsensusInput in;
    in.price = 100;
    in.atr = 2;
    in.minRewardRisk = 1.5;
    in.minAgreeing = 2;

    { // nobody answered
        const Signal s = consensus({failed("openai"), failed("gemini")}, in);
        CHECK(s.action == Action::Hold);
        CHECK(s.voters == 0);
        CHECK(!s.note.empty());
        CHECK(s.verdicts.size() == 2);
    }
    { // three buy at 80, one holds: (240 - 0) / 4 = 60
        const Signal s = consensus({vote("rules", Action::Buy, 80, 96, 108), vote("openai", Action::Buy, 80, 97, 109),
                                    vote("anthropic", Action::Buy, 80, 95, 110), vote("gemini", Action::Hold, 0)}, in);
        CHECK(s.action == Action::Buy);
        NEAR(s.confidence, 60, 1e-12);
        CHECK(s.voters == 4 && s.agreeing == 3 && s.opposing == 0 && s.holding == 1);
        NEAR(s.stop, 96, 1e-12);        // median of 95, 96, 97
        NEAR(s.takeProfit, 109, 1e-12); // median of 108, 109, 110
        NEAR(s.rewardRisk, 9.0 / 4.0, 1e-12);
        NEAR(s.entry, 100, 0);
    }
    { // an opposing vote takes weight away: (240 - 70) / 4 = 42.5
        const Signal s = consensus({vote("a", Action::Buy, 80, 96, 108), vote("b", Action::Buy, 80, 96, 108),
                                    vote("c", Action::Buy, 80, 96, 108), vote("d", Action::Sell, 70, 104, 92)}, in);
        CHECK(s.action == Action::Buy);
        NEAR(s.confidence, 42.5, 1e-12);
        CHECK(s.opposing == 1);
    }
    { // sell side wins; the lead is the most confident seller
        const Signal s = consensus({vote("a", Action::Sell, 60, 104, 92), vote("b", Action::Sell, 90, 103, 94),
                                    vote("c", Action::Buy, 50, 96, 108)}, in);
        CHECK(s.action == Action::Sell);
        CHECK(s.lead == "b");
        CHECK(s.reasoning == "because b");
        NEAR(s.confidence, 100.0 / 3.0, 1e-9);
        NEAR(s.stop, 103.5, 1e-12);
        NEAR(s.takeProfit, 93, 1e-12);
    }
    { // one buyer among four: not enough agree
        const Signal s = consensus({vote("a", Action::Buy, 90, 96, 108), vote("b", Action::Hold, 0),
                                    vote("c", Action::Hold, 0), vote("d", Action::Hold, 0)}, in);
        CHECK(s.action == Action::Hold);
        CHECK(s.leaning == Action::Buy);
        CHECK(s.agreeing == 1);
        CHECK(!s.note.empty());
        NEAR(s.confidence, 22.5, 1e-12);
    }
    { // a single analyst (no model keys): its word is enough
        const Signal s = consensus({vote("rules", Action::Buy, 71, 97, 106), failed("openai")}, in);
        CHECK(s.action == Action::Buy);
        CHECK(s.voters == 1 && s.agreeing == 1);
        NEAR(s.confidence, 71, 1e-12);
    }
    { // an even split
        const Signal s = consensus({vote("a", Action::Buy, 70, 96, 108), vote("b", Action::Sell, 70, 104, 92)}, in);
        CHECK(s.action == Action::Hold);
        CHECK(s.leaning == Action::Hold);
        CHECK(!s.note.empty());
        CHECK(s.agreeing == 1 && s.opposing == 1 && s.holding == 0 && s.voters == 2);
    }
    { // the counts always add up to the analysts that answered
        const std::vector<std::vector<Verdict>> panels = {
            {vote("a", Action::Buy, 80, 96, 108), vote("b", Action::Sell, 70, 104, 92), vote("c", Action::Hold, 0), failed("d")},
            {vote("a", Action::Hold, 0), vote("b", Action::Hold, 0)},
            {vote("a", Action::Sell, 60, 104, 92), vote("b", Action::Sell, 60, 104, 92), vote("c", Action::Buy, 10, 96, 108)},
        };
        for (const std::vector<Verdict> &panel : panels) {
            const Signal s = consensus(panel, in);
            CHECK(s.agreeing + s.opposing + s.holding == s.voters);
        }
    }
    { // everybody holds
        const Signal s = consensus({vote("a", Action::Hold, 0), vote("b", Action::Hold, 0)}, in);
        CHECK(s.action == Action::Hold);
        CHECK(s.holding == 2);
    }
    { // levels on the wrong side are ignored; the valid ones are used
        const Signal s = consensus({vote("a", Action::Buy, 80, 105, 110), vote("b", Action::Buy, 80, 96, 108)}, in);
        CHECK(s.action == Action::Buy);
        NEAR(s.stop, 96, 1e-12);
        NEAR(s.takeProfit, 108, 1e-12);
    }
    { // no analyst gave levels: 1.5 ATR stop, target at max(minRR, 2) times that
        const Signal s = consensus({vote("a", Action::Sell, 80), vote("b", Action::Sell, 80)}, in);
        CHECK(s.action == Action::Sell);
        NEAR(s.stop, 103, 1e-12);
        NEAR(s.takeProfit, 94, 1e-12);
        NEAR(s.rewardRisk, 2.0, 1e-12);
    }
    { // no levels and no volatility: no trade
        ConsensusInput bare = in;
        bare.atr = 0;
        const Signal s = consensus({vote("a", Action::Buy, 80), vote("b", Action::Buy, 80)}, bare);
        CHECK(s.action == Action::Hold);
        CHECK(!s.note.empty());
    }
    { // confidence out of range or not a number is clamped, never propagated
        const Signal s = consensus({vote("a", Action::Buy, 500, 96, 108),
                                    vote("b", Action::Buy, std::numeric_limits<double>::quiet_NaN(), 96, 108)}, in);
        CHECK(s.action == Action::Buy);
        NEAR(s.confidence, 50, 1e-12);
    }
}

static Indicators::Summary summary(const char *trend)
{
    Indicators::Summary s;
    s.valid = true;
    s.price = 100;
    s.atr14 = 2;
    s.trend = trend;
    s.momentum = "neutral";
    s.macdState = "flat";
    s.volumeState = "normal";
    return s;
}

static void testRules()
{
    Smc::Analysis none; // valid = false: structure is not counted

    { // not enough history
        Indicators::Summary ta;
        ta.reason = "Only 12 closed candles.";
        const Verdict v = rulesVerdict(ta, nullptr, none, 1.5);
        CHECK(!v.ok);
        CHECK(v.error == "Only 12 closed candles.");
    }
    { // trend alone (2 points) does not trade
        const Verdict v = rulesVerdict(summary("bullish"), nullptr, none, 1.5);
        CHECK(v.ok);
        CHECK(v.action == Action::Hold);
        NEAR(v.confidence, 0, 0);
        CHECK(v.reasoning.find("+2") != std::string::npos);
    }
    { // trend + higher timeframe = 3 points: the weakest buy, 55
        const Indicators::Summary higher = summary("bullish");
        const Verdict v = rulesVerdict(summary("bullish"), &higher, none, 1.5);
        CHECK(v.action == Action::Buy);
        NEAR(v.confidence, 55, 1e-12);
        NEAR(v.stop, 97, 1e-12);
        NEAR(v.takeProfit, 106, 1e-12);
    }
    { // everything bearish
        Indicators::Summary ta = summary("bearish");
        ta.macdState = "falling";
        ta.macdHistogram = -0.4;
        const Indicators::Summary higher = summary("bearish");
        Smc::Analysis smc;
        smc.valid = true;
        smc.closedCandles = 100; // the newest closed candle is number 99
        smc.trend = Smc::Trend::Bearish;
        smc.zone = "premium";
        Smc::StructureEvent event;
        event.index = 80; // 19 candles ago: the oldest that still counts
        event.bullish = false;
        event.changeOfCharacter = true;
        smc.events.push_back(event);
        Smc::Sweep sweep;
        sweep.index = 97; // 2 candles ago: the oldest that still counts
        sweep.bullish = false;
        smc.sweeps.push_back(sweep);
        const Verdict v = rulesVerdict(ta, &higher, smc, 3.0);
        CHECK(v.action == Action::Sell);
        // -2 -1 -1 -1 -1 -1 -1 = -8: 55 + 5 * 8 = 95
        NEAR(v.confidence, 95, 1e-12);
        NEAR(v.stop, 103, 1e-12);
        NEAR(v.takeProfit, 91, 1e-12); // ratio 3
        CHECK(v.reasoning.find("-8") != std::string::npos);

        // On a real market the last change of character can be days old and
        // old levels are wicked through all the time: neither counts then.
        Smc::Analysis stale = smc;
        stale.events[0].index = 79; // 20 candles ago
        const Verdict oldEvent = rulesVerdict(ta, &higher, stale, 3.0);
        NEAR(oldEvent.confidence, 87, 1e-12); // -7
        CHECK(oldEvent.reasoning.find("change of character") == std::string::npos);
        stale.sweeps[0].index = 96; // 3 candles ago
        const Verdict oldSweep = rulesVerdict(ta, &higher, stale, 3.0);
        NEAR(oldSweep.confidence, 79, 1e-12); // -6
        CHECK(oldSweep.reasoning.find("swept") == std::string::npos);

        // Fresh sweeps of both sides say nothing about direction.
        Smc::Analysis both = smc;
        Smc::Sweep lows;
        lows.index = 99;
        lows.bullish = true;
        both.sweeps.push_back(lows);
        const Verdict twoSided = rulesVerdict(ta, &higher, both, 3.0);
        NEAR(twoSided.confidence, 87, 1e-12); // -7: the sweep point is gone, nothing is added
        CHECK(twoSided.reasoning.find("swept") == std::string::npos);
        // A fresh sweep of the lows alone is a point for the buyers.
        both.sweeps.erase(both.sweeps.begin());
        const Verdict lowsOnly = rulesVerdict(ta, &higher, both, 3.0);
        NEAR(lowsOnly.confidence, 79, 1e-12); // -7 + 1 = -6
        CHECK(lowsOnly.reasoning.find("+ sell-side liquidity just swept") != std::string::npos);
    }
    { // an overbought market takes a point off a bullish reading
        Indicators::Summary ta = summary("bullish");
        ta.momentum = "overbought";
        const Indicators::Summary higher = summary("bullish");
        const Verdict v = rulesVerdict(ta, &higher, none, 1.5);
        CHECK(v.action == Action::Hold); // 2 + 1 - 1 = 2
    }
    { // a rising histogram counts only above zero
        Indicators::Summary ta = summary("bullish");
        ta.macdState = "rising";
        ta.macdHistogram = -0.1;
        const Indicators::Summary higher = summary("bullish");
        const Verdict below = rulesVerdict(ta, &higher, none, 1.5);
        NEAR(below.confidence, 55, 1e-12); // 3 points
        ta.macdHistogram = 0.1;
        const Verdict above = rulesVerdict(ta, &higher, none, 1.5);
        NEAR(above.confidence, 63, 1e-12); // 4 points
    }
    { // a trade without volatility has no levels, so it is not a trade
        Indicators::Summary ta = summary("bullish");
        ta.atr14 = 0;
        const Indicators::Summary higher = summary("bullish");
        const Verdict v = rulesVerdict(ta, &higher, none, 1.5);
        CHECK(v.action == Action::Hold);
        NEAR(v.confidence, 0, 0);
    }
    { // an invalid higher timeframe is ignored
        Indicators::Summary higher;
        const Verdict v = rulesVerdict(summary("bullish"), &higher, none, 1.5);
        CHECK(v.action == Action::Hold);
    }
}

static Signal buySignal()
{
    Signal s;
    s.action = Action::Buy;
    s.confidence = 75;
    s.entry = 100;
    s.stop = 98;
    s.takeProfit = 106;
    s.voters = 4;
    s.agreeing = 3;
    return s;
}

static AccountView freshAccount()
{
    AccountView a;
    a.equity = 10000;
    a.available = 10000;
    a.dayStartEquity = 10000;
    a.peakEquity = 10000;
    return a;
}

static const RiskCheck *find(const RiskDecision &d, const char *key)
{
    for (const RiskCheck &c : d.checks) {
        if (c.key == key)
            return &c;
    }
    return nullptr;
}

static void testRisk()
{
    const RiskSettings settings;
    const long long now = 1700000000000LL;

    { // the plain case: 1 % of 10 000 at risk over a 4-point stop = 25 units
        Signal s = buySignal();
        s.stop = 96;
        s.takeProfit = 112;
        const RiskDecision d = evaluate(s, freshAccount(), settings, now);
        CHECK(d.allowed);
        CHECK(d.reason.empty());
        NEAR(d.quantity, 25, 1e-9);
        NEAR(d.notional, 2500, 1e-9);
        NEAR(d.riskAmount, 100, 1e-9);
        NEAR(d.rewardRisk, 3.0, 1e-12);
        CHECK(!d.reducedByBalance);
        CHECK(d.checks.size() == 10);
        for (const RiskCheck &c : d.checks)
            CHECK(c.pass);
    }
    { // a 2-point stop wants 5 000, more than a third of the account: cut to the share
        const RiskDecision d = evaluate(buySignal(), freshAccount(), settings, now);
        CHECK(d.allowed);
        CHECK(d.reducedByBalance);
        NEAR(d.notional, 10000.0 / 3.0, 1e-6);
        NEAR(d.quantity, 100.0 / 3.0, 1e-9);
        NEAR(d.riskAmount, 200.0 / 3.0, 1e-9);
        CHECK(find(d, "size")->detail.find("limited by the account") != std::string::npos);
    }
    { // the share follows the number of positions allowed
        RiskSettings two = settings;
        two.maxOpenPositions = 2;
        NEAR(evaluate(buySignal(), freshAccount(), two, now).notional, 5000, 1e-6); // exactly the risk size
        CHECK(!evaluate(buySignal(), freshAccount(), two, now).reducedByBalance);
        RiskSettings one = settings;
        one.maxOpenPositions = 1;
        Signal tight = buySignal();
        tight.stop = 99.9; // 0.1 away: 100 000 wanted
        tight.takeProfit = 100.3;
        const RiskDecision d = evaluate(tight, freshAccount(), one, now);
        CHECK(d.allowed);
        CHECK(d.reducedByBalance);
        NEAR(d.notional, 9950, 1e-6); // one position may take it all: 99.5 % of the balance
        NEAR(d.riskAmount, 9.95, 1e-9);
    }
    { // what is left of the balance is the lower ceiling
        AccountView a = freshAccount();
        a.available = 1000;
        a.openPositions = 2;
        const RiskDecision d = evaluate(buySignal(), a, settings, now);
        CHECK(d.allowed);
        NEAR(d.notional, 995, 1e-6);
    }
    { // hold
        Signal s = buySignal();
        s.action = Action::Hold;
        s.note = "Only 1 of 4 analysts agree.";
        const RiskDecision d = evaluate(s, freshAccount(), settings, now);
        CHECK(!d.allowed);
        CHECK(d.reason.find("Only 1 of 4") != std::string::npos);
        NEAR(d.quantity, 0, 0);
        CHECK(!find(d, "signal")->pass);
    }
    { // confidence one tenth below the bar, and exactly at it
        Signal s = buySignal();
        s.confidence = 59.9;
        CHECK(!evaluate(s, freshAccount(), settings, now).allowed);
        s.confidence = 60;
        CHECK(evaluate(s, freshAccount(), settings, now).allowed);
    }
    { // agreement: 1 of 4 fails, 1 of 1 passes (rules only, no model keys)
        Signal s = buySignal();
        s.agreeing = 1;
        const RiskDecision d = evaluate(s, freshAccount(), settings, now);
        CHECK(!d.allowed);
        CHECK(!find(d, "agreement")->pass);
        s.voters = 1;
        CHECK(evaluate(s, freshAccount(), settings, now).allowed);
        s.voters = 0;
        s.agreeing = 0;
        CHECK(!evaluate(s, freshAccount(), settings, now).allowed);
    }
    { // stop on the wrong side, too close, too far
        Signal s = buySignal();
        s.stop = 101;
        CHECK(!find(evaluate(s, freshAccount(), settings, now), "stop")->pass);
        s.stop = 99.96; // 0.04 %
        s.takeProfit = 100.2;
        CHECK(!find(evaluate(s, freshAccount(), settings, now), "stop")->pass);
        s.stop = 89; // 11 %
        s.takeProfit = 130;
        CHECK(!find(evaluate(s, freshAccount(), settings, now), "stop")->pass);
        s.stop = 90; // exactly 10 %
        CHECK(find(evaluate(s, freshAccount(), settings, now), "stop")->pass);
    }
    { // reward to risk below, and exactly at, the minimum
        Signal s = buySignal();
        s.takeProfit = 102.9; // 1.45
        const RiskDecision d = evaluate(s, freshAccount(), settings, now);
        CHECK(!d.allowed);
        CHECK(!find(d, "reward")->pass);
        s.takeProfit = 103; // 1.5
        CHECK(evaluate(s, freshAccount(), settings, now).allowed);
    }
    { // positions
        AccountView a = freshAccount();
        a.positionOnSymbol = true;
        CHECK(!find(evaluate(buySignal(), a, settings, now), "positions")->pass);
        a.positionOnSymbol = false;
        a.openPositions = 3;
        CHECK(!evaluate(buySignal(), a, settings, now).allowed);
        a.openPositions = 2;
        CHECK(evaluate(buySignal(), a, settings, now).allowed);
    }
    { // daily loss: 3 % of the day's start
        AccountView a = freshAccount();
        a.equity = 9700;
        a.available = 9700;
        CHECK(dailyLossReached(a, settings));
        CHECK(!find(evaluate(buySignal(), a, settings, now), "daily_loss")->pass);
        a.equity = 9701;
        CHECK(!dailyLossReached(a, settings));
        CHECK(evaluate(buySignal(), a, settings, now).allowed);
    }
    { // drawdown from the peak, and the halt that follows it
        AccountView a = freshAccount();
        a.peakEquity = 12000;
        a.equity = 10800; // exactly 10 %
        a.available = 10800;
        a.dayStartEquity = 10800;
        NEAR(drawdownPct(a.equity, a.peakEquity), 10, 1e-12);
        CHECK(drawdownReached(a, settings));
        CHECK(!find(evaluate(buySignal(), a, settings, now), "drawdown")->pass);
        a.equity = 10801;
        CHECK(!drawdownReached(a, settings));
        CHECK(evaluate(buySignal(), a, settings, now).allowed);
        a.halted = true; // stays refused until the user resumes
        CHECK(!evaluate(buySignal(), a, settings, now).allowed);
        NEAR(drawdownPct(13000, 12000), 0, 0); // above the peak: no drawdown
        NEAR(drawdownPct(100, 0), 0, 0);
    }
    { // cooldown: 30 minutes after a loss on the symbol
        AccountView a = freshAccount();
        a.lastLossOnSymbolMs = now - 29 * 60000LL;
        const RiskDecision d = evaluate(buySignal(), a, settings, now);
        CHECK(!d.allowed);
        CHECK(find(d, "cooldown")->detail == "1 min left");
        a.lastLossOnSymbolMs = now - 30 * 60000LL;
        CHECK(evaluate(buySignal(), a, settings, now).allowed);
    }
    { // too small to be worth an order
        AccountView a = freshAccount();
        a.equity = 5;
        a.available = 5;
        a.dayStartEquity = 5;
        a.peakEquity = 5;
        const RiskDecision d = evaluate(buySignal(), a, settings, now);
        CHECK(!d.allowed);
        CHECK(!find(d, "size")->pass);
        NEAR(d.quantity, 0, 0);
    }
    { // nothing available
        AccountView a = freshAccount();
        a.available = 0;
        CHECK(!evaluate(buySignal(), a, settings, now).allowed);
    }
    { // a short
        Signal s;
        s.action = Action::Sell;
        s.confidence = 80;
        s.entry = 100;
        s.stop = 104;
        s.takeProfit = 92;
        s.voters = 3;
        s.agreeing = 2;
        const RiskDecision d = evaluate(s, freshAccount(), settings, now);
        CHECK(d.allowed);
        NEAR(d.quantity, 25, 1e-9);
        NEAR(d.rewardRisk, 2.0, 1e-12);
    }
    { // the first failed check is the reason, and every check is still listed
        Signal s = buySignal();
        s.confidence = 10;
        s.takeProfit = 100.5;
        const RiskDecision d = evaluate(s, freshAccount(), settings, now);
        CHECK(d.reason.rfind("Confidence", 0) == 0);
        CHECK(d.checks.size() == 10);
    }
}

static void testPaper()
{
    PaperSettings settings;
    settings.startingBalance = 10000;
    settings.feePct = 0.1;
    settings.slippagePct = 0; // exact arithmetic first; slippage has its own block
    const long long t0 = 20000LL * 86400000LL + 3600000LL; // 01:00 UTC on some day
    const long long day = PaperAccount::dayNumber(t0);

    { // a long that reaches its target
        PaperAccount account(settings);
        NEAR(account.equity(), 10000, 0);
        const PaperAccount::OpenResult r = account.open("BTCUSDT", true, 2, 100, 95, 110, t0, 72, "anthropic", "why");
        CHECK(r.ok);
        CHECK(r.position.id == 1);
        NEAR(r.position.entry, 100, 0);
        NEAR(account.balance(), 10000 - 0.2, 1e-9); // fee 0.1 % of 200
        NEAR(account.marginInUse(), 200, 1e-9);
        NEAR(account.available(), 9799.8, 1e-9);
        CHECK(account.positionOn("BTCUSDT") != nullptr);
        CHECK(account.positionOn("ETHUSDT") == nullptr);

        CHECK(account.onPrice("BTCUSDT", 104, 104, 100, t0 + 1000).empty());
        NEAR(account.unrealized(), 8, 1e-9);
        NEAR(account.equity(), 10007.8, 1e-9);
        CHECK(account.onPrice("ETHUSDT", 1, 1, 1, t0 + 1500).empty()); // another market

        const std::vector<Trade> closed = account.onPrice("BTCUSDT", 111, 111, 104, t0 + 2000);
        CHECK(closed.size() == 1);
        const Trade &t = closed[0];
        CHECK(t.reason == "take_profit");
        NEAR(t.exit, 110, 0); // a limit order fills at its price, not at 111
        NEAR(t.fees, 0.2 + 0.22, 1e-9);
        NEAR(t.pnl, 20 - 0.42, 1e-9);
        NEAR(t.pnlPct, (20 - 0.42) / 200 * 100, 1e-9);
        CHECK(t.lead == "anthropic" && t.reasoning == "why");
        NEAR(t.confidence, 72, 0);
        CHECK(account.positions().empty());
        NEAR(account.balance(), 10000 + 20 - 0.42, 1e-9);
        NEAR(account.equity(), account.balance(), 0);
        NEAR(account.realizedOn(day), 19.58, 1e-9);
        NEAR(account.realizedOn(day + 1), 0, 0);
        CHECK(account.lastLossOn("BTCUSDT") == 0);

        const PaperStats s = account.stats();
        CHECK(s.trades == 1 && s.wins == 1 && s.losses == 0);
        NEAR(s.winRatePct, 100, 0);
        NEAR(s.totalPnl, 19.58, 1e-9);
        NEAR(s.profitFactor, 0, 0); // no loss yet
    }
    { // a long stopped out; the stop wins when one update covers both levels
        PaperAccount account(settings);
        CHECK(account.open("BTCUSDT", true, 2, 100, 95, 110, t0).ok);
        const std::vector<Trade> closed = account.onPrice("BTCUSDT", 100, 111, 94, t0 + 1000);
        CHECK(closed.size() == 1);
        CHECK(closed[0].reason == "stop_loss");
        NEAR(closed[0].exit, 95, 0);
        NEAR(closed[0].pnl, -10 - 0.2 - 0.19, 1e-9);
        CHECK(account.lastLossOn("BTCUSDT") == t0 + 1000);
        CHECK(account.lastLossOn("ETHUSDT") == 0);
        const PaperStats s = account.stats();
        CHECK(s.losses == 1 && s.wins == 0);
        NEAR(s.grossLoss, 10.39, 1e-9);
        NEAR(s.averageLoss, 10.39, 1e-9);
        NEAR(s.worstTrade, -10.39, 1e-9);
        NEAR(s.maxDrawdownPct, 10.39 / 10000 * 100, 1e-9);
    }
    { // the price gapped through the stop: the fill is the worse price
        PaperAccount account(settings);
        CHECK(account.open("BTCUSDT", true, 1, 100, 95, 110, t0).ok);
        const std::vector<Trade> closed = account.onPrice("BTCUSDT", 90, 90, 90, t0 + 1000);
        CHECK(closed.size() == 1);
        NEAR(closed[0].exit, 90, 0);
    }
    { // a short: target below, stop above
        PaperAccount account(settings);
        const PaperAccount::OpenResult r = account.open("ETHUSDT", false, 10, 50, 52, 46, t0);
        CHECK(r.ok);
        CHECK(account.onPrice("ETHUSDT", 48, 50, 48, t0 + 1).empty());
        NEAR(account.unrealized(), 20, 1e-9);
        const std::vector<Trade> won = account.onPrice("ETHUSDT", 45, 48, 45, t0 + 2);
        CHECK(won.size() == 1 && won[0].reason == "take_profit");
        NEAR(won[0].exit, 46, 0);
        NEAR(won[0].pnl, 40 - 0.5 - 0.46, 1e-9);

        CHECK(account.open("ETHUSDT", false, 10, 50, 52, 46, t0 + 3).ok);
        const std::vector<Trade> lost = account.onPrice("ETHUSDT", 53, 53, 50, t0 + 4);
        CHECK(lost.size() == 1 && lost[0].reason == "stop_loss");
        NEAR(lost[0].exit, 53, 0); // past the stop: the worse price
        NEAR(lost[0].pnl, -30 - 0.5 - 0.53, 1e-9);
        const PaperStats s = account.stats();
        CHECK(s.trades == 2 && s.wins == 1 && s.losses == 1);
        NEAR(s.winRatePct, 50, 1e-12);
        NEAR(s.profitFactor, 39.04 / 31.03, 1e-9);
        NEAR(s.bestTrade, 39.04, 1e-9);
        NEAR(s.worstTrade, -31.03, 1e-9);
    }
    { // refusals
        PaperAccount account(settings);
        CHECK(!account.open("", true, 1, 100, 95, 110, t0).ok);
        CHECK(!account.open("BTCUSDT", true, 0, 100, 95, 110, t0).ok);
        CHECK(!account.open("BTCUSDT", true, -1, 100, 95, 110, t0).ok);
        CHECK(!account.open("BTCUSDT", true, 1, 0, 95, 110, t0).ok);
        CHECK(!account.open("BTCUSDT", true, 1, 100, 105, 110, t0).ok);  // stop above a long
        CHECK(!account.open("BTCUSDT", false, 1, 100, 95, 90, t0).ok);   // stop below a short
        CHECK(!account.open("BTCUSDT", true, 1, 100, 95, 99, t0).ok);    // target below a long
        CHECK(!account.open("BTCUSDT", true, 1, 100, std::numeric_limits<double>::quiet_NaN(), 110, t0).ok);
        CHECK(!account.open("BTCUSDT", true, 100, 100, 95, 110, t0).ok); // 10 000 + fee > balance
        CHECK(account.open("BTCUSDT", true, 99, 100, 95, 110, t0).ok);
        CHECK(!account.open("BTCUSDT", true, 0.01, 100, 95, 110, t0).ok); // already in this market
        CHECK(!account.open("ETHUSDT", true, 2, 100, 95, 110, t0).ok);   // only ~90 left
        CHECK(account.open("ETHUSDT", true, 0.5, 100, 95, 110, t0).ok);
        CHECK(account.positions().size() == 2);
        NEAR(account.balance(), 10000 - 9.9 - 0.05, 1e-9);
    }
    { // manual close, and closing something that is not there
        PaperAccount account(settings);
        const long long id = account.open("BTCUSDT", true, 1, 100, 95, 110, t0).position.id;
        Trade t;
        CHECK(!account.close(id + 7, 101, t0 + 1, "manual", &t));
        CHECK(!account.close(id, 0, t0 + 1, "manual", &t));
        CHECK(account.close(id, 101, t0 + 1, "manual", &t));
        CHECK(t.reason == "manual");
        NEAR(t.pnl, 1 - 0.1 - 0.101, 1e-9);
        CHECK(!account.close(id, 101, t0 + 2, "manual"));
    }
    { // slippage: every market fill is worse, a limit fill is not
        PaperSettings slip = settings;
        slip.slippagePct = 1.0;
        slip.feePct = 0;
        PaperAccount account(slip);
        const PaperAccount::OpenResult r = account.open("BTCUSDT", true, 1, 100, 95, 110, t0);
        NEAR(r.position.entry, 101, 1e-9);
        CHECK(!account.open("ETHUSDT", true, 1, 100, 95, 100.5, t0).ok); // target below the 101 fill
        const std::vector<Trade> stopped = account.onPrice("BTCUSDT", 95, 95, 95, t0 + 1);
        NEAR(stopped[0].exit, 95 * 0.99, 1e-9);
        const PaperAccount::OpenResult s = account.open("BTCUSDT", false, 1, 100, 105, 90, t0 + 2);
        NEAR(s.position.entry, 99, 1e-9);
        const std::vector<Trade> won = account.onPrice("BTCUSDT", 89, 89, 89, t0 + 3);
        NEAR(won[0].exit, 90, 0);
        Trade t;
        CHECK(account.open("BTCUSDT", false, 1, 100, 105, 90, t0 + 4).ok);
        CHECK(account.close(account.positions()[0].id, 100, t0 + 5, "manual", &t));
        NEAR(t.exit, 101, 1e-9); // buying back costs more
    }
    { // peaks, drawdown and the day roll
        PaperSettings free = settings;
        free.feePct = 0;
        PaperAccount account(free);
        CHECK(account.open("BTCUSDT", true, 10, 100, 50, 200, t0).ok);
        NEAR(account.dayStartEquity(), 10000, 0);
        account.onPrice("BTCUSDT", 120, 120, 100, t0 + 1); // +200
        NEAR(account.peakEquity(), 10200, 1e-9);
        account.onPrice("BTCUSDT", 90, 120, 90, t0 + 2);   // -100
        NEAR(account.peakEquity(), 10200, 1e-9);
        NEAR(account.equity(), 9900, 1e-9);
        NEAR(account.stats().maxDrawdownPct, 300.0 / 10200 * 100, 1e-9);
        account.onPrice("BTCUSDT", 110, 110, 90, t0 + 3);  // recovers, the record stays
        NEAR(account.stats().maxDrawdownPct, 300.0 / 10200 * 100, 1e-9);
        account.mark(t0 + 86400000LL);                      // next UTC day
        NEAR(account.dayStartEquity(), 10100, 1e-9);
        account.mark(t0 + 86400000LL + 5);                  // same day: unchanged
        account.onPrice("BTCUSDT", 130, 130, 110, t0 + 86400000LL + 6);
        NEAR(account.dayStartEquity(), 10100, 1e-9);
    }
    { // state survives a round trip; garbage is refused
        PaperAccount account(settings);
        CHECK(account.open("BTCUSDT", true, 1, 100, 95, 110, t0).ok);
        CHECK(account.open("ETHUSDT", false, 2, 50, 55, 40, t0).ok);
        account.onPrice("BTCUSDT", 111, 111, 100, t0 + 10);
        const PaperAccount::State saved = account.state();

        PaperAccount copy;
        CHECK(copy.restore(saved));
        NEAR(copy.balance(), account.balance(), 0);
        NEAR(copy.equity(), account.equity(), 0);
        CHECK(copy.positions().size() == 1 && copy.trades().size() == 1);
        CHECK(copy.positions()[0].symbol == "ETHUSDT");
        // ids continue after the highest one in use
        CHECK(copy.open("BTCUSDT", true, 1, 100, 95, 110, t0 + 20).position.id == 3);

        PaperAccount::State bad = saved;
        bad.balance = -5;
        PaperAccount untouched(settings);
        CHECK(!untouched.restore(bad));
        NEAR(untouched.balance(), 10000, 0);
        bad = saved;
        bad.balance = std::numeric_limits<double>::quiet_NaN();
        CHECK(!untouched.restore(bad));
        bad = saved;
        bad.positions[0].quantity = 0;
        CHECK(!untouched.restore(bad));
        bad = saved;
        bad.positions[0].stop = 0;
        CHECK(!untouched.restore(bad));
        bad = saved;
        bad.nextId = 1; // a file that would reuse ids
        CHECK(untouched.restore(bad));
        CHECK(untouched.open("BTCUSDT", true, 1, 100, 95, 110, t0 + 30).position.id == 3);
    }
    { // reset and costs
        PaperAccount account(settings);
        CHECK(account.open("BTCUSDT", true, 1, 100, 95, 110, t0).ok);
        PaperSettings bigger = settings;
        bigger.startingBalance = 50000;
        account.reset(bigger);
        NEAR(account.balance(), 50000, 0);
        CHECK(account.positions().empty() && account.trades().empty());
        account.setCosts(0.5, 0);
        CHECK(account.open("BTCUSDT", true, 1, 100, 95, 110, t0).ok);
        NEAR(account.balance(), 50000 - 0.5, 1e-9);
        account.setCosts(-1, std::numeric_limits<double>::quiet_NaN()); // ignored
        NEAR(account.settings().feePct, 0.5, 0);
        PaperSettings broken;
        broken.startingBalance = -3;
        account.reset(broken);
        NEAR(account.balance(), 10000, 0); // falls back to the default
    }
}

// A market whose base is the account's currency: USD/CHF on a dollar account.
// The quantity is in dollars; a profit in francs comes back at the price it
// is taken at.
static void testInverseMarkets()
{
    NEAR(notionalOf(false, 0.8, 1000), 800, 1e-12);
    NEAR(notionalOf(true, 0.8, 1000), 1000, 1e-12);
    NEAR(moveValue(false, 100, 110, 2), 20, 1e-12);
    // 1000 dollars of USD/CHF from 0.80 to 0.88: 80 francs, which are 90.909 dollars at 0.88.
    NEAR(moveValue(true, 0.80, 0.88, 1000), 80.0 / 0.88, 1e-9);
    NEAR(moveValue(true, 0.80, 0.72, 1000), -80.0 / 0.72, 1e-9);

    PaperSettings settings;
    settings.startingBalance = 10000;
    settings.feePct = 0.1;
    settings.slippagePct = 0;
    const long long t0 = 20000LL * 86400000LL + 3600000LL;

    { // a long that reaches its target
        PaperAccount account(settings);
        const PaperAccount::OpenResult r = account.open("USDCHF", true, 2000, 0.80, 0.76, 0.88, t0, 70, "a", "b", true);
        CHECK(r.ok);
        CHECK(r.position.inverse);
        NEAR(account.balance(), 10000 - 2.0, 1e-9);   // fee 0.1 % of 2000 dollars, not of 1600
        NEAR(account.marginInUse(), 2000, 1e-9);
        CHECK(account.onPrice("USDCHF", 0.84, 0.84, 0.80, t0 + 1000).empty());
        NEAR(account.unrealized(), 0.04 * 2000 / 0.84, 1e-9);
        const std::vector<Trade> closed = account.onPrice("USDCHF", 0.89, 0.89, 0.84, t0 + 2000);
        CHECK(closed.size() == 1);
        if (closed.size() == 1) {
            const Trade &t = closed[0];
            CHECK(t.inverse);
            CHECK(t.reason == "take_profit");
            NEAR(t.exit, 0.88, 1e-12);
            const double gross = 0.08 * 2000 / 0.88;
            NEAR(t.fees, 2.0 + 2.0, 1e-9);            // 0.1 % of 2000 dollars on each side
            NEAR(t.pnl, gross - 4.0, 1e-9);
            NEAR(t.pnlPct, (gross - 4.0) / 2000 * 100, 1e-9);
            NEAR(account.balance(), 10000 + gross - 4.0, 1e-9);
        }
        CHECK(account.positions().empty());
    }
    { // a short that is stopped out
        PaperAccount account(settings);
        CHECK(account.open("USDCAD", false, 1000, 1.40, 1.44, 1.30, t0, 0, {}, {}, true).ok);
        const std::vector<Trade> closed = account.onPrice("USDCAD", 1.44, 1.44, 1.40, t0 + 1000);
        CHECK(closed.size() == 1);
        if (closed.size() == 1) {
            CHECK(closed[0].reason == "stop_loss");
            NEAR(closed[0].exit, 1.44, 1e-12);
            NEAR(closed[0].pnl, -(0.04 * 1000 / 1.44) - 2.0, 1e-9);
        }
    }
    { // what the balance covers is counted in dollars
        PaperAccount account(settings);
        CHECK(!account.open("USDCHF", true, 10001, 0.80, 0.76, 0.88, t0, 0, {}, {}, true).ok);
        CHECK(account.open("USDCHF", true, 9000, 0.80, 0.76, 0.88, t0, 0, {}, {}, true).ok);
    }

    // The size: one per cent of the account is lost when the stop is hit.
    Signal signal;
    signal.action = Action::Buy;
    signal.confidence = 80;
    signal.entry = 0.80;
    signal.stop = 0.796;          // half a per cent away
    signal.takeProfit = 0.82;
    signal.voters = 3;
    signal.agreeing = 3;
    AccountView view;
    view.equity = view.available = view.dayStartEquity = view.peakEquity = 10000;
    view.inverse = true;
    RiskSettings risk;
    risk.maxOpenPositions = 1;    // the whole account may go into this one
    const RiskDecision sized = evaluate(signal, view, risk, t0);
    CHECK(sized.allowed);
    // Lost at the stop: 0.004 francs a dollar, in dollars at 0.796.
    NEAR(sized.riskAmount, sized.quantity * 0.004 / 0.796, 1e-9);
    NEAR(sized.notional, sized.quantity, 1e-9);                     // dollars are dollars
    CHECK(sized.notional <= 10000 * 0.995 + 1e-9);
    if (!sized.reducedByBalance)
        NEAR(sized.riskAmount, 100, 1e-6);
    // With a stop five per cent away nothing caps the size: exactly 100 dollars at risk.
    signal.stop = 0.76;
    signal.takeProfit = 0.90;
    const RiskDecision wide = evaluate(signal, view, risk, t0);
    CHECK(wide.allowed);
    CHECK(!wide.reducedByBalance);
    NEAR(wide.riskAmount, 100, 1e-9);
    NEAR(wide.quantity, 100 * 0.76 / 0.04, 1e-6);
    // The same signal on a linear market sizes differently, as before.
    view.inverse = false;
    const RiskDecision linear = evaluate(signal, view, risk, t0);
    NEAR(linear.quantity, 100 / 0.04, 1e-6);
    NEAR(linear.notional, linear.quantity * 0.80, 1e-6);
}

int main()
{
    testNamesAndLevels();
    testConsensus();
    testRules();
    testRisk();
    testPaper();
    testInverseMarkets();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
