// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests of the desk's reading of messages and of the analysis state it keeps.
// No framework; from applications/trading:
//
//   g++ -std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Wshadow -Werror -fsanitize=address,undefined
//       core/desk.cpp tests/test_desk.cpp -o /tmp/test_desk
//   /tmp/test_desk
//
// or: tests/run-unit.sh desk
#include "../core/desk.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace Trading::Desk;

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

static bool hasMarket(const Reading &r, const std::string &base, const std::string &quote = "*")
{
    for (const Mention &m : r.markets) {
        if (m.base == base && (quote == "*" || m.quote == quote))
            return true;
    }
    return false;
}

static const LevelNote *level(const Reading &r, double price)
{
    for (const LevelNote &l : r.levels) {
        if (std::fabs(l.price - price) < 1e-9)
            return &l;
    }
    return nullptr;
}

static const Observation *observation(const Reading &r, const std::string &text)
{
    for (const Observation &o : r.observations) {
        if (o.text == text)
            return &o;
    }
    return nullptr;
}

static bool hasFact(const Reading &r, const std::string &fact)
{
    for (const std::string &f : r.facts) {
        if (f == fact)
            return true;
    }
    return false;
}

static void testPrices()
{
    double v = 0;
    CHECK(parsePrice("95,000", &v));
    NEAR(v, 95000, 0);
    CHECK(parsePrice("95k", &v));
    NEAR(v, 95000, 0);
    CHECK(parsePrice("94.5K", &v));
    NEAR(v, 94500, 1e-9);
    CHECK(parsePrice("$1,234,567.5", &v));
    NEAR(v, 1234567.5, 1e-9);
    CHECK(parsePrice("1.0850", &v));
    NEAR(v, 1.085, 1e-12);
    CHECK(parsePrice("@3200", &v));
    NEAR(v, 3200, 0);
    CHECK(!parsePrice("95,0", &v));       // not a thousands separator
    CHECK(!parsePrice("15m", &v));        // a timeframe
    CHECK(!parsePrice("4h", &v));
    CHECK(!parsePrice("2x", &v));
    CHECK(!parsePrice("abc", &v));
    CHECK(!parsePrice("1.", &v));
    CHECK(!parsePrice("0", &v));
    CHECK(!parsePrice("1,2345", &v));

    CHECK(formatPrice(95000) == "95,000");
    CHECK(formatPrice(94812.4) == "94,812.4");
    CHECK(formatPrice(1234567.891) == "1,234,567.89");
    CHECK(formatPrice(1.0982) == "1.0982");
    CHECK(formatPrice(0.231456) == "0.231456");
    CHECK(formatPrice(3200) == "3,200");
}

static void testMarkets()
{
    CHECK(hasMarket(read("BTC is approaching 95,000."), "BTC"));
    CHECK(hasMarket(read("bitcoin looks heavy"), "BTC"));
    CHECK(hasMarket(read("what about ethusdt"), "ETH", "USDT"));
    CHECK(hasMarket(read("XAUUSD at the highs"), "XAU", "USD"));
    CHECK(hasMarket(read("gold is flying"), "XAU", "USD"));
    CHECK(hasMarket(read("eur/usd broke down"), "EUR", "USD"));
    CHECK(hasMarket(read("cable is weak"), "GBP", "USD"));
    CHECK(hasMarket(read("PAXG holds"), "XAU", "USD"));
    CHECK(hasMarket(read("NEAR pumped"), "NEAR"));
    // An English word is not a market unless written like one.
    CHECK(!hasMarket(read("price is near 95k"), "NEAR"));
    CHECK(!hasMarket(read("link me the chart"), "LINK"));
    CHECK(read("what do you think about entering short?").markets.empty());
    // Two markets, each once.
    const Reading two = read("BTC and ETH, btc leads");
    CHECK(two.markets.size() == 2);
}

static void testLevels()
{
    Reading r = read("BTC is approaching 95,000.");
    CHECK(r.levels.size() == 1);
    CHECK(level(r, 95000) && level(r, 95000)->relation == "approaching");
    CHECK(level(r, 95000) && level(r, 95000)->kind == "level");

    r = read("support at 94k, resistance 96,500");
    CHECK(level(r, 94000) && level(r, 94000)->kind == "support");
    CHECK(level(r, 96500) && level(r, 96500)->kind == "resistance");

    r = read("short from 95100 with a stop at 95,800 and take profit at 93k");
    CHECK(level(r, 95100) != nullptr);
    CHECK(level(r, 95800) && level(r, 95800)->kind == "stop");
    CHECK(level(r, 93000) && level(r, 93000)->kind == "target");
    CHECK(r.tradeSide == "short");

    r = read("range 94,500-95,000 for now");
    CHECK(level(r, 94500) && level(r, 95000));

    // Numbers that are not prices.
    CHECK(read("RSI is at 72").levels.empty());
    CHECK(read("RSI 72 on the 4h").levels.empty());
    CHECK(read("the 200 ema is flat").levels.empty());
    CHECK(read("up 5 % today").levels.empty());
    CHECK(read("up 5% today").levels.empty());
    CHECK(read("I have 3 trades open").levels.empty());
    CHECK(read("in 2025 it ran").levels.empty());
    CHECK(read("15m chart looks fine").levels.empty());
    CHECK(read("using 10x leverage").levels.empty());
    CHECK(read("it is 5").levels.empty());
    // A small price with words that make it one.
    CHECK(level(read("EURUSD rejected at 1.1050"), 1.105) != nullptr);
}

static void testObservations()
{
    Reading r = read("RSI is also showing bearish divergence.");
    CHECK(observation(r, "RSI bearish divergence") && observation(r, "RSI bearish divergence")->side == Side::Bearish);
    CHECK(observation(r, "RSI bearish divergence")->topic == "rsi");
    CHECK(r.lean == Side::Bearish);
    CHECK(!r.question);

    r = read("macd bullish cross on the daily, higher lows too");
    CHECK(observation(r, "MACD bullish cross") != nullptr);
    CHECK(observation(r, "Higher highs / higher lows") != nullptr);
    CHECK(r.lean == Side::Bullish);

    r = read("rsi 78, overbought");
    CHECK(observation(r, "RSI overbought (78)") != nullptr);
    r = read("RSI at 25");
    CHECK(observation(r, "RSI oversold (25)") != nullptr);
    r = read("RSI at 55");
    CHECK(observation(r, "RSI at 55") && observation(r, "RSI at 55")->side == Side::Neutral);

    CHECK(observation(read("double top forming"), "Double top") != nullptr);
    CHECK(observation(read("inverse head and shoulders"), "Inverse head and shoulders") != nullptr);
    CHECK(!observation(read("inverse head and shoulders"), "Head and shoulders"));
    CHECK(observation(read("volume is drying up"), "Volume falling") != nullptr);
    CHECK(observation(read("CPI tomorrow"), "CPI release") != nullptr);
    CHECK(observation(read("death cross on the daily"), "Death cross") != nullptr);
    CHECK(observation(read("it is consolidating"), "Ranging") != nullptr);
    // "bullish divergence" counts once, not also as a standalone "bullish".
    r = read("bullish divergence but I am bearish");
    CHECK(r.lean == Side::None);
}

static void testIntent()
{
    Reading r = read("What do you think about entering short?");
    CHECK(r.question);
    CHECK(r.tradeDecision);
    CHECK(r.tradeSide == "short");
    CHECK(r.facts.empty());

    r = read("BTC is approaching 95,000.");
    CHECK(!r.question);
    CHECK(!r.tradeDecision);

    r = read("what is the price of BTC?");
    CHECK(r.question && !r.tradeDecision);
    CHECK(hasFact(r, "price"));

    r = read("where is rsi now");
    CHECK(r.question && hasFact(r, "rsi"));

    r = read("why is rsi so high?");
    CHECK(r.question && r.facts.empty()); // wants an explanation, not a number

    r = read("should I go long here?");
    CHECK(r.tradeDecision && r.tradeSide == "long");

    // "how long" is not a long trade.
    r = read("how long will this range last?");
    CHECK(r.tradeSide.empty());

    // Roman Urdu.
    r = read("btc short lena chahiye");
    CHECK(r.question && r.tradeDecision && r.tradeSide == "short");

    r = read("analyze eth for me");
    CHECK(r.question);
}

static void testState()
{
    DeskState s;
    const long long t = 1'700'000'000'000LL;
    // Said before a market was named: it belongs to the first one named.
    std::vector<Change> c = s.apply(read("RSI is showing bearish divergence"), 1, t);
    CHECK(c.size() == 1);
    CHECK(s.current().empty());
    c = s.focus("BTCUSDT", "BTC/USDT", t + 1);
    CHECK(c.size() == 1 && c[0].field == "market");
    CHECK(s.find("") == nullptr);
    CHECK(s.find("BTCUSDT") && s.find("BTCUSDT")->entries.size() == 1);

    c = s.apply(read("BTC is approaching 95,000."), 2, t + 2);
    CHECK(c.size() == 1 && c[0].field == "level");
    // The same again changes nothing.
    c = s.apply(read("btc approaching 95k"), 3, t + 3);
    CHECK(c.empty());
    CHECK(s.find("BTCUSDT")->levels.size() == 1);
    // Named as resistance now: the level gets its kind.
    c = s.apply(read("95,000 is resistance"), 4, t + 4);
    CHECK(c.size() == 1 && s.find("BTCUSDT")->levels[0].kind == "resistance");

    c = s.apply(read("What do you think about entering short?"), 5, t + 5);
    CHECK(s.find("BTCUSDT")->plan.side == "short");
    CHECK(c.size() == 1 && c[0].field == "plan");

    const Lean lean = leanOf(*s.find("BTCUSDT"));
    CHECK(lean.side == Side::Bearish && lean.bearish == 1 && lean.bullish == 0);

    // Another market keeps its own book; coming back finds the old one.
    s.focus("ETHUSDT", "ETH/USDT", t + 6);
    s.apply(read("support at 3,100"), 6, t + 6);
    CHECK(s.find("ETHUSDT")->levels.size() == 1);
    s.focus("BTCUSDT", "BTC/USDT", t + 7);
    CHECK(s.currentBook().levels.size() == 1 && s.currentBook().levels[0].price == 95000);

    // A model's bias and conclusion, and the person's own word staying theirs.
    std::vector<Change> changes;
    Bias b;
    b.side = Side::Bearish;
    b.source = Source::Model;
    b.by = "Kimi K3.1 Pro";
    b.time = t + 8;
    s.setBias("BTCUSDT", b, &changes);
    CHECK(changes.size() == 1 && changes[0].field == "bias");
    s.setBias("BTCUSDT", b, &changes); // same side: no new change
    CHECK(changes.size() == 1);
    Entry e;
    e.topic = "rsi";
    e.text = "RSI bearish divergence";
    e.side = Side::Bearish;
    e.source = Source::Model;
    e.time = t + 9;
    CHECK(!s.addEntry("BTCUSDT", e, &changes));
    CHECK(s.find("BTCUSDT")->entries[0].source == Source::You);
}

static void testPlan()
{
    Plan p;
    p.side = "short";
    p.entry = 95000;
    p.stop = 95800;
    p.target = 93000;
    NEAR(planRewardRisk(p), 2.5, 1e-12);
    CHECK(planProblem(p).empty());
    p.stop = 94000; // on the winning side
    NEAR(planRewardRisk(p), 0, 0);
    CHECK(planProblem(p) == "the stop of a short must be above the entry");
    p.side = "long";
    p.entry = 100;
    p.stop = 95;
    p.target = 110;
    NEAR(planRewardRisk(p), 2.0, 1e-12);
    p.target = 99;
    CHECK(planProblem(p) == "the target of a long must be above the entry");
    Plan empty;
    CHECK(planProblem(empty) == "no direction yet");

    // Merging: numbers given later join the side given earlier.
    DeskState s;
    s.focus("BTCUSDT", "BTC/USDT", 1);
    s.apply(read("thinking about a short"), 1, 1);
    s.apply(read("entry 95,000, stop 95,800, target 93,000"), 2, 2);
    const Plan &merged = s.currentBook().plan;
    CHECK(merged.side == "short");
    NEAR(merged.entry, 95000, 0);
    NEAR(merged.stop, 95800, 0);
    NEAR(merged.target, 93000, 0);
    NEAR(planRewardRisk(merged), 2.5, 1e-12);
}

static void testPriceUpdates()
{
    DeskState s;
    s.focus("BTCUSDT", "BTC/USDT", 1);
    s.apply(read("resistance at 95,000"), 1, 1);
    // Far below: no event, the distance is known.
    std::vector<Change> c = s.priceUpdate("BTCUSDT", 92000, 0.25, 2);
    CHECK(c.empty());
    CHECK(s.currentBook().levels[0].state == LevelState::Below);
    NEAR(s.currentBook().levels[0].distancePct, (92000.0 - 95000.0) / 95000.0 * 100, 1e-9);
    // Close: one event, not repeated while it stays close.
    c = s.priceUpdate("BTCUSDT", 94850, 0.25, 3);
    CHECK(c.size() == 1 && c[0].field == "price" && c[0].source == Source::Market);
    CHECK(c[0].text.find("below resistance 95,000") != std::string::npos);
    c = s.priceUpdate("BTCUSDT", 94900, 0.25, 4);
    CHECK(c.empty());
    // Just over it is not yet a crossing: inside the dead band (half of "near").
    c = s.priceUpdate("BTCUSDT", 95020, 0.25, 5);
    CHECK(c.empty());
    CHECK(s.currentBook().levels[0].state == LevelState::Below);
    // Clearly past it: one event.
    c = s.priceUpdate("BTCUSDT", 95300, 0.25, 6);
    CHECK(c.size() == 1 && c[0].text.find("crossed above") != std::string::npos);
    CHECK(c[0].text.find("live 95,300") != std::string::npos);
    // A price hovering on the level: it is close (said once), and no crossing.
    size_t hover = 0;
    for (int i = 0; i < 20; ++i) {
        const std::vector<Change> h = s.priceUpdate("BTCUSDT", i % 2 ? 94999 : 95001, 0.25, 7 + i);
        hover += h.size();
        for (const Change &x : h)
            CHECK(x.text.find("crossed") == std::string::npos);
    }
    CHECK(hover == 1);
    // Back below and above again within a minute: crossings are not repeated.
    CHECK(s.priceUpdate("BTCUSDT", 94700, 0.25, 30).empty());
    CHECK(s.currentBook().levels[0].state == LevelState::Below);
    CHECK(s.priceUpdate("BTCUSDT", 95300, 0.25, 40).empty());
    // A minute later a crossing is news again.
    c = s.priceUpdate("BTCUSDT", 94700, 0.25, 60100);
    CHECK(c.size() == 1 && c[0].text.find("crossed below") != std::string::npos);
    c = s.priceUpdate("BTCUSDT", 95300, 0.25, 130000);
    CHECK(c.size() == 1 && c[0].text.find("crossed above") != std::string::npos);
    // Away and back: "near" again.
    s.priceUpdate("BTCUSDT", 96000, 0.25, 130001);
    c = s.priceUpdate("BTCUSDT", 95100, 0.25, 130002);
    CHECK(c.size() == 1 && c[0].text.find("above resistance") != std::string::npos);
    // A market without levels or a bad price: nothing.
    CHECK(s.priceUpdate("ETHUSDT", 3000, 0.25, 8).empty());
    CHECK(s.priceUpdate("BTCUSDT", std::nan(""), 0.25, 9).empty());
    CHECK(s.priceUpdate("BTCUSDT", 0, 0.25, 9).empty());
}

int main()
{
    testPrices();
    testMarkets();
    testLevels();
    testObservations();
    testIntent();
    testState();
    testPlan();
    testPriceUpdates();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
