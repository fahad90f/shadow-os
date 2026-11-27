// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../skills/candle.h"

#include <string>
#include <vector>

// The trader's own strategy, "SBT Model 1", in one engine for both
// directions. Plain C++17, no Qt: every rule is unit-tested with only a
// compiler (tests/test_sbt.cpp).
//
// Bullish (bearish mirrors it):
//  1. A proper 1-2-3: Level 1 is a swing high, Level 2 the low after it.
//  2. A break of structure (BOS): a candle CLOSES above Level 1.
//  3. The lowest BEARISH candle between Level 1 and the BOS marks the Level 2
//     zone (its range: low to high, or low to open with zone = body).
//  4. Level 3 is the high made after the BOS; it is confirmed once
//     'swingStrength' candles have closed after it without going higher.
//  5. Wait for the market to come back to the zone: a closed candle whose
//     low reaches the zone (without making a new high: that candle also
//     confirms Level 3). If that candle (or any candle before it) CLOSES
//     below the zone, the setup is invalid.
//  6. PD arrays (fair value gaps, order blocks, demand zones; for a bearish
//     setup supply zones) at or below the zone, unbroken, and reached by the
//     move into the zone, confirm it (required by default). The candle that
//     marks the zone does not count as its own confirmation.
//  7. Entry at the OPEN of the next candle; target Level 3; the stop below
//     the zone and Level 2 by a fraction of the ATR. Entered only when
//     reward/risk meets the minimum.
//
// Only closed candles decide. The forming candle is used for two things: the
// open of the entry candle, and an "intrabar" note that price is in the zone
// (shown, never acted on). Nothing is ever recomputed from later candles: a
// setup that entered stays entered, with its levels as they were.
//
// Work per closed candle is constant (a few candles back, the setups and PD
// arrays still in play), never a rescan of the history.
namespace Trading::Sbt {

enum class Direction { Bullish, Bearish };
const char *directionName(Direction d); // "bullish" | "bearish"

enum class Status {
    Armed,       // structure and BOS confirmed; waiting for the retest
    Triggered,   // retest confirmed at a candle close; entry at the next open
    Entered,     // the signal: entry at that open
    Target,      // after entry, Level 3 was reached
    Stopped,     // after entry, the stop was reached
    Invalidated, // a structural condition failed (closed through the zone, …)
    Skipped,     // the rules were met but the trade was not taken (reward/risk, no PD array, gap past target)
    Expired,     // no retest within the allowed number of candles
};
const char *statusName(Status s);
bool finished(Status s); // no further change can come

enum class PdKind { FairValueGap, OrderBlock, Zone };
// "fvg" | "order_block" | "demand" | "supply"
const char *pdName(PdKind kind, bool bullish);

struct PdArray
{
    PdKind kind = PdKind::FairValueGap;
    bool bullish = true;
    double top = 0;
    double bottom = 0;
    long long time = 0;   // the candle it belongs to
    int index = 0;        // position in the engine's candle count (not the window)
    int formed = 0;       // the candle that completed it (its pattern is known from then on)
    bool broken = false;  // a candle closed beyond its far edge
    bool filled = false;  // a gap price traded fully through
    bool touched = false; // price came back into it after it formed
};

struct Point
{
    long long time = 0;
    double price = 0;
    int index = -1;
};

struct Setup
{
    std::string id;           // "<prefix>bullish:<time of Level 1>": one setup per structure
    Direction direction = Direction::Bullish;
    Point level1;
    Point level2;
    Point bos;                // the candle that closed beyond Level 1 (its close)
    Point level3;
    bool level3Confirmed = false;
    double zoneTop = 0;
    double zoneBottom = 0;
    long long zoneTime = 0;   // the candle that marks the zone
    Status status = Status::Armed;
    std::string note;         // why it was invalidated, skipped or expired
    Point retest;             // the candle that retested the zone (its close)
    bool intrabar = false;    // the forming candle is in the zone now (not confirmed)
    double entry = 0;         // the open of the entry candle
    long long entryTime = 0;
    double stop = 0;
    double target = 0;
    double rewardRisk = 0;    // at the entry (before it: estimated from the retest close)
    bool historical = false;  // entered while the history was replayed: never traded
    std::vector<PdArray> confirmations; // the PD arrays that confirmed it
    long long updated = 0;    // time of the candle that last changed it
    long long outcomeTime = 0;
};

struct Settings
{
    int swingStrength = 2;        // candles on each side of a swing point
    bool zoneBody = false;        // zone = low..open of the bearish candle instead of its whole range
    double stopBufferAtr = 0.25;  // the stop beyond the zone, in ATRs
    double minRewardRisk = 1.5;
    bool requirePdArray = true;
    double pdDistanceAtr = 1.5;   // how far beyond the zone a PD array may lie, in ATRs
    double displacementAtr = 1.2; // a candle body this large is a displacement
    double minGapAtr = 0.05;      // smaller gaps are ignored
    int maxArmedCandles = 200;    // a setup waits this long for its retest
    int keepFinished = 12;        // finished setups kept (the history view keeps many)
};

struct Event
{
    std::string kind;         // "armed" | "triggered" | "entered" | "target" | "stopped" | "invalidated" | "skipped" | "expired"
    Setup setup;              // as it is after the event
    bool live = false;        // false while the history is replayed
};

struct Swing
{
    Point point;
    bool high = false;
};

class Engine
{
public:
    explicit Engine(const Settings &settings = Settings());

    void reset();
    // While false, events are history: an entry made then is "historical".
    void setLive(bool live) { m_live = live; }
    // Put in front of every setup id ("BTCUSDT:15m:"), so ids are unique
    // across markets and the same setup read again has the same id.
    void setIdPrefix(const std::string &prefix) { m_prefix = prefix; }
    bool live() const { return m_live; }

    // The next closed candle, in order. A candle not newer than the last one
    // is ignored (returns nothing).
    std::vector<Event> close(const Candle &candle);
    // The forming candle (newer than the last closed one): the entry of a
    // triggered setup at its open, the intrabar note, targets and stops.
    std::vector<Event> forming(const Candle &candle);

    const std::vector<Setup> &setups() const { return m_setups; }   // oldest first
    const std::vector<PdArray> &pdArrays() const { return m_arrays; }
    const std::vector<Swing> &swings() const { return m_swings; }
    // The structure being watched for a BOS: the latest unbroken swing high
    // (bullish) or low (bearish); index -1 when there is none.
    Point candidate(Direction d) const { return d == Direction::Bullish ? m_bullCandidate : m_bearCandidate; }
    double atr() const { return m_atr; }
    int closedCount() const { return m_count; }
    long long lastTime() const { return m_window.empty() ? 0 : m_window.back().openTime; }
    const Settings &settings() const { return m_settings; }

private:
    const Candle &at(int index) const;   // by engine index; must be in the window
    bool inWindow(int index) const;
    void updateAtr(const Candle &c);
    void confirmSwing(std::vector<Event> *events);
    void detectArrays();
    void updateArrays(const Candle &c);
    void checkBos(const Candle &c, std::vector<Event> *events);
    void advance(Setup &s, const Candle &c, std::vector<Event> *events);
    void enter(Setup &s, const Candle &entryCandle, std::vector<Event> *events);
    void outcome(Setup &s, const Candle &c, bool closed, std::vector<Event> *events);
    std::vector<PdArray> confirmationsFor(const Setup &s, const Candle &retest) const;
    void emit(const std::string &kind, const Setup &s, std::vector<Event> *events) const;
    void trim();

    Settings m_settings;
    std::vector<Candle> m_window;   // the newest closed candles
    int m_first = 0;                // engine index of m_window[0]
    int m_count = 0;                // closed candles seen
    double m_atr = 0;
    int m_atrCount = 0;
    std::vector<Swing> m_swings;
    Point m_bullCandidate;          // swing high not yet broken
    Point m_bearCandidate;          // swing low not yet broken
    std::vector<Setup> m_setups;
    std::vector<PdArray> m_arrays;
    long long m_formingTime = 0;    // the forming candle last seen
    bool m_live = false;
    std::string m_prefix;
};

} // namespace Trading::Sbt
