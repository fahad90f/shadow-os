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
        NEAR(s.zoneTop, 101.2, 1e-9);           // from the zone candle's open to its low
        CHECK(s.zoneTime == v[25].openTime);
        CHECK(s.orderBlock && s.demand);        // the same candle is the order block and the demand zone
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
        NEAR(s.zoneTop, 100.0, 1e-9);           // the highest bullish candle, from its open to its high: 98.8..100.0
        NEAR(s.zoneBottom, 200 - 101.2, 1e-9);
        CHECK(s.orderBlock && s.demand);        // order block and supply zone
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
    v.push_back(bar(29, 104.4, 104.5, 101.1, 101.8));
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
    w.push_back(bar(29, 104.4, 105.5, 101.1, 101.8));
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
        s.requirePdArray = true; // the extra filter, off by default
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

// The owner's own SBT Model 1 chart (bullish), candle by candle: a rise to
// Level 1 (candle 6, high 128), a pull-back of bearish candles whose lowest
// is candle 10 (Level 2 and the zone, 78..91), a rally that closes above
// Level 1 at candle 14 (the BOS) and runs to Level 3 at candle 17 (182), a
// fall back into the zone: candle 24 tests it (low 85) and closes inside it
// (88), so the BUY is the open of candle 25 (88) and the target Level 3. No
// fair value gap, order block or demand zone is needed: the original rules
// have none.
static std::vector<Candle> ownerBullish()
{
    std::vector<Candle> v;
    v.push_back(bar(0, 50, 57, 49, 56));
    v.push_back(bar(1, 56, 63, 55, 62));
    v.push_back(bar(2, 62, 71, 61, 70));
    v.push_back(bar(3, 70, 86, 69, 85));
    v.push_back(bar(4, 85, 101, 70, 100));    // overlapping wicks: no gaps under the zone
    v.push_back(bar(5, 100, 112, 85, 110));
    v.push_back(bar(6, 110, 128, 109, 122));  // 1
    v.push_back(bar(7, 122, 127, 110, 112));
    v.push_back(bar(8, 112, 113, 98, 100));
    v.push_back(bar(9, 100, 101, 88, 90));
    v.push_back(bar(10, 90, 91, 78, 82));     // 2: the lowest bearish candle
    v.push_back(bar(11, 82, 94, 79, 92));
    v.push_back(bar(12, 92, 106, 91, 104));
    v.push_back(bar(13, 104, 118, 103, 116));
    v.push_back(bar(14, 116, 135, 115, 133)); // BOS: closes above 128
    v.push_back(bar(15, 133, 152, 132, 150));
    v.push_back(bar(16, 150, 167, 149, 165));
    v.push_back(bar(17, 165, 182, 164, 178)); // 3
    v.push_back(bar(18, 178, 181, 168, 172));
    v.push_back(bar(19, 172, 173, 156, 158));
    v.push_back(bar(20, 158, 159, 141, 143));
    v.push_back(bar(21, 143, 144, 126, 128));
    v.push_back(bar(22, 128, 129, 110, 112));
    v.push_back(bar(23, 112, 113, 95, 97));
    v.push_back(bar(24, 97, 98, 85, 88));     // the test: into the zone, closes inside it
    v.push_back(bar(25, 88, 100, 87, 98));    // BUY at its open
    v.push_back(bar(26, 98, 130, 97, 128));
    v.push_back(bar(27, 128, 160, 127, 158));
    v.push_back(bar(28, 158, 185, 157, 183)); // Level 3 reached
    return v;
}

static void testTheOwnersChart(const std::vector<Candle> &v, bool bull)
{
    const double top = bull ? 90 : 200 - 78, bottom = bull ? 78 : 200 - 90; // open to wick
    Run r; // the default rules, nothing switched off
    r.closeAll(v, 25);
    const Event *armed = r.find("armed");
    CHECK(armed != nullptr);
    if (armed) {
        const Setup &s = armed->setup;
        CHECK(s.direction == (bull ? Direction::Bullish : Direction::Bearish));
        NEAR(s.level1.price, bull ? 128 : 72, 1e-9);
        CHECK(s.level1.time == v[6].openTime);
        NEAR(s.level2.price, bull ? 78 : 122, 1e-9);
        CHECK(s.level2.time == v[10].openTime);
        CHECK(s.bos.time == v[14].openTime);
        CHECK(s.zoneTime == v[10].openTime);
        NEAR(s.zoneTop, top, 1e-9);
        NEAR(s.zoneBottom, bottom, 1e-9);
    }
    CHECK(r.find("skipped") == nullptr);
    CHECK(r.find("invalidated") == nullptr);
    const Event *trig = r.find("triggered");
    CHECK(trig != nullptr);
    if (trig) {
        CHECK(trig->setup.retest.time == v[24].openTime);
        NEAR(trig->setup.level3.price, bull ? 182 : 18, 1e-9);
        CHECK(trig->setup.level3.time == v[17].openTime);
        NEAR(trig->setup.target, bull ? 182 : 18, 1e-9);
        CHECK(bull ? trig->setup.stop < 78 : trig->setup.stop > 122); // beyond the zone
    }
    CHECK(r.find("entered") == nullptr); // only when the next candle opens
    Candle forming = v[25];
    forming.closed = false;
    forming.high = forming.low = forming.close = forming.open;
    r.form(forming);
    const Event *in = r.find("entered");
    CHECK(in != nullptr);
    if (in) {
        NEAR(in->setup.entry, bull ? 88 : 112, 1e-9);
        CHECK(in->setup.entryTime == v[25].openTime);
    }
    r.closeAll(v, v.size());
    CHECK(r.count("entered") == 1);
    const Event *hit = r.find("target");
    CHECK(hit != nullptr);
    if (hit)
        CHECK(hit->setup.outcomeTime == v[28].openTime);
    CHECK(r.find("stopped") == nullptr);

    // Rule 5: the testing candle must not close beyond the zone.
    std::vector<Candle> broken = v;
    broken[24] = bull ? bar(24, 97, 98, 74, 76) : mirror({bar(24, 97, 98, 74, 76)})[0];
    broken[24].openTime = v[24].openTime;
    Run b;
    b.closeAll(broken, broken.size());
    const Event *bad = b.find("invalidated");
    CHECK(bad != nullptr);
    if (bad)
        CHECK(bad->setup.note.find(bull ? "closed below" : "closed above") != std::string::npos);
    CHECK(b.find("entered") == nullptr);
}

static void testOwnersCharts()
{
    testTheOwnersChart(ownerBullish(), true);
    testTheOwnersChart(mirror(ownerBullish()), false); // the bearish chart is the same picture upside down
}

// External, not internal. The pull-back from Level 1 (candle 6, 128) has an
// internal lower high at candle 10 (112, a swing point with two lower
// candles on each side) and the rally after Level 2 (candle 14, 78) closes
// above it at 17: an internal break, not a setup. The external BOS is the
// close above 128 at 19. The leg up then pauses at candle 20 (150, again a
// two-candle swing point) without a valid pullback (no close below candle
// 20's low): internal, so 23's close above 150 is no setup either, and
// Level 3 is the leg's top at 24 (182), confirmed by 26's close below 24's
// low. The test at 31 closes inside the zone: BUY at 32's open.
static std::vector<Candle> internalSwings()
{
    std::vector<Candle> v;
    v.push_back(bar(0, 50, 57, 49, 56));
    v.push_back(bar(1, 56, 63, 55, 62));
    v.push_back(bar(2, 62, 71, 61, 70));
    v.push_back(bar(3, 70, 86, 69, 85));
    v.push_back(bar(4, 85, 101, 70, 100));
    v.push_back(bar(5, 100, 112, 85, 110));
    v.push_back(bar(6, 110, 128, 109, 122));  // 1 (external high)
    v.push_back(bar(7, 122, 124, 104, 106));  // closes below 6's low: 128 is confirmed
    v.push_back(bar(8, 106, 107, 96, 98));
    v.push_back(bar(9, 98, 105, 97, 104));
    v.push_back(bar(10, 104, 112, 103, 110)); // internal lower high
    v.push_back(bar(11, 110, 111, 100, 102));
    v.push_back(bar(12, 102, 103, 92, 94));
    v.push_back(bar(13, 94, 95, 86, 88));
    v.push_back(bar(14, 88, 89, 78, 80));     // 2: the lowest bearish candle
    v.push_back(bar(15, 80, 92, 79, 90));
    v.push_back(bar(16, 90, 104, 89, 102));
    v.push_back(bar(17, 102, 116, 101, 114)); // closes above the internal 112: no setup
    v.push_back(bar(18, 114, 124, 112, 122));
    v.push_back(bar(19, 122, 135, 120, 133)); // external BOS: closes above 128
    v.push_back(bar(20, 133, 150, 132, 148)); // internal pause in the leg
    v.push_back(bar(21, 148, 150, 143, 145));
    v.push_back(bar(22, 145, 147, 140, 142));
    v.push_back(bar(23, 142, 165, 141, 163)); // closes above the internal 150: no setup
    v.push_back(bar(24, 163, 182, 162, 178)); // 3
    v.push_back(bar(25, 178, 180, 168, 170));
    v.push_back(bar(26, 170, 171, 155, 158)); // closes below 24's low: 182 is confirmed
    v.push_back(bar(27, 158, 159, 140, 142));
    v.push_back(bar(28, 142, 143, 124, 126));
    v.push_back(bar(29, 126, 127, 108, 110));
    v.push_back(bar(30, 110, 111, 94, 96));
    v.push_back(bar(31, 96, 97, 84, 87));     // the test: into the zone, closes inside it
    v.push_back(bar(32, 87, 100, 86, 98));    // BUY at its open
    v.push_back(bar(33, 98, 130, 97, 128));
    v.push_back(bar(34, 128, 160, 127, 158));
    v.push_back(bar(35, 158, 175, 157, 170));
    v.push_back(bar(36, 170, 185, 169, 181)); // Level 3 reached (no close above 182)
    return v;
}

static void testExternalNotInternal(const std::vector<Candle> &v, bool bull)
{
    Run r;
    r.closeAll(v, 32);
    CHECK(r.count("armed") == 1);
    const Event *armed = r.find("armed");
    CHECK(armed != nullptr);
    if (armed) {
        const Setup &s = armed->setup;
        CHECK(s.level1.time == v[6].openTime);
        NEAR(s.level1.price, bull ? 128 : 72, 1e-9);
        CHECK(s.bos.time == v[19].openTime);
        CHECK(s.level2.time == v[14].openTime);
        NEAR(s.level2.price, bull ? 78 : 122, 1e-9);
        CHECK(s.zoneTime == v[14].openTime);
    }
    // Internal swing points were seen, and none became Level 1.
    bool sawInternal = false;
    for (const Swing &w : r.engine.swings())
        sawInternal = sawInternal || w.point.time == v[10].openTime || w.point.time == v[20].openTime;
    CHECK(sawInternal);
    const Event *trig = r.find("triggered");
    CHECK(trig != nullptr);
    if (trig) {
        CHECK(trig->setup.retest.time == v[31].openTime);
        CHECK(trig->setup.level3.time == v[24].openTime); // the leg's top, not the internal 150
        NEAR(trig->setup.target, bull ? 182 : 18, 1e-9);
    }
    // The external points now: Level 3 is the external high (bearish: low),
    // Level 2 the external low (high).
    const Point hi = r.engine.candidate(bull ? Direction::Bullish : Direction::Bearish);
    const Point lo = r.engine.candidate(bull ? Direction::Bearish : Direction::Bullish);
    CHECK(hi.time == v[24].openTime);
    CHECK(lo.time == v[14].openTime);
    Candle forming = v[32];
    forming.closed = false;
    forming.high = forming.low = forming.close = forming.open;
    r.form(forming);
    const Event *in = r.find("entered");
    CHECK(in != nullptr);
    if (in)
        NEAR(in->setup.entry, bull ? 87 : 113, 1e-9);
    r.closeAll(v, v.size());
    CHECK(r.count("entered") == 1);
    CHECK(r.find("target") != nullptr);
    CHECK(r.count("armed") == 1);
}

static void testExternalStructure()
{
    testExternalNotInternal(internalSwings(), true);
    testExternalNotInternal(mirror(internalSwings()), false);
}

// Rules 3/4: the zone candle must be both the order block and the demand
// (bearish: supply) zone, or the setup is skipped.
// The setup of the BOS at candle 14 (Level 1 at candle 6) is skipped for 'why',
// never armed or entered.
static void expectSkipped(const std::vector<Candle> &v, const Settings &settings, const std::string &why)
{
    Run r(settings);
    r.closeAll(v, v.size());
    const Event *skipped = nullptr;
    for (const Event &e : r.events) {
        if (e.setup.level1.time != v[6].openTime)
            continue;
        CHECK(e.kind != "armed" && e.kind != "entered");
        if (e.kind == "skipped" && !skipped)
            skipped = &e;
    }
    CHECK(skipped != nullptr);
    if (skipped && skipped->setup.note.find(why) == std::string::npos) {
        CHECK(skipped->setup.note.find(why) != std::string::npos);
        std::printf("  note: %s\n", skipped->setup.note.c_str());
    }
}

static void testZoneIsDemandAndOrderBlock(bool bull)
{
    const auto chart = [bull](std::vector<Candle> v) { return bull ? v : mirror(v); };
    const auto keep = [](std::vector<Candle> v, int i, const Candle &c) { Candle x = c; x.openTime = v[size_t(i)].openTime; v[size_t(i)] = x; return v; };
    // The candle after the zone candle goes down too: the zone candle is not the
    // last bearish candle before the move, so not the order block.
    {
        std::vector<Candle> v = keep(ownerBullish(), 11, bar(11, 82, 84, 79, 80));
        v = keep(v, 12, bar(12, 80, 106, 79.5, 104)); // opens where 11 closed: no gap
        expectSkipped(chart(v), Settings(), "no order block");
    }
    // No close above the zone candle's high (91) within three candles.
    {
        std::vector<Candle> v = ownerBullish();
        v = keep(v, 11, bar(11, 82, 88, 79, 86));
        v = keep(v, 12, bar(12, 86, 90, 84, 88));
        v = keep(v, 13, bar(13, 88, 91, 85, 89));
        v = keep(v, 14, bar(14, 89, 135, 88, 133));
        expectSkipped(chart(v), Settings(), "no order block");
    }
    // Price comes back into the zone (its open, 90) before the BOS: not fresh.
    expectSkipped(chart(keep(ownerBullish(), 12, bar(12, 92, 106, 89, 104))), Settings(),
                  bull ? "not a fresh demand zone" : "not a fresh supply zone");
    // A move away that is too small for a demand (supply) zone.
    Settings strict;
    strict.zoneMoveAtr = 50;
    expectSkipped(chart(ownerBullish()), strict, bull ? "not a demand zone" : "not a supply zone");
    // And the owner's chart as it is passes: both found on the zone candle.
    Run r;
    r.closeAll(chart(ownerBullish()), 25);
    const Event *a = r.find("armed");
    CHECK(a && a->setup.orderBlock && a->setup.demand);
}

// Rule 5: after a gap up or down nothing is set up or entered until the gap
// is filled or the pause has run out.
static void testGapFilter(bool bull)
{
    const auto chart = [bull](std::vector<Candle> v) { return bull ? v : mirror(v); };
    const auto keep = [](std::vector<Candle> v, int i, const Candle &c) { Candle x = c; x.openTime = v[size_t(i)].openTime; v[size_t(i)] = x; return v; };
    // Candle 13 opens at 115, above candle 12's high (106) and 11 from its close:
    // a gap up. The BOS at 14 comes inside the pause: skipped.
    {
        const std::vector<Candle> v = chart(keep(ownerBullish(), 13, bar(13, 115, 118, 114, 116)));
        Run r;
        r.closeAll(v, 14);
        CHECK(r.engine.gap().active);
        CHECK(r.engine.gap().time == v[13].openTime);
        CHECK(r.engine.gap().up == bull);
        NEAR(r.engine.gap().fill, bull ? 104 : 96, 1e-9);
        expectSkipped(v, Settings(), "gap");
        // The pause runs out after six more candles.
        Run q;
        q.closeAll(v, 19);
        CHECK(q.engine.gap().active);
        q.closeAll(v, 20);
        CHECK(!q.engine.gap().active);
    }
    // The same gap, filled inside its own candle (back down to 104): trading
    // goes on, and the BOS at 14 is a setup.
    {
        Run r;
        r.closeAll(chart(keep(ownerBullish(), 13, bar(13, 115, 118, 103.5, 116))), 15);
        CHECK(!r.engine.gap().active);
        CHECK(r.find("armed") != nullptr);
    }
    // The entry candle opens with a gap (75, under the retest candle's low 85): no entry.
    {
        const std::vector<Candle> v = chart(ownerBullish());
        Run r;
        r.closeAll(v, 25);
        CHECK(r.find("triggered") != nullptr);
        Candle forming = v[25];
        forming.open = bull ? 75 : 125;
        forming.high = forming.low = forming.close = forming.open;
        forming.closed = false;
        r.form(forming);
        CHECK(r.find("entered") == nullptr);
        const Event *e = r.find("skipped");
        CHECK(e && e->setup.note.find("gap") != std::string::npos);
    }
    // No gap: a normal open is no gap.
    {
        Run r;
        r.closeAll(chart(ownerBullish()), ownerBullish().size());
        CHECK(!r.engine.gap().active);
        CHECK(r.count("entered") == 1);
    }
}

static void testOwnersRules()
{
    testZoneIsDemandAndOrderBlock(true);
    testZoneIsDemandAndOrderBlock(false);
    testGapFilter(true);
    testGapFilter(false);
}

int main()
{
    testOwnersRules();
    testExternalStructure();
    testOwnersCharts();
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
