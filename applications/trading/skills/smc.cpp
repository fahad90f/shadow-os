// SPDX-License-Identifier: GPL-3.0-or-later
#include "smc.h"

#include <algorithm>
#include <cstddef>

namespace Trading::Smc {

namespace {

constexpr int kMinimumCandles = 30;

// The candles to read: everything but a forming tail. Only the end of a
// series can still be forming, so dropping from the back keeps every index
// valid for the series that was passed in.
int closedCount(const Candles &candles)
{
    std::size_t count = candles.size();
    while (count > 0 && !candles[count - 1].closed)
        --count;
    return static_cast<int>(count);
}

// How far 'price' lies beyond 'level' in the direction that takes out a swing
// of that kind: above a swing high, below a swing low. Positive means beyond.
// It lets one piece of code read both sides of the market.
double beyond(double price, double level, bool high)
{
    return high ? price - level : level - price;
}

// Reads one candle against the swings of one kind that are known and
// unbroken ('open', positions in out.swings, oldest first).
void readCandle(const Candle &candle, int index, bool high, std::vector<std::size_t> &open, Analysis &out)
{
    if (!open.empty()) {
        const Swing &recent = out.swings[open.back()];
        if (beyond(candle.close, recent.price, high) > 0) {
            const Trend direction = high ? Trend::Bullish : Trend::Bearish;
            StructureEvent event;
            event.index = index;
            event.time = candle.openTime;
            event.bullish = high;
            event.changeOfCharacter = out.trend != Trend::Ranging && out.trend != direction;
            event.level = recent.price;
            event.swingIndex = recent.index;
            out.events.push_back(event);
            out.trend = direction;
        }
    }

    const double extreme = high ? candle.high : candle.low;
    // Newest first: the sweeps of one candle are listed from the most recent
    // swing back, and erasing does not disturb the positions still to visit.
    for (std::size_t i = open.size(); i-- > 0;) {
        const double level = out.swings[open[i]].price;
        const double closedBeyond = beyond(candle.close, level, high);
        if (closedBeyond > 0) {
            // A close beyond a level takes the swing out whether or not it
            // was the most recent one and gave the event: each swing can be
            // broken once, and an older one must not fire on a later candle
            // for a level that price had already closed through.
            open.erase(open.begin() + static_cast<std::ptrdiff_t>(i));
        } else if (closedBeyond < 0 && beyond(extreme, level, high) > 0) {
            Sweep sweep;
            sweep.index = index;
            sweep.time = candle.openTime;
            sweep.bullish = !high; // taking the lows is the bullish sweep
            sweep.level = level;
            out.sweeps.push_back(sweep);
        }
    }
}

void findStructure(const Candles &candles, int count, int strength, Analysis &out)
{
    std::vector<std::size_t> openHighs;
    std::vector<std::size_t> openLows;
    std::size_t next = 0;
    for (int i = 0; i < count; ++i) {
        // A swing is known once 'strength' closed candles follow it, so the
        // first candle that can act on it is the one after those.
        while (next < out.swings.size() && out.swings[next].index + strength < i) {
            (out.swings[next].high ? openHighs : openLows).push_back(next);
            ++next;
        }
        readCandle(candles[i], i, true, openHighs, out);
        readCandle(candles[i], i, false, openLows, out);
    }
}

void findOrderBlocks(const Candles &candles, int count, Analysis &out)
{
    for (const StructureEvent &event : out.events) {
        // Backwards from the candle before the break (the break is the move
        // itself, not what preceded it) down to the swing candle, which can
        // be the opposite candle the whole move started from.
        for (int i = event.index - 1; i >= event.swingIndex; --i) {
            const Candle &candle = candles[i];
            const bool opposite = event.bullish ? candle.close < candle.open : candle.close > candle.open;
            if (!opposite)
                continue;
            // Two breaks in one run lead back to the same candle; it is
            // still one zone.
            const bool listed = std::any_of(out.orderBlocks.begin(), out.orderBlocks.end(),
                                            [i](const OrderBlock &block) { return block.index == i; });
            if (!listed) {
                OrderBlock block;
                block.index = i;
                block.time = candle.openTime;
                block.bullish = event.bullish;
                block.top = candle.high;
                block.bottom = candle.low;
                // "Later" counts from the break: the candles between the
                // block and the break are price leaving the zone, and they
                // would mark nearly every block as mitigated at once.
                for (int j = event.index + 1; j < count && !block.mitigated; ++j)
                    block.mitigated = candles[j].low <= block.top && candles[j].high >= block.bottom;
                out.orderBlocks.push_back(block);
            }
            break;
        }
    }
}

std::vector<FairValueGap> findGaps(const Candles &candles, int count, double minGapPct)
{
    std::vector<FairValueGap> out;
    for (int i = 1; i + 1 < count; ++i) {
        const Candle &before = candles[i - 1];
        const Candle &after = candles[i + 1];
        const bool bullish = after.low > before.high;
        const bool bearish = after.high < before.low;
        if (!bullish && !bearish)
            continue;

        FairValueGap gap;
        gap.index = i;
        gap.time = candles[i].openTime;
        gap.bullish = bullish;
        gap.top = bullish ? after.low : before.low;
        gap.bottom = bullish ? before.high : after.high;
        if (gap.top - gap.bottom < candles[i].close * minGapPct / 100.0)
            continue;

        // Price leaves a bullish gap upwards and a bearish one downwards, so
        // it has traded through the whole gap once it reaches the far edge:
        // the bottom of a bullish gap, the top of a bearish one.
        for (int j = i + 2; j < count && !gap.filled; ++j)
            gap.filled = bullish ? candles[j].low <= gap.bottom : candles[j].high >= gap.top;
        out.push_back(gap);
    }
    return out;
}

void setRange(const Candles &candles, int count, Analysis &out)
{
    // Without a swing of a kind the whole series is the range on that side.
    double high = candles[0].high;
    double low = candles[0].low;
    for (int i = 1; i < count; ++i) {
        high = std::max(high, candles[i].high);
        low = std::min(low, candles[i].low);
    }
    bool haveHigh = false;
    bool haveLow = false;
    for (auto swing = out.swings.rbegin(); swing != out.swings.rend() && !(haveHigh && haveLow); ++swing) {
        if (swing->high && !haveHigh) {
            high = swing->price;
            haveHigh = true;
        } else if (!swing->high && !haveLow) {
            low = swing->price;
            haveLow = true;
        }
    }

    // In a rally that has left its last swing high behind, a newer swing low
    // can stand above that high. The range is then still the span between
    // the two levels; ordering them keeps "premium" meaning the upper part.
    out.rangeHigh = std::max(high, low);
    out.rangeLow = std::min(high, low);
    out.equilibrium = (out.rangeHigh + out.rangeLow) / 2.0;

    const double width = out.rangeHigh - out.rangeLow;
    // A range with no width has no upper or lower half: call it the middle.
    out.rangePosition = width > 0 ? (candles[count - 1].close - out.rangeLow) / width : 0.5;
    if (out.rangePosition > 0.55)
        out.zone = "premium";
    else if (out.rangePosition < 0.45)
        out.zone = "discount";
    else
        out.zone = "equilibrium";
}

} // namespace

std::vector<Swing> swings(const Candles &candles, int strength)
{
    std::vector<Swing> out;
    // With no neighbours to compare against every candle would be a swing,
    // which says nothing: a strength below 1 finds none.
    if (strength < 1)
        return out;

    const int count = closedCount(candles);
    for (int i = strength; i < count - strength; ++i) {
        const Candle &candle = candles[i];
        bool isHigh = true;
        bool isLow = true;
        for (int k = 1; k <= strength; ++k) {
            const Candle &before = candles[i - k];
            const Candle &after = candles[i + k];
            // Strict on the left, loose on the right: of several equal
            // highs (or lows) in a row only the first is the swing.
            isHigh = isHigh && before.high < candle.high && after.high <= candle.high;
            isLow = isLow && before.low > candle.low && after.low >= candle.low;
        }
        if (isHigh)
            out.push_back({i, candle.openTime, candle.high, true});
        if (isLow)
            out.push_back({i, candle.openTime, candle.low, false});
    }
    return out;
}

const char *trendName(Trend trend)
{
    switch (trend) {
    case Trend::Bullish:
        return "bullish";
    case Trend::Bearish:
        return "bearish";
    case Trend::Ranging:
        break;
    }
    return "ranging";
}

Analysis analyze(const Candles &candles, int swingStrength, double minGapPct)
{
    Analysis out;
    const int count = closedCount(candles);
    if (count < kMinimumCandles) {
        out.reason = "Need at least " + std::to_string(kMinimumCandles) + " closed candles, have "
            + std::to_string(count) + ".";
        return out;
    }
    out.valid = true;
    out.swings = swings(candles, swingStrength);
    findStructure(candles, count, swingStrength, out);
    findOrderBlocks(candles, count, out);
    out.gaps = findGaps(candles, count, minGapPct);
    setRange(candles, count, out);
    return out;
}

} // namespace Trading::Smc
