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
//  1. A proper 1-2-3 of the EXTERNAL structure: Level 1 is the external high
//     (the top of the last leg, confirmed by a real pullback). The valid
//     swing low, Level 2, is the HIGHER LOW (HL): the low of the pullback
//     after Level 1, above the swing low before it (the external low, or the
//     low of the leg down still running). Bearish: the valid swing high is
//     the LOWER HIGH (LH), below the swing high before it. A low that is not
//     a higher low (a high that is not a lower high) is no valid swing: no
//     setup. Swings inside a leg (internal structure) are never Level 1, 2
//     or 3.
//  2. A break of structure (BOS): a candle CLOSES above Level 1.
//  3. The lowest BEARISH candle at the higher low (between Level 1 and the
//     BOS) marks the Level 2 zone, measured from its OPEN to its lowest wick
//     (bearish: the highest bullish candle, from its open to its highest
//     wick). That same candle must be both the ORDER BLOCK and the DEMAND
//     zone (bearish: supply zone), or the setup is skipped:
//       order block: the next candle moves the other way (bullish) and,
//         within three candles, one closes beyond the zone candle's far end
//         (above its high): it is the last bearish candle before the move;
//       demand: the move away is strong (the best close of those three
//         candles at least 'zoneMoveAtr' ATRs beyond its open) and the zone
//         stays fresh: no candle comes back into it before the BOS.
//  4. Level 3 is the high of the leg after the BOS; it is confirmed by a
//     valid pullback (a candle closing below the low of the candle that made
//     it) without a new high first.
//  5. Wait for the market to come back to the zone: a closed candle whose
//     low reaches the zone (without making a new high: that candle also
//     confirms Level 3). If that candle (or any candle before it) CLOSES
//     below the zone, the setup is invalid.
//  6. PD arrays (fair value gaps, order blocks, demand zones; for a bearish
//     setup supply zones) at or below the zone, unbroken, and reached by the
//     move into the zone, are listed with it. They are not part of the
//     original rules: only with 'requirePdArray' (off by default) must one
//     confirm the retest. The candle that marks the zone never counts as its
//     own confirmation.
//  7. Entry at the OPEN of the next candle; target Level 3; the stop below
//     the zone and Level 2 by a fraction of the ATR. Entered only when
//     reward/risk meets the minimum.
//
// Gaps (rule 5): a candle that opens beyond the previous candle's range, and
// at least 'gapAtr' ATRs from its close, is a gap up or down. From that
// candle on nothing is set up or entered (a BOS then is skipped, a retest
// then is skipped, an entry candle that opens with a gap is skipped) until
// the gap is filled (price trades back to the close before it) or
// 'gapPauseCandles' more candles have closed. Open trades keep their stops
// and targets.
//
// Only closed candles decide. The forming candle is used for two things: the
// open of the entry candle, and an "intrabar" note that price is in the zone
// (shown, never acted on). Nothing is ever recomputed from later candles: a
// setup that entered stays entered, with its levels as they were.
//
// External structure. After a bullish BOS, Level 2 becomes the external
// (protected) low and the leg up is followed; its top becomes the external
// high only when a candle closes below the low of the candle that made it (a
// valid pullback). Small swings inside the leg or inside the pullback do not
// move either point. A close above the external high is the next bullish BOS
// (Level 1 = that high); a close below the external low is a bearish one
// (Level 1 = that low, Level 2 = the highest point between). The structure
// starts from the first candle's range; the first breaks and valid
// pullbacks put the external points in place.
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
    Point previousSwing;      // the swing low before Level 2: Level 2 is the higher low above it (bearish: lower high below it)
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
    bool orderBlock = false;  // the zone candle is the order block (rule 3/4)
    bool demand = false;      // ... and the demand (bearish: supply) zone
    long long updated = 0;    // time of the candle that last changed it
    long long outcomeTime = 0;
};

struct Settings
{
    int swingStrength = 2;        // candles on each side of a recorded (internal) swing point: context only
    double zoneMoveAtr = 1.0;     // demand/supply: how far the move away from the zone goes, in ATRs
    double gapAtr = 0.5;          // a gap: the open this far from the previous close (and outside its range)
    int gapPauseCandles = 6;      // after a gap, candles without trading unless it is filled sooner
    double stopBufferAtr = 0.25;  // the stop beyond the zone, in ATRs
    double minRewardRisk = 1.5;
    bool requirePdArray = false;  // an extra filter; the original 7 rules have no PD arrays
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

// The market opened with a gap: no setups or entries until it is filled or
// the pause has run out.
struct Gap
{
    bool active = false;
    bool up = false;          // gap up (bearish: down)
    double fill = 0;          // the close before the gap: trading back to it fills it
    long long time = 0;       // the candle that opened with the gap
    int index = -1;
    int left = 0;             // candles of the pause still to come
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
    // The structure being watched for a BOS: the external high (bullish) or
    // low (bearish); index -1 while a leg is still running (no pullback yet).
    Point candidate(Direction d) const { return d == Direction::Bullish ? m_extHigh : m_extLow; }
    // The leg after the last break, until a valid pullback confirms its end
    // as the new external point; index -1 when there is none.
    Point leg(Direction d) const { return d == Direction::Bullish ? m_legHigh : m_legLow; }
    double atr() const { return m_atr; }
    // The gap pause; active while nothing may be set up or entered.
    const Gap &gap() const { return m_gap; }
    // Whether 'c' opens with a gap after 'prev' (by the settings, at 'atr').
    bool opensWithGap(const Candle &prev, const Candle &c, double atr) const;
    int closedCount() const { return m_count; }
    long long lastTime() const { return m_window.empty() ? 0 : m_window.back().openTime; }
    const Settings &settings() const { return m_settings; }

private:
    const Candle &at(int index) const;   // by engine index; must be in the window
    bool inWindow(int index) const;
    void updateAtr(const Candle &c);
    void confirmSwing(std::vector<Event> *events);
    void updateLegs(const Candle &c);
    void detectArrays();
    void updateArrays(const Candle &c);
    void checkBos(const Candle &c, std::vector<Event> *events);
    void advance(Setup &s, const Candle &c, std::vector<Event> *events);
    void enter(Setup &s, const Candle &entryCandle, std::vector<Event> *events);
    void outcome(Setup &s, const Candle &c, bool closed, std::vector<Event> *events);
    std::vector<PdArray> confirmationsFor(const Setup &s, const Candle &retest) const;
    // Rules 3/4: "" when the zone candle 'z' is both the order block and the
    // demand (supply) zone of the move to the BOS at 'n', else why not.
    std::string zoneCheck(Setup &s, int z, int n) const;
    void emit(const std::string &kind, const Setup &s, std::vector<Event> *events) const;
    void trim();

    Settings m_settings;
    std::vector<Candle> m_window;   // the newest closed candles
    int m_first = 0;                // engine index of m_window[0]
    int m_count = 0;                // closed candles seen
    double m_atr = 0;
    int m_atrCount = 0;
    std::vector<Swing> m_swings;
    Point m_extHigh;                // external high not yet broken
    Point m_extLow;                 // external low not yet broken
    Point m_legHigh;                // the leg up after a bullish break, until its pullback
    Point m_legLow;                 // the leg down after a bearish break
    // False for the first candle's range the structure starts from: a break
    // of it starts the structure but is not a setup.
    bool m_extHighReal = false;
    bool m_extLowReal = false;
    std::vector<Setup> m_setups;
    std::vector<PdArray> m_arrays;
    long long m_formingTime = 0;    // the forming candle last seen
    Gap m_gap;
    bool m_live = false;
    std::string m_prefix;
};

} // namespace Trading::Sbt
