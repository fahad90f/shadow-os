// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests of the SBT Model 1 engine (core/sbt.cpp): bullish and bearish
// structures, the BOS, the Level 2 zone, retests, invalidations, PD arrays,
// entries at the next open and Level 3 targets. No framework; from
// applications/trading: tests/run-unit.sh sbt
//
// The bullish chart below is drawn by hand; the bearish one is the same chart
// turned upside down (mirror()), so both directions are held to the same
// numbers.
#include "../core/sbt.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace Trading;
using namespace Trading::Sbt;

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

static Candle bar(int i, double o, double h, double l, double c)
{
    Candle x;
    x.openTime = 1'700'000'000'000LL + i * 60000LL;
    x.open = o;
    x.high = h;
    x.low = l;
    x.close = c;
    x.volume = 100;
    return x;
}

// The hand-drawn bullish SBT: warm-up whose last three small candles are a
// base, a displacement at 20 (so an order block at 19 and a demand zone at
// 17-19, 99.6..100.6, under Level 2), Level 1 at candle 22 (103.5), the
// lowest bearish candle at 25 (zone 100.0..101.4, Level 2 100.0), the BOS at
// 28 (close 104.4 above 103.5), Level 3 at 30 (106.5), the retest at 34 (low
// 101.2 into the zone, close 101.6 above it), entry at the open of 35, the
// target reached at 36.
static std::vector<Candle> bullish()
{
    std::vector<Candle> v;
    for (int i = 0; i < 20; ++i)
        v.push_back(i % 2 == 0 ? bar(i, 100.0, 100.6, 99.6, 100.3) : bar(i, 100.3, 100.6, 99.6, 100.0));
    v.push_back(bar(20, 100.0, 101.6, 99.9, 101.5)); // a displacement out of the base
    v.push_back(bar(21, 101.5, 102.4, 101.4, 102.2));
    v.push_back(bar(22, 102.2, 103.5, 102.0, 103.2)); // Level 1
    v.push_back(bar(23, 103.2, 103.3, 102.0, 102.2));
    v.push_back(bar(24, 102.2, 102.4, 101.0, 101.2));
    v.push_back(bar(25, 101.2, 101.4, 100.0, 100.3)); // the lowest bearish candle: Level 2 and the zone
    v.push_back(bar(26, 100.3, 101.6, 100.2, 101.5));
    v.push_back(bar(27, 101.5, 102.8, 101.4, 102.7));
    v.push_back(bar(28, 102.7, 104.6, 102.6, 104.4)); // BOS
    v.push_back(bar(29, 104.4, 106.0, 104.2, 105.8));
    v.push_back(bar(30, 105.8, 106.5, 105.0, 105.2)); // Level 3
    v.push_back(bar(31, 105.2, 105.6, 104.0, 104.2));
    v.push_back(bar(32, 104.2, 104.4, 103.0, 103.2));
    v.push_back(bar(33, 103.2, 103.4, 102.0, 102.2));
    v.push_back(bar(34, 102.2, 102.3, 101.2, 101.6)); // the retest
    v.push_back(bar(35, 101.6, 103.0, 101.5, 102.9)); // entry at its open
    v.push_back(bar(36, 102.9, 106.6, 102.8, 106.2)); // Level 3 reached
    return v;
}

// The same chart upside down: every price p becomes 200 - p.
static std::vector<Candle> mirror(const std::vector<Candle> &v)
{
    std::vector<Candle> out;
    for (const Candle &c : v) {
        Candle m = c;
        m.open = 200 - c.open;
        m.close = 200 - c.close;
        m.high = 200 - c.low;
        m.low = 200 - c.high;
        out.push_back(m);
    }
    return out;
}

struct Run
{
    std::vector<Event> events;
    Engine engine;
    explicit Run(const Settings &s = Settings()) : engine(s) { engine.setLive(true); }
    void closeAll(const std::vector<Candle> &v, size_t upTo)
    {
        for (size_t i = 0; i < upTo && i < v.size(); ++i) {
            const std::vector<Event> e = engine.close(v[i]);
            events.insert(events.end(), e.begin(), e.end());
        }
    }
    void form(const Candle &c)
    {
        const std::vector<Event> e = engine.forming(c);
        events.insert(events.end(), e.begin(), e.end());
    }
    const Event *find(const std::string &kind) const
    {
        for (const Event &e : events) {
            if (e.kind == kind)
                return &e;
        }
        return nullptr;
    }
    int count(const std::string &kind) const
    {
        int n = 0;
        for (const Event &e : events)
            n += e.kind == kind;
        return n;
    }
};

static bool hasKind(const Setup &s, PdKind kind)
{
    for (const PdArray &a : s.confirmations) {
        if (a.kind == kind)
            return true;
    }
    return false;
}

static void testBullish()
{
    const std::vector<Candle> v = bullish();
    Run r;
    r.closeAll(v, 35); // up to the retest
    const Event *armed = r.find("armed");
    CHECK(armed != nullptr);
    if (armed) {
        const Setup &s = armed->setup;
        CHECK(s.direction == Direction::Bullish);
        NEAR(s.level1.price, 103.5, 1e-9);
        CHECK(s.level1.time == v[22].openTime);
        NEAR(s.level2.price, 100.0, 1e-9);
        CHECK(s.level2.time == v[25].openTime);
        NEAR(s.bos.price, 104.4, 1e-9);
        CHECK(s.bos.time == v[28].openTime);
        NEAR(s.zoneBottom, 100.0, 1e-9);
        NEAR(s.zoneTop, 101.4, 1e-9);
        CHECK(s.zoneTime == v[25].openTime);
        CHECK(s.id == "bullish:" + std::to_string(v[22].openTime));
    }
    const Event *trig = r.find("triggered");
    CHECK(trig != nullptr);
    if (trig) {
        const Setup &s = trig->setup;
        CHECK(s.retest.time == v[34].openTime);
        NEAR(s.level3.price, 106.5, 1e-9);
        CHECK(s.level3.time == v[30].openTime);
        CHECK(s.level3Confirmed);
        NEAR(s.target, 106.5, 1e-9);
        CHECK(s.stop < 100.0 && s.stop > 99.0); // below the zone by a quarter ATR
        CHECK(!s.confirmations.empty());
        CHECK(hasKind(s, PdKind::OrderBlock)); // the last bearish candle before the displacement at 20
        CHECK(hasKind(s, PdKind::Zone));       // the demand base under it
        for (const PdArray &a : s.confirmations) {
            CHECK(a.time != s.zoneTime);       // the zone's own candle does not confirm itself
            CHECK(a.bullish);
            CHECK(a.top <= s.zoneTop + 0.2);   // at or below Level 2
        }
        CHECK(r.find("entered") == nullptr);   // not before the next candle opens
    }
    // The next candle opens: the entry is its open.
    Candle forming = v[35];
    forming.closed = false;
    forming.high = forming.low = forming.close = forming.open;
    r.form(forming);
    const Event *in = r.find("entered");
    CHECK(in != nullptr);
    if (in) {
        NEAR(in->setup.entry, 101.6, 1e-9);
        CHECK(in->setup.entryTime == v[35].openTime);
        CHECK(in->setup.rewardRisk > 1.5);
        NEAR(in->setup.rewardRisk, (106.5 - 101.6) / (101.6 - in->setup.stop), 1e-9);
        CHECK(!in->setup.historical);
        CHECK(in->live);
    }
    // The forming candle again: no second entry.
    r.form(forming);
    CHECK(r.count("entered") == 1);
    r.closeAll(v, 37); // closes 35 and 36 (earlier ones are ignored as old)
    CHECK(r.count("entered") == 1);
    const Event *hit = r.find("target");
    CHECK(hit != nullptr);
    if (hit)
        CHECK(hit->setup.outcomeTime == v[36].openTime);
    CHECK(r.find("stopped") == nullptr);
    CHECK(r.engine.setups().size() == 1);
    CHECK(r.engine.setups().back().status == Status::Target);
}

static void testBearish()
{
    const std::vector<Candle> v = mirror(bullish());
    Run r;
    r.closeAll(v, 35);
    Candle forming = v[35];
    forming.closed = false;
    forming.high = forming.low = forming.close = forming.open;
    r.form(forming);
    r.closeAll(v, 37);
    const Event *armed = r.find("armed");
    CHECK(armed != nullptr);
    if (armed) {
        const Setup &s = armed->setup;
        CHECK(s.direction == Direction::Bearish);
        NEAR(s.level1.price, 200 - 103.5, 1e-9);
        NEAR(s.level2.price, 200 - 100.0, 1e-9);
        NEAR(s.zoneTop, 100.0, 1e-9);           // the highest bullish candle: 98.6..100.0
        NEAR(s.zoneBottom, 200 - 101.4, 1e-9);
        CHECK(s.id == "bearish:" + std::to_string(v[22].openTime));
    }
    const Event *in = r.find("entered");
    CHECK(in != nullptr);
    if (in) {
        NEAR(in->setup.entry, 200 - 101.6, 1e-9);
        NEAR(in->setup.target, 200 - 106.5, 1e-9);
        CHECK(in->setup.stop > 100.0 && in->setup.stop < 101.0);
        CHECK(hasKind(in->setup, PdKind::OrderBlock));
    }
    CHECK(r.find("target") != nullptr);
    CHECK(r.count("entered") == 1);
    // The bullish engine sees no bullish setup in the bearish chart, and the other way round.
    for (const Setup &s : r.engine.setups())
        CHECK(s.direction == Direction::Bearish);
}

static void testCloseThroughTheZoneInvalidates()
{
    for (bool bear : {false, true}) {
        std::vector<Candle> v = bullish();
        v[34] = bar(34, 102.2, 102.3, 99.5, 99.8); // closes below the zone (100.0)
        if (bear)
            v = mirror(v);
        Run r;
        r.closeAll(v, 36);
        CHECK(r.find("invalidated") != nullptr);
        CHECK(r.find("triggered") == nullptr);
        CHECK(r.find("entered") == nullptr);
        const Event *e = r.find("invalidated");
        if (e)
            CHECK(e->setup.note == (bear ? "a candle closed above the Level 2 zone" : "a candle closed below the Level 2 zone"));
    }
}

static void testWickBelowButCloseInsideIsARetest()
{
    std::vector<Candle> v = bullish();
    v[34] = bar(34, 102.2, 102.3, 99.9, 100.5); // wick under the zone, close inside it: rule 5 holds
    Run r;
    r.closeAll(v, 35);
    CHECK(r.find("triggered") != nullptr);
    CHECK(r.find("invalidated") == nullptr);
}

static void testAnEarlyRetestCounts()
{
    // Straight back into the zone one candle after the BOS, without a new
    // high: the BOS candle's high is Level 3, and this is the retest.
    std::vector<Candle> v = bullish();
    v.resize(29);
    v.push_back(bar(29, 104.4, 104.5, 101.3, 101.8));
    Run r;
    r.closeAll(v, 30);
    const Event *t = r.find("triggered");
    CHECK(t != nullptr);
    if (t) {
        CHECK(t->setup.level3Confirmed);
        NEAR(t->setup.level3.price, 104.6, 1e-9);
        CHECK(t->setup.level3.time == v[28].openTime);
    }
    // A candle that makes a new high AND reaches the zone is no retest: Level 3 moves.
    std::vector<Candle> w = bullish();
    w.resize(29);
    w.push_back(bar(29, 104.4, 105.5, 101.3, 101.8));
    Run q;
    q.closeAll(w, 30);
    CHECK(q.find("triggered") == nullptr);
    CHECK(q.engine.setups().back().status == Status::Armed);
    NEAR(q.engine.setups().back().level3.price, 105.5, 1e-9);
}

static void testStopIsBeyondLevel2()
{
    // Level 2 made by a bullish candle's wick under the zone candle: the stop goes below the wick.
    std::vector<Candle> v = bullish();
    v[26] = bar(26, 100.3, 101.6, 99.7, 101.5);
    Run r;
    r.closeAll(v, 35);
    const Event *t = r.find("triggered");
    CHECK(t != nullptr);
    if (t) {
        NEAR(t->setup.level2.price, 99.7, 1e-9);
        NEAR(t->setup.zoneBottom, 100.0, 1e-9);
        CHECK(t->setup.stop < 99.7);
    }
}

static void testIdsCarryThePrefix()
{
    Settings s;
    Engine e(s);
    e.setIdPrefix("BTCUSDT:15m:");
    e.setLive(true);
    for (const Candle &c : bullish())
        e.close(c);
    CHECK(!e.setups().empty());
    if (!e.setups().empty())
        CHECK(e.setups().front().id.rfind("BTCUSDT:15m:bullish:", 0) == 0);
}

static void testOutcomeNeverFromBeforeTheEntry()
{
    // Entered at the forming candle 35; then a late close of an OLDER candle
    // that went below the stop must not stop it.
    const std::vector<Candle> v = bullish();
    Run r;
    r.closeAll(v, 35);
    Candle forming = v[35];
    forming.closed = false;
    forming.high = forming.low = forming.close = forming.open;
    r.form(forming);
    CHECK(r.find("entered") != nullptr);
    Candle late = v[34];
    late.openTime = v[34].openTime + 30000; // between 34 and 35
    late.low = 90;
    r.closeAll({late}, 1);
    CHECK(r.find("stopped") == nullptr);
}

static void testLevel3FollowsANewHighBeforeTheRetest()
{
    std::vector<Candle> v = bullish();
    v.resize(33);
    v.push_back(bar(33, 103.2, 107.5, 103.1, 107.2)); // a new high instead of the retest
    Run r;
    r.closeAll(v, 34);
    const Setup &s = r.engine.setups().back();
    NEAR(s.level3.price, 107.5, 1e-9);
    CHECK(!s.level3Confirmed);
}

static void testNoBosWithoutAClose()
{
    std::vector<Candle> v = bullish();
    v[28] = bar(28, 102.7, 104.6, 102.6, 103.4); // above Level 1 by its wick only
    v.resize(29);
    Run r;
    r.closeAll(v, 29);
    CHECK(r.find("armed") == nullptr);
    CHECK(r.engine.candidate(Direction::Bullish).index >= 0); // still watched
}

static void testRewardRiskAndPdRequirement()
{
    {
        Settings s;
        s.minRewardRisk = 5;
        Run r(s);
        const std::vector<Candle> v = bullish();
        r.closeAll(v, 36); // the entry candle closes without a forming update first
        const Event *e = r.find("skipped");
        CHECK(e != nullptr);
        if (e)
            CHECK(e->setup.note.find("reward/risk") == 0);
        CHECK(r.find("entered") == nullptr);
    }
    {
        Settings s;
        s.displacementAtr = 50; // no order blocks or zones
        s.minGapAtr = 50;       // no gaps
        Run r(s);
        r.closeAll(bullish(), 35);
        const Event *e = r.find("skipped");
        CHECK(e != nullptr);
        if (e)
            CHECK(e->setup.note.find("no PD array") == 0);
        s.requirePdArray = false;
        Run free(s);
        free.closeAll(bullish(), 35);
        CHECK(free.find("triggered") != nullptr);
    }
}

static void testHistoryIsNotTraded()
{
    const std::vector<Candle> v = bullish();
    Run r;
    r.engine.setLive(false);
    r.closeAll(v, 36);
    const Event *in = r.find("entered");
    CHECK(in != nullptr);
    if (in) {
        CHECK(in->setup.historical);
        CHECK(!in->live);
    }
    // Replayed up to the retest, then live: the entry at the forming candle is real.
    Run live;
    live.engine.setLive(false);
    live.closeAll(v, 35);
    live.engine.setLive(true);
    Candle forming = v[35];
    forming.closed = false;
    live.form(forming);
    const Event *real = live.find("entered");
    CHECK(real != nullptr);
    if (real)
        CHECK(!real->setup.historical && real->live);
}

static void testExpiry()
{
    Settings s;
    s.maxArmedCandles = 3;
    Run r(s);
    std::vector<Candle> v = bullish();
    v.resize(30);
    for (int i = 30; i < 36; ++i)
        v.push_back(bar(i, 105.8, 106.2 + i * 0.1, 105.5, 106.0 + i * 0.1)); // higher, never back
    r.closeAll(v, v.size());
    CHECK(r.find("expired") != nullptr);
}

static void testNoBearishCandleAtLevel2()
{
    std::vector<Candle> v = bullish();
    // The fall from Level 1 drawn with bullish candles only (each opens lower).
    v[23] = bar(23, 101.9, 103.3, 101.8, 102.2);
    v[24] = bar(24, 100.9, 102.4, 100.8, 101.2);
    v[25] = bar(25, 100.0, 101.4, 99.9, 100.3);
    Run r;
    r.closeAll(v, 30);
    const Event *e = r.find("invalidated");
    CHECK(e != nullptr);
    if (e)
        CHECK(e->setup.note == "no bearish candle at Level 2");
    CHECK(r.find("armed") == nullptr);
}

static void testPdArrays()
{
    // A bullish gap: candle 1 leaves 101.0..102.0 open between candles 0 and 2.
    Engine e;
    std::vector<Candle> v;
    for (int i = 0; i < 10; ++i)
        v.push_back(bar(i, 100.0, 100.6, 99.6, i % 2 ? 100.0 : 100.3));
    v.push_back(bar(10, 100.3, 101.0, 100.2, 100.9));
    v.push_back(bar(11, 100.9, 103.0, 100.8, 102.8));
    v.push_back(bar(12, 102.8, 103.5, 102.0, 103.3));
    for (const Candle &c : v)
        e.close(c);
    // The gap left by candle 11 (the warm-up may leave small ones of its own).
    const auto theGap = [&e, &v]() -> const PdArray * {
        for (const PdArray &a : e.pdArrays()) {
            if (a.kind == PdKind::FairValueGap && a.bullish && a.time == v[11].openTime)
                return &a;
        }
        return nullptr;
    };
    CHECK(theGap() != nullptr);
    if (const PdArray *a = theGap()) {
        NEAR(a->bottom, 101.0, 1e-9);
        NEAR(a->top, 102.0, 1e-9);
        CHECK(!a->filled && !a->broken);
    }
    // Price fills it, then closes below it.
    e.close(bar(13, 103.3, 103.4, 100.9, 101.2));
    CHECK(theGap() && theGap()->filled && theGap()->touched && !theGap()->broken);
    e.close(bar(14, 101.2, 101.3, 100.0, 100.5));
    CHECK(theGap() && theGap()->broken);
    // A supply zone: a small-bodied base, then a large bearish candle.
    Engine s;
    std::vector<Candle> w;
    for (int i = 0; i < 10; ++i)
        w.push_back(bar(i, 100.0, 100.6, 99.6, i % 2 ? 100.0 : 100.3));
    w.push_back(bar(10, 100.2, 100.9, 100.0, 100.3));
    w.push_back(bar(11, 100.3, 100.4, 97.8, 98.0));
    for (const Candle &c : w)
        s.close(c);
    bool supply = false, block = false;
    for (const PdArray &a : s.pdArrays()) {
        supply = supply || (a.kind == PdKind::Zone && !a.bullish);
        block = block || (a.kind == PdKind::OrderBlock && !a.bullish);
    }
    CHECK(supply);
    CHECK(block);
    CHECK(std::string(pdName(PdKind::Zone, false)) == "supply");
    CHECK(std::string(pdName(PdKind::Zone, true)) == "demand");
}

static void testOldAndRepeatedCandlesAreIgnored()
{
    Run r;
    const std::vector<Candle> v = bullish();
    r.closeAll(v, 30);
    const size_t before = r.engine.setups().size();
    const int count = r.engine.closedCount();
    CHECK(r.engine.close(v[29]).empty()); // the same candle again
    CHECK(r.engine.close(v[10]).empty()); // an old one
    CHECK(r.engine.closedCount() == count);
    CHECK(r.engine.setups().size() == before);
}

// A long random walk: bounded memory, every event well formed, no entry twice.
static void testLongRun()
{
    Engine e;
    e.setLive(true);
    unsigned x = 12345;
    const auto rnd = [&x] { x = x * 1103515245u + 12345u; return double((x >> 8) & 0xffff) / 65535.0; };
    double p = 100;
    int entered = 0;
    std::vector<std::string> ids;
    for (int i = 0; i < 6000; ++i) {
        const double o = p;
        p += (rnd() - 0.5) * 2.0;
        const double c = p;
        const Candle k = bar(i, o, std::max(o, c) + rnd(), std::min(o, c) - rnd(), c);
        for (const Event &ev : e.close(k)) {
            if (ev.kind == "entered") {
                ++entered;
                for (const std::string &id : ids)
                    CHECK(id != ev.setup.id);
                ids.push_back(ev.setup.id);
                CHECK(ev.setup.direction == Direction::Bullish ? ev.setup.stop < ev.setup.entry && ev.setup.entry < ev.setup.target
                                                               : ev.setup.target < ev.setup.entry && ev.setup.entry < ev.setup.stop);
                CHECK(ev.setup.rewardRisk >= 1.5 - 1e-9);
            }
        }
        CHECK(e.setups().size() <= 40);
        CHECK(e.pdArrays().size() <= 80);
    }
    CHECK(entered > 0);
}

int main()
{
    testBullish();
    testBearish();
    testCloseThroughTheZoneInvalidates();
    testWickBelowButCloseInsideIsARetest();
    testAnEarlyRetestCounts();
    testStopIsBeyondLevel2();
    testIdsCarryThePrefix();
    testOutcomeNeverFromBeforeTheEntry();
    testLevel3FollowsANewHighBeforeTheRetest();
    testNoBosWithoutAClose();
    testRewardRiskAndPdRequirement();
    testHistoryIsNotTraded();
    testExpiry();
    testNoBearishCandleAtLevel2();
    testPdArrays();
    testOldAndRepeatedCandlesAreIgnored();
    testLongRun();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
