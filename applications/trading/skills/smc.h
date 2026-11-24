// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "candle.h"

#include <string>
#include <vector>

// The "SMC Patterns" skill: market-structure reading in the "smart money
// concepts" vocabulary: swing points, breaks of structure, order blocks,
// fair value gaps and liquidity sweeps. These are mechanical definitions
// applied to past candles; they describe what price did, they do not say what
// it will do. Only CLOSED candles are read (a forming last candle is ignored).
// Indexes refer to positions in the series that was passed in.
namespace Trading::Smc {

// A swing high is a candle whose high is strictly above the highs of the
// 'strength' candles before it and at or above the highs of the 'strength'
// candles after it; a swing low mirrors that with lows. A swing is only known
// once 'strength' closed candles follow it.
struct Swing
{
    int index = 0;
    long long time = 0;
    double price = 0;
    bool high = false;
};
std::vector<Swing> swings(const Candles &candles, int strength = 3);

enum class Trend { Ranging, Bullish, Bearish };
const char *trendName(Trend trend); // "ranging" | "bullish" | "bearish"

// A close beyond the most recent unbroken swing: above a swing high (bullish)
// or below a swing low (bearish). It is a "BOS" (break of structure) when it
// continues the trend in force at that moment, or when no trend was set yet,
// and a "CHoCH" (change of character) when it goes against it. Each swing can
// be broken once. After an event the trend is the direction of the break.
struct StructureEvent
{
    int index = 0;        // the candle that closed beyond the level
    long long time = 0;
    bool bullish = false;
    bool changeOfCharacter = false; // false: BOS
    double level = 0;     // the swing price that was broken
    int swingIndex = 0;   // where that swing was
};

// The last opposite-coloured candle before the move that produced a structure
// event: for a bullish event the last bearish candle (close < open) between
// the broken swing and the breaking candle, for a bearish event the last
// bullish candle. The zone is that candle's full range (low..high).
// 'mitigated' is true once a later closed candle traded back into the zone.
struct OrderBlock
{
    int index = 0;
    long long time = 0;
    bool bullish = false;
    double top = 0;
    double bottom = 0;
    bool mitigated = false;
};

// A three-candle imbalance. Bullish: the low of candle i+1 is above the high
// of candle i-1 (the gap is between them, created by candle i). Bearish: the
// high of candle i+1 is below the low of candle i-1. 'index' is i. Gaps
// smaller than minGapPct of the middle candle's close are ignored. 'filled' is
// true once a later closed candle traded through the whole gap.
struct FairValueGap
{
    int index = 0;
    long long time = 0;
    bool bullish = false;
    double top = 0;
    double bottom = 0;
    bool filled = false;
};

// A candle that traded beyond a prior unbroken swing but closed back inside:
// its low went below a swing low and it closed above it (bullish: sell-side
// liquidity taken), or its high went above a swing high and it closed below
// it (bearish). The swing stays unbroken.
struct Sweep
{
    int index = 0;
    long long time = 0;
    bool bullish = false;
    double level = 0;
};

struct Analysis
{
    bool valid = false;   // false with fewer than 30 closed candles
    std::string reason;
    Trend trend = Trend::Ranging;
    std::vector<Swing> swings;
    std::vector<StructureEvent> events;
    std::vector<OrderBlock> orderBlocks;
    std::vector<FairValueGap> gaps;
    std::vector<Sweep> sweeps;
    // The dealing range: the most recent swing high and swing low. With no
    // swing of a kind yet, the highest high / lowest low of the series.
    double rangeHigh = 0;
    double rangeLow = 0;
    double equilibrium = 0; // midpoint of the range
    // Where the last close sits in the range: "premium" (upper half, above
    // 55 %), "discount" (below 45 %), otherwise "equilibrium".
    std::string zone;
    double rangePosition = 0; // 0 = at rangeLow, 1 = at rangeHigh; may leave 0..1
};

Analysis analyze(const Candles &candles, int swingStrength = 3, double minGapPct = 0.05);

} // namespace Trading::Smc
