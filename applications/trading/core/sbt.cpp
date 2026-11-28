// SPDX-License-Identifier: GPL-3.0-or-later
#include "sbt.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace Trading::Sbt {

namespace {

const int kMinWindow = 400;   // closed candles kept at least
const int kMaxWindow = 3000;  // and at most
const size_t kMaxArrays = 80;
const int kAtrPeriod = 14;
const int kAtrSlowPeriod = 100;

bool bullishCandle(const Candle &c) { return c.close > c.open; }
bool bearishCandle(const Candle &c) { return c.close < c.open; }
double body(const Candle &c) { return std::fabs(c.close - c.open); }

std::string fixed(double v, int decimals)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    return buf;
}

} // namespace

const char *directionName(Direction d)
{
    return d == Direction::Bullish ? "bullish" : "bearish";
}

const char *statusName(Status s)
{
    switch (s) {
    case Status::Armed: return "armed";
    case Status::Triggered: return "triggered";
    case Status::Entered: return "entered";
    case Status::Target: return "target";
    case Status::Stopped: return "stopped";
    case Status::Invalidated: return "invalidated";
    case Status::Skipped: return "skipped";
    case Status::Expired: return "expired";
    }
    return "armed";
}

bool finished(Status s)
{
    return s == Status::Target || s == Status::Stopped || s == Status::Invalidated || s == Status::Skipped
        || s == Status::Expired;
}

const char *pdName(PdKind kind, bool bullish)
{
    switch (kind) {
    case PdKind::FairValueGap: return "fvg";
    case PdKind::OrderBlock: return "order_block";
    case PdKind::Zone: return bullish ? "demand" : "supply";
    }
    return "fvg";
}

bool sameRules(const Settings &a, const Settings &b)
{
    return a.legAtr == b.legAtr && a.minPullbackCandles == b.minPullbackCandles && a.supersede == b.supersede
        && a.firstReturn == b.firstReturn && a.extensionAtr == b.extensionAtr && a.entryZoneAtr == b.entryZoneAtr
        && a.targetBody == b.targetBody && a.zoneMoveAtr == b.zoneMoveAtr && a.gapAtr == b.gapAtr
        && a.gapPauseCandles == b.gapPauseCandles && a.stopBufferAtr == b.stopBufferAtr
        && a.minRewardRisk == b.minRewardRisk && a.requirePdArray == b.requirePdArray
        && a.pdDistanceAtr == b.pdDistanceAtr && a.displacementAtr == b.displacementAtr && a.minGapAtr == b.minGapAtr
        && a.maxArmedCandles == b.maxArmedCandles;
}

Engine::Engine(const Settings &settings)
    : m_settings(settings)
{
}

void Engine::reset()
{
    const Settings keep = m_settings;
    const bool live = m_live;
    *this = Engine(keep);
    m_live = live;
}

bool Engine::inWindow(int index) const
{
    return index >= m_first && index < m_first + int(m_window.size());
}

const Candle &Engine::at(int index) const
{
    return m_window[size_t(index - m_first)];
}

void Engine::updateAtr(const Candle &c)
{
    double tr = c.high - c.low;
    if (m_window.size() >= 2) {
        const Candle &prev = m_window[m_window.size() - 2];
        tr = std::max({tr, std::fabs(c.high - prev.close), std::fabs(c.low - prev.close)});
    }
    ++m_atrCount;
    if (m_atrCount <= kAtrSlowPeriod)
        m_atrSlow += (tr - m_atrSlow) / m_atrCount;
    else
        m_atrSlow = (m_atrSlow * (kAtrSlowPeriod - 1) + tr) / kAtrSlowPeriod;
    if (m_atrCount <= kAtrPeriod)
        m_atr += (tr - m_atr) / m_atrCount; // the plain average while there are fewer than 14
    else
        m_atr = (m_atr * (kAtrPeriod - 1) + tr) / kAtrPeriod;
}

void Engine::emit(const std::string &kind, const Setup &s, std::vector<Event> *events) const
{
    events->push_back({kind, s, m_live});
}

std::vector<Event> Engine::close(const Candle &input)
{
    std::vector<Event> events;
    if (!m_window.empty() && input.openTime <= m_window.back().openTime)
        return events;
    Candle c = input;
    c.closed = true;
    const int n = m_count;
    const double atrBefore = m_atr;
    m_window.push_back(c);
    ++m_count;
    updateAtr(c);
    // Rule 5: a candle that opens with a gap starts a pause.
    if (n > 0 && opensWithGap(m_window[m_window.size() - 2], c, atrBefore)) {
        const Candle &prev = m_window[m_window.size() - 2];
        m_gap = {true, c.open > prev.close, prev.close, c.openTime, n, std::max(0, m_settings.gapPauseCandles)};
    }

    for (Setup &s : m_setups) {
        if (s.status == Status::Triggered)
            enter(s, c, &events);        // no forming candle was seen: this one opened the trade
        else if (s.status == Status::Entered && c.openTime >= s.entryTime)
            outcome(s, c, true, &events); // never from a candle before the entry
        else if (s.status == Status::Armed && s.bos.index < n)
            advance(s, c, &events);
    }
    updateArrays(c);
    detectArrays();
    updateLegs(c);
    checkBos(c, &events);
    // The pause ends once the gap is filled or its candles have passed.
    if (m_gap.active) {
        const bool filled = m_gap.up ? c.low <= m_gap.fill : c.high >= m_gap.fill;
        if (filled || (n > m_gap.index && --m_gap.left <= 0) || (n == m_gap.index && m_gap.left == 0))
            m_gap.active = false;
    }
    for (Setup &s : m_setups) {
        if (s.status == Status::Armed)
            s.intrabar = false;
    }
    trim();
    return events;
}

std::vector<Event> Engine::forming(const Candle &c)
{
    std::vector<Event> events;
    if (m_window.empty() || c.openTime <= m_window.back().openTime)
        return events;
    m_formingTime = c.openTime;
    for (Setup &s : m_setups) {
        if (s.status == Status::Triggered) {
            enter(s, c, &events); // a gap at its open (or a pause) skips it there
        } else if (s.status == Status::Entered && c.openTime >= s.entryTime) {
            outcome(s, c, false, &events);
        } else if (s.status == Status::Armed) {
            // Shown only: a retest counts when the candle has closed.
            const bool bull = s.direction == Direction::Bullish;
            const bool in = bull ? c.low <= s.zoneTop : c.high >= s.zoneBottom;
            const bool newExtreme = bull ? c.high > s.level3.price : c.low < s.level3.price;
            s.intrabar = in && !newExtreme;
        }
    }
    return events;
}

bool Engine::opensWithGap(const Candle &prev, const Candle &c, double atr) const
{
    if (!(atr > 0) || !(m_settings.gapAtr > 0))
        return false;
    const bool outside = c.open > prev.high || c.open < prev.low;
    return outside && std::fabs(c.open - prev.close) >= m_settings.gapAtr * atr;
}

std::string Engine::zoneCheck(Setup &s, int z, int n) const
{
    const bool bull = s.direction == Direction::Bullish;
    const Candle &zc = at(z);
    if (z + 1 >= n + 1 || !inWindow(z + 1))
        return bull ? "no order block: no candle after the lowest bearish candle"
                    : "no order block: no candle after the highest bullish candle";
    // Order block: the last opposite candle before the move: the next candle
    // goes the other way, and within three candles one closes beyond it.
    const Candle &next = at(z + 1);
    if (bull ? !bullishCandle(next) : !bearishCandle(next))
        return bull ? "no order block: the candle after the zone candle is not bullish"
                    : "no order block: the candle after the zone candle is not bearish";
    const int last = std::min(z + 3, n);
    int broke = -1;
    double best = bull ? next.close : next.close;
    for (int i = z + 1; i <= last; ++i) {
        const Candle &x = at(i);
        best = bull ? std::max(best, x.close) : std::min(best, x.close);
        if (broke < 0 && (bull ? x.close > zc.high : x.close < zc.low))
            broke = i;
    }
    if (broke < 0)
        return bull ? "no order block: no close above the zone candle's high within three candles"
                    : "no order block: no close below the zone candle's low within three candles";
    s.orderBlock = true;
    // Demand (supply): a strong move away, and fresh until the BOS.
    const double move = bull ? best - zc.open : zc.open - best;
    if (!(move >= m_settings.zoneMoveAtr * m_atr))
        return bull ? "not a demand zone: the move away from it is too weak"
                    : "not a supply zone: the move away from it is too weak";
    for (int i = broke + 1; i < n; ++i) {
        const Candle &x = at(i);
        if (bull ? x.low <= s.zoneTop : x.high >= s.zoneBottom)
            return bull ? "not a fresh demand zone: price came back into it before the BOS"
                        : "not a fresh supply zone: price came back into it before the BOS";
    }
    s.demand = true;
    return {};
}

void Engine::recordPoint(const Point &p, bool high, const std::string &label)
{
    StructurePoint sp;
    sp.point = p;
    sp.high = high;
    sp.label = label;
    if (sp.label.empty()) {
        for (auto it = m_structure.rbegin(); it != m_structure.rend(); ++it) {
            if (it->high != high)
                continue;
            sp.label = high ? (p.price > it->point.price ? "HH" : "LH") : (p.price > it->point.price ? "HL" : "LL");
            break;
        }
    }
    m_structure.push_back(sp);
    if (m_structure.size() > 200)
        m_structure.erase(m_structure.begin(), m_structure.begin() + long(m_structure.size() - 200));
}

// The structure in proper legs. A leg ends, and its extreme becomes a
// confirmed high or low, once price has moved against it by 'legAtr' ATRs
// over at least 'minPullbackCandles' candles. Small moves inside a leg never
// make a high or low of the structure.
void Engine::updateLegs(const Candle &c)
{
    const int n = m_count - 1;
    const double need = legNeed();
    if (m_legDir == 0) {
        if (m_startHigh.index < 0 || c.high > m_startHigh.price)
            m_startHigh = {c.openTime, c.high, n};
        if (m_startLow.index < 0 || c.low < m_startLow.price)
            m_startLow = {c.openTime, c.low, n};
        if (!(need > 0) || m_startHigh.index == m_startLow.index || m_startHigh.price - m_startLow.price < need)
            return;
        // The first leg runs from the earlier of the two to the later.
        const bool up = m_startHigh.index > m_startLow.index;
        m_legDir = up ? 1 : -1;
        m_ext = up ? m_startHigh : m_startLow;
        m_counter = Point();
        recordPoint(up ? m_startLow : m_startHigh, !up);
        (up ? m_pivotLow : m_pivotHigh) = up ? m_startLow : m_startHigh;
        m_startHigh = m_startLow = Point();
        return;
    }
    const bool up = m_legDir > 0;
    if (up ? c.high > m_ext.price : c.low < m_ext.price) {
        m_ext = {c.openTime, up ? c.high : c.low, n};
        m_counter = Point();
        return;
    }
    if (m_counter.index < 0 || (up ? c.low < m_counter.price : c.high > m_counter.price))
        m_counter = {c.openTime, up ? c.low : c.high, n};
    const double moved = up ? m_ext.price - m_counter.price : m_counter.price - m_ext.price;
    if (!(need > 0) || moved < need || m_counter.index - m_ext.index < m_settings.minPullbackCandles)
        return;
    // The leg is over: its extreme is a confirmed high (low), and the move
    // against it is the new leg.
    recordPoint(m_ext, up);
    (up ? m_pivotHigh : m_pivotLow) = m_ext;
    m_legDir = -m_legDir;
    m_ext = m_counter;
    m_counter = Point();
    for (int i = std::max(m_first, m_ext.index + 1); i <= n; ++i) {
        const Candle &x = at(i);
        if (m_counter.index < 0 || (up ? x.high > m_counter.price : x.low < m_counter.price))
            m_counter = {x.openTime, up ? x.high : x.low, i};
    }
}

void Engine::checkBos(const Candle &c, std::vector<Event> *events)
{
    const int n = m_count - 1;
    for (Direction d : {Direction::Bullish, Direction::Bearish}) {
        const bool bull = d == Direction::Bullish;
        Point &level1 = bull ? m_pivotHigh : m_pivotLow;
        if (level1.index < 0)
            continue;
        if (!(bull ? c.close > level1.price : c.close < level1.price))
            continue;
        const Point one = level1;
        level1 = Point(); // broken once
        // Level 2: the extreme between Level 1 and the BOS.
        int l2 = -1;
        if (inWindow(one.index)) {
            for (int i = one.index + 1; i < n; ++i) {
                const Candle &x = at(i);
                if (l2 < 0 || (bull ? x.low < at(l2).low : x.high > at(l2).high))
                    l2 = i;
            }
        }
        // The previous low: the last confirmed low before Level 1 (bearish:
        // the last confirmed high). Level 2 must be a Higher Low above it
        // (a Lower High below it).
        Point previous;
        for (auto it = m_structure.rbegin(); it != m_structure.rend(); ++it) {
            if (it->high != bull && it->point.index < one.index) {
                previous = it->point;
                break;
            }
        }
        std::string label;
        Point three{c.openTime, bull ? c.high : c.low, n};
        if (l2 >= 0) {
            const Point two{at(l2).openTime, bull ? at(l2).low : at(l2).high, l2};
            if (previous.index >= 0)
                label = bull ? (two.price > previous.price ? "HL" : "LL") : (two.price < previous.price ? "LH" : "HH");
            // Level 2 is a confirmed low (high) now, whatever the legs had made of it so far.
            bool recorded = false;
            for (auto it = m_structure.rbegin(); it != m_structure.rend() && it->point.index >= two.index; ++it)
                recorded = recorded || (it->high != bull && it->point.index == two.index);
            if (!recorded)
                recordPoint(two, !bull, label);
            (bull ? m_pivotLow : m_pivotHigh) = two;
            // Level 3 starts at the furthest point since Level 2.
            for (int i = l2 + 1; i < n; ++i) {
                const Candle &x = at(i);
                if (bull ? x.high > three.price : x.low < three.price)
                    three = {x.openTime, bull ? x.high : x.low, i};
            }
        }
        // The leg after the break runs from Level 2.
        m_legDir = bull ? 1 : -1;
        m_ext = three;
        m_counter = Point();
        for (int i = three.index + 1; i <= n; ++i) {
            const Candle &x = at(i);
            if (m_counter.index < 0 || (bull ? x.low < m_counter.price : x.high > m_counter.price))
                m_counter = {x.openTime, bull ? x.low : x.high, i};
        }
        if (l2 < 0)
            continue;
        Setup s;
        s.direction = d;
        s.id = m_prefix + directionName(d) + ":" + std::to_string(one.time);
        bool known = false;
        for (const Setup &other : m_setups)
            known = known || other.id == s.id;
        if (known)
            continue;
        s.level1 = one;
        for (auto it = m_structure.rbegin(); it != m_structure.rend(); ++it) {
            if (it->high == bull && it->point.index == one.index) {
                s.level1Label = it->label;
                break;
            }
        }
        s.bos = {c.openTime, c.close, n};
        s.updated = c.openTime;
        s.level2 = {at(l2).openTime, bull ? at(l2).low : at(l2).high, l2};
        s.previous = previous;
        // The zone: the lowest bearish candle (bullish setup) or the highest
        // bullish candle (bearish setup) between Level 1 and the BOS.
        int z = -1;
        for (int i = one.index + 1; i < n; ++i) {
            const Candle &x = at(i);
            if (bull ? !bearishCandle(x) : !bullishCandle(x))
                continue;
            if (z < 0 || (bull ? x.low < at(z).low : x.high > at(z).high))
                z = i;
        }
        s.level3 = three;
        if (z < 0) {
            s.status = Status::Invalidated;
            s.note = bull ? "no bearish candle at Level 2" : "no bullish candle at Level 2";
            m_setups.push_back(s);
            emit("invalidated", s, events);
            continue;
        }
        const Candle &zc = at(z);
        s.zoneTime = zc.openTime;
        // From the candle's open to its wick (rule 1/2).
        if (bull) {
            s.zoneBottom = zc.low;
            s.zoneTop = zc.open;
        } else {
            s.zoneTop = zc.high;
            s.zoneBottom = zc.open;
        }
        // Rule 5: nothing is set up while the market has gapped.
        // Rules 3/4: the zone candle must be the order block and the demand
        // (supply) zone.
        std::string why;
        if (m_gap.active)
            why = "the market opened with a gap: no setups until it trades normally again";
        else if (previous.index < 0)
            why = bull ? "no valid Higher Low (HL): there is no previous low to compare Level 2 with"
                       : "no valid Lower High (LH): there is no previous high to compare Level 2 with";
        else if (label != (bull ? "HL" : "LH"))
            why = bull ? "no valid Higher Low (HL): Level 2 is a lower low (LL), not above the previous low ("
                             + fixed(previous.price, 5) + ")"
                       : "no valid Lower High (LH): Level 2 is a higher high (HH), not below the previous high ("
                             + fixed(previous.price, 5) + ")";
        else
            why = zoneCheck(s, z, n);
        if (!why.empty()) {
            s.status = Status::Skipped;
            s.note = why;
            s.outcomeTime = c.openTime;
            m_setups.push_back(s);
            emit("skipped", s, events);
            continue;
        }
        s.status = Status::Armed;
        m_setups.push_back(s);
        emit("armed", s, events);
    }
}

void Engine::advance(Setup &s, const Candle &c, std::vector<Event> *events)
{
    const int n = m_count - 1;
    const bool bull = s.direction == Direction::Bullish;
    // Level 3 follows the leg after the BOS until a proper pullback confirms
    // it ('legAtr' ATRs over 'minPullbackCandles' candles, as for every high
    // and low of the structure). A candle that reaches the zone without a
    // new extreme confirms it too: the turn is plain.
    const bool newExtreme = bull ? c.high > s.level3.price : c.low < s.level3.price;
    const bool reached = bull ? c.low <= s.zoneTop : c.high >= s.zoneBottom;
    if (newExtreme) {
        // A close beyond a confirmed Level 3 is the BOS of a newer structure:
        // this one's zone was never tested, and it is left behind.
        if (s.level3Confirmed && m_settings.supersede && (bull ? c.close > s.level3.price : c.close < s.level3.price)) {
            s.status = Status::Expired;
            s.note = "a newer structure formed before the zone was tested (a close beyond Level 3)";
            s.outcomeTime = c.openTime;
            s.updated = c.openTime;
            emit("expired", s, events);
            return;
        }
        s.level3 = {c.openTime, bull ? c.high : c.low, n};
        s.level3Confirmed = false;
        s.level3Pull = 0;
        s.pullEnd = Point();
        s.bounce = 0;
    } else {
        s.level3Pull = std::max(s.level3Pull, bull ? s.level3.price - c.low : c.high - s.level3.price);
        const bool pulled = s.level3Pull >= legNeed() && n - s.level3.index >= m_settings.minPullbackCandles;
        if (!s.level3Confirmed && (pulled || reached))
            s.level3Confirmed = true;
        // The pullback's furthest point, and how far price has bounced from it.
        const double far = bull ? c.low : c.high;
        if (s.pullEnd.index < 0 || (bull ? far < s.pullEnd.price : far > s.pullEnd.price)) {
            s.pullEnd = {c.openTime, far, n};
            s.bounce = 0;
        } else {
            s.bounce = std::max(s.bounce, bull ? c.high - s.pullEnd.price : s.pullEnd.price - c.low);
        }
    }
    s.updated = c.openTime;
    // A close through the zone ends it, retest or not.
    if (bull ? c.close < s.zoneBottom : c.close > s.zoneTop) {
        s.status = Status::Invalidated;
        s.note = bull ? "a candle closed below the Level 2 zone" : "a candle closed above the Level 2 zone";
        s.outcomeTime = c.openTime;
        emit("invalidated", s, events);
        return;
    }
    if (s.level3Confirmed && reached && !newExtreme && m_gap.active) {
        s.retest = {c.openTime, c.close, n};
        s.status = Status::Skipped;
        s.note = "the retest came while the market had gapped: no trades until it trades normally again";
        s.outcomeTime = c.openTime;
        emit("skipped", s, events);
        return;
    }
    if (s.level3Confirmed && reached && !newExtreme) {
        s.retest = {c.openTime, c.close, n};
        // A proper 3 is a new high well beyond 1; a break that went nowhere
        // before price fell back is no 1-2-3.
        const double extension = bull ? s.level3.price - s.level1.price : s.level1.price - s.level3.price;
        // The entry is at the zone: a candle that only touched it and closed
        // far away leaves no entry there.
        const double away = bull ? c.close - s.zoneTop : s.zoneBottom - c.close;
        std::string why;
        if (extension < m_settings.extensionAtr * m_atr)
            why = std::string(bull ? "no proper 3: the high after the BOS is only " : "no proper 3: the low after the BOS is only ")
                + fixed(m_atr > 0 ? extension / m_atr : 0, 1) + " ATR beyond Level 1";
        else if (away > m_settings.entryZoneAtr * m_atr)
            why = "the test left the zone at once: the entry would be " + fixed(m_atr > 0 ? away / m_atr : 0, 1)
                + " ATR away from it";
        if (!why.empty()) {
            s.status = Status::Skipped;
            s.note = why;
            s.outcomeTime = c.openTime;
            emit("skipped", s, events);
            return;
        }
        s.confirmations = confirmationsFor(s, c);
        // The target: the area of 3. Its near edge is the body of the Level 3
        // candle; its far edge the end of the wick.
        s.target = s.level3.price;
        if (m_settings.targetBody && inWindow(s.level3.index)) {
            const Candle &three = at(s.level3.index);
            s.target = bull ? std::max(three.open, three.close) : std::min(three.open, three.close);
        }
        // Past the zone and Level 2 (a wick below the zone candle is part of the structure).
        const double buffer = m_settings.stopBufferAtr * m_atr;
        s.stop = bull ? std::min(s.zoneBottom, s.level2.price) - buffer : std::max(s.zoneTop, s.level2.price) + buffer;
        const double risk = bull ? c.close - s.stop : s.stop - c.close;
        s.rewardRisk = risk > 0 ? (bull ? s.target - c.close : c.close - s.target) / risk : 0;
        if (m_settings.requirePdArray && s.confirmations.empty()) {
            s.status = Status::Skipped;
            s.note = bull ? "no PD array (FVG, order block, demand) at or below Level 2 confirms the retest"
                          : "no PD array (FVG, order block, supply) at or above Level 2 confirms the retest";
            s.outcomeTime = c.openTime;
            emit("skipped", s, events);
            return;
        }
        s.status = Status::Triggered;
        emit("triggered", s, events);
        return;
    }
    // The first pullback from 3 ended above the zone: what follows is a new
    // leg and a new structure, not this one's test.
    if (m_settings.firstReturn && s.level3Confirmed && s.pullEnd.index >= 0 && s.bounce >= legNeed()
        && n - s.pullEnd.index >= m_settings.minPullbackCandles) {
        s.status = Status::Expired;
        s.note = "the pullback from 3 ended before the zone: a new leg began, so the zone was not tested in the first return";
        s.outcomeTime = c.openTime;
        emit("expired", s, events);
        return;
    }
    if (n - s.bos.index > m_settings.maxArmedCandles) {
        s.status = Status::Expired;
        s.note = "no retest within " + std::to_string(m_settings.maxArmedCandles) + " candles";
        s.outcomeTime = c.openTime;
        emit("expired", s, events);
    }
}

void Engine::enter(Setup &s, const Candle &e, std::vector<Event> *events)
{
    const bool bull = s.direction == Direction::Bullish;
    s.entry = e.open;
    s.entryTime = e.openTime;
    s.updated = e.openTime;
    s.intrabar = false;
    const double risk = bull ? s.entry - s.stop : s.stop - s.entry;
    const double reward = bull ? s.target - s.entry : s.entry - s.target;
    s.rewardRisk = risk > 0 && reward > 0 ? reward / risk : 0;
    std::string why;
    const Candle *before = m_window.empty() ? nullptr
        : (e.closed && m_window.size() >= 2 ? &m_window[m_window.size() - 2] : &m_window.back());
    if (m_gap.active || (before && opensWithGap(*before, e, m_atr)))
        why = "the entry candle opened with a gap (or the market had gapped): no trades until it trades normally again";
    else if (reward <= 0)
        why = "the entry candle opened at or beyond Level 3";
    else if (risk <= 0)
        why = "the entry candle opened beyond the stop";
    else if (s.rewardRisk + 1e-9 < m_settings.minRewardRisk)
        why = "reward/risk " + fixed(s.rewardRisk, 2) + " is below " + fixed(m_settings.minRewardRisk, 2);
    if (!why.empty()) {
        s.status = Status::Skipped;
        s.note = why;
        s.outcomeTime = e.openTime;
        emit("skipped", s, events);
        return;
    }
    s.status = Status::Entered;
    s.historical = !m_live;
    emit("entered", s, events);
    // A closed entry candle happened after the entry: its whole range counts.
    if (e.closed)
        outcome(s, e, true, events);
}

void Engine::outcome(Setup &s, const Candle &c, bool closed, std::vector<Event> *events)
{
    (void)closed;
    const bool bull = s.direction == Direction::Bullish;
    // Within one candle the order of high and low is unknown: the stop is
    // assumed to come first.
    const bool stop = bull ? c.low <= s.stop : c.high >= s.stop;
    const bool target = bull ? c.high >= s.target : c.low <= s.target;
    if (!stop && !target)
        return;
    s.status = stop ? Status::Stopped : Status::Target;
    s.outcomeTime = c.openTime;
    s.updated = c.openTime;
    emit(stop ? "stopped" : "target", s, events);
}

std::vector<PdArray> Engine::confirmationsFor(const Setup &s, const Candle &r) const
{
    std::vector<PdArray> found;
    const bool bull = s.direction == Direction::Bullish;
    const double reach = m_settings.pdDistanceAtr * m_atr;
    const double slack = 0.1 * m_atr;
    const int n = m_count - 1;
    for (const PdArray &a : m_arrays) {
        if (a.bullish != bull || a.formed >= n || a.broken)
            continue;
        if (a.time == s.zoneTime)
            continue; // the zone's own candle is not an additional confirmation
        if (a.kind == PdKind::FairValueGap && a.filled)
            continue;
        const bool overlap = a.top >= s.zoneBottom && a.bottom <= s.zoneTop;
        bool placed, reached;
        if (bull) {
            // At or below Level 2: its top no higher than the zone's, not too far under it.
            placed = a.top <= s.zoneTop + slack && a.top >= s.zoneBottom - reach;
            reached = r.low <= a.top || overlap;
            if (r.close < a.bottom)
                continue; // the retest closed through it
        } else {
            placed = a.bottom >= s.zoneBottom - slack && a.bottom <= s.zoneTop + reach;
            reached = r.high >= a.bottom || overlap;
            if (r.close > a.top)
                continue;
        }
        if (placed && reached)
            found.push_back(a);
    }
    // Nearest to the zone first; a few are enough.
    std::sort(found.begin(), found.end(), [&](const PdArray &x, const PdArray &y) {
        const double dx = bull ? s.zoneTop - x.top : x.bottom - s.zoneBottom;
        const double dy = bull ? s.zoneTop - y.top : y.bottom - s.zoneBottom;
        return std::fabs(dx) < std::fabs(dy);
    });
    if (found.size() > 4)
        found.resize(4);
    return found;
}

void Engine::detectArrays()
{
    if (m_atrCount < 5 || m_atr <= 0)
        return;
    const int n = m_count - 1;
    const Candle &c = at(n);
    const auto add = [&](PdArray a) {
        for (const PdArray &o : m_arrays) {
            if (o.kind == a.kind && o.bullish == a.bullish && o.index == a.index)
                return;
        }
        a.formed = n;
        m_arrays.push_back(a);
    };
    // Fair value gaps: candle n-1 left a gap between candles n-2 and n.
    if (n - 2 >= m_first) {
        const Candle &a = at(n - 2), &mid = at(n - 1);
        if (c.low > a.high && c.low - a.high >= m_settings.minGapAtr * m_atr)
            add({PdKind::FairValueGap, true, c.low, a.high, mid.openTime, n - 1});
        if (c.high < a.low && a.low - c.high >= m_settings.minGapAtr * m_atr)
            add({PdKind::FairValueGap, false, a.low, c.high, mid.openTime, n - 1});
    }
    // A displacement: a large body. Before it, an order block (the last
    // opposite candle) and a demand or supply zone (the small-bodied base).
    const bool up = bullishCandle(c) && body(c) >= m_settings.displacementAtr * m_atr;
    const bool down = bearishCandle(c) && body(c) >= m_settings.displacementAtr * m_atr;
    if (!up && !down)
        return;
    for (int j = n - 1; j >= n - 3 && j >= m_first; --j) {
        const Candle &x = at(j);
        if (up && bearishCandle(x) && c.close > x.high) {
            add({PdKind::OrderBlock, true, x.high, x.low, x.openTime, j});
            break;
        }
        if (down && bullishCandle(x) && c.close < x.low) {
            add({PdKind::OrderBlock, false, x.high, x.low, x.openTime, j});
            break;
        }
    }
    int base = 0;
    double low = 0, high = 0;
    long long time = 0;
    int first = n;
    for (int j = n - 1; j >= n - 3 && j >= m_first; --j) {
        const Candle &x = at(j);
        if (body(x) > 0.5 * m_atr)
            break;
        const double bodyHigh = std::max(x.open, x.close), bodyLow = std::min(x.open, x.close);
        if (base == 0) {
            low = up ? x.low : bodyLow;
            high = up ? bodyHigh : x.high;
        } else {
            low = std::min(low, up ? x.low : bodyLow);
            high = std::max(high, up ? bodyHigh : x.high);
        }
        ++base;
        time = x.openTime;
        first = j;
    }
    if (base > 0 && high > low)
        add({PdKind::Zone, up, high, low, time, first});
}

void Engine::updateArrays(const Candle &c)
{
    const int n = m_count - 1;
    for (PdArray &a : m_arrays) {
        if (a.formed >= n)
            continue;
        if (a.bullish) {
            a.broken = a.broken || c.close < a.bottom;
            a.touched = a.touched || c.low <= a.top;
            if (a.kind == PdKind::FairValueGap)
                a.filled = a.filled || c.low <= a.bottom;
        } else {
            a.broken = a.broken || c.close > a.top;
            a.touched = a.touched || c.high >= a.bottom;
            if (a.kind == PdKind::FairValueGap)
                a.filled = a.filled || c.high >= a.top;
        }
    }
}

void Engine::trim()
{
    const int n = m_count - 1;
    // PD arrays: the ones that can still matter.
    std::vector<PdArray> kept;
    for (const PdArray &a : m_arrays) {
        const bool dead = a.broken || (a.kind == PdKind::FairValueGap && a.filled);
        if (!(dead && n - a.formed > 50) && n - a.formed <= kMaxWindow)
            kept.push_back(a);
    }
    if (kept.size() > kMaxArrays)
        kept.erase(kept.begin(), kept.begin() + long(kept.size() - kMaxArrays));
    m_arrays = kept;
    // Setups: every open one, and the newest finished ones.
    size_t done = 0;
    for (const Setup &s : m_setups)
        done += finished(s.status) ? 1 : 0;
    const size_t keep = size_t(std::max(0, m_settings.keepFinished));
    if (done > keep) {
        size_t drop = done - keep;
        std::vector<Setup> next;
        for (const Setup &s : m_setups) {
            if (drop > 0 && finished(s.status)) {
                --drop;
                continue;
            }
            next.push_back(s);
        }
        m_setups = next;
    }
    // Candles: back to the oldest external point or leg still watched, and
    // the Level 3 candle of every open setup, within limits.
    int need = n - kMinWindow;
    for (const Point *p : {&m_pivotHigh, &m_pivotLow, &m_ext, &m_counter, &m_startHigh, &m_startLow}) {
        if (p->index >= 0)
            need = std::min(need, p->index - 3);
    }
    for (const Setup &s : m_setups) {
        if (!finished(s.status) && s.level3.index >= 0)
            need = std::min(need, s.level3.index);
    }
    need = std::max(need, n - kMaxWindow);
    if (need > m_first) {
        m_window.erase(m_window.begin(), m_window.begin() + (need - m_first));
        m_first = need;
    }
    // An external point older than the window stays the structure; only a
    // setup cannot be drawn from it.
}

} // namespace Trading::Sbt
