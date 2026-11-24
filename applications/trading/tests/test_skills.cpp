// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tests of the analysis skills (TA indicators, SMC patterns). The expected
// values are worked out by hand in the comments; the few that are too long
// for that ("reference") come from a separate exact-fraction calculation of
// the definitions in the headers, never from the functions under test.
// No framework; from applications/trading:
//
//   g++ -std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Wshadow -Werror -fsanitize=address,undefined
//       -I skills skills/indicators.cpp skills/smc.cpp tests/test_skills.cpp -o /tmp/test_skills
//   /tmp/test_skills
#include "../skills/indicators.h"
#include "../skills/smc.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using namespace Trading;
using Values = std::vector<double>;

namespace {

const double kNaN = std::numeric_limits<double>::quiet_NaN();
const double kTolerance = 1e-9;

int g_checks = 0;
int g_failed = 0;

// Counts one check and prints a failed one with the place and both values.
// The values go through a stream, so one function serves numbers, strings
// and bools.
template <typename Actual, typename Expected>
void report(bool pass, const char *file, int line, const std::string &what, const Actual &actual,
            const Expected &expected)
{
    ++g_checks;
    if (pass)
        return;
    ++g_failed;
    std::cout << "FAIL " << file << ":" << line << ": " << what << " is " << actual << ", expected " << expected
              << "\n";
}

#define CHECK(condition) report(static_cast<bool>(condition), __FILE__, __LINE__, #condition, false, true)

#define EQUAL(actual, expected) \
    do { \
        const auto &equal_a = (actual); \
        const auto &equal_b = (expected); \
        report(equal_a == equal_b, __FILE__, __LINE__, #actual, equal_a, equal_b); \
    } while (0)

#define NEAR(actual, expected) \
    do { \
        const double near_a = (actual); \
        const double near_b = (expected); \
        report(std::fabs(near_a - near_b) <= kTolerance, __FILE__, __LINE__, #actual, near_a, near_b); \
    } while (0)

#define IS_NAN(value) report(std::isnan(value), __FILE__, __LINE__, #value, value, "NaN")

// A whole series against what it should be, position by position; NaN in the
// expected values means "no number here yet".
#define SERIES(actual, ...) checkSeries(actual, Values{__VA_ARGS__}, __FILE__, __LINE__, #actual)

// Leaves the test function when a list has the wrong length, so the checks
// after it can index the list without reading past its end.
#define SIZE_OR_RETURN(list, expected) \
    do { \
        EQUAL((list).size(), expected); \
        if ((list).size() != (expected)) \
            return; \
    } while (0)

void checkSeries(const Values &actual, const Values &expected, const char *file, int line, const std::string &what)
{
    report(actual.size() == expected.size(), file, line, what + ".size()", actual.size(), expected.size());
    for (std::size_t i = 0; i < std::min(actual.size(), expected.size()); ++i) {
        const bool pass = std::isnan(expected[i]) ? std::isnan(actual[i])
                                                  : std::fabs(actual[i] - expected[i]) <= kTolerance;
        report(pass, file, line, what + "[" + std::to_string(i) + "]", actual[i], expected[i]);
    }
}

// How many positions at the start hold NaN: the index of the first number.
std::size_t leadingNaNs(const Values &series)
{
    std::size_t count = 0;
    while (count < series.size() && std::isnan(series[count]))
        ++count;
    return count;
}

bool finiteFrom(const Values &series, std::size_t first)
{
    return std::all_of(series.begin() + static_cast<std::ptrdiff_t>(std::min(first, series.size())), series.end(),
                       [](double value) { return std::isfinite(value); });
}

bool allNaN(const Values &series)
{
    return leadingNaNs(series) == series.size();
}

long long timeOf(int index)
{
    return index * 60000LL;
}

Candle bar(int index, double open, double high, double low, double close, double volume = 10)
{
    Candle candle;
    candle.openTime = timeOf(index);
    candle.open = open;
    candle.high = high;
    candle.low = low;
    candle.close = close;
    candle.volume = volume;
    return candle;
}

Candle forming(Candle candle)
{
    candle.closed = false;
    return candle;
}

// 'count' identical candles around one price, half a point of wick each way.
Candles flat(int count, double price, double volume)
{
    Candles candles;
    for (int i = 0; i < count; ++i)
        candles.push_back(bar(i, price, price + 0.5, price - 0.5, price, volume));
    return candles;
}

// A straight line: every candle closes one 'step' beyond its open and has
// half a point of wick each way, so every true range is |step| + 1.
Candles ramp(int count, double start, double step)
{
    Candles candles;
    for (int i = 0; i < count; ++i) {
        const double close = start + step * i;
        const double open = close - step;
        candles.push_back(bar(i, open, std::max(open, close) + 0.5, std::min(open, close) - 0.5, close));
    }
    return candles;
}

// Candles for a list of closes: each opens at the previous close (the first
// at its own) and has half a point of wick each way.
Candles fromCloses(const Values &closes)
{
    Candles candles;
    for (std::size_t i = 0; i < closes.size(); ++i) {
        const double open = i == 0 ? closes[i] : closes[i - 1];
        candles.push_back(bar(static_cast<int>(i), open, std::max(open, closes[i]) + 0.5,
                              std::min(open, closes[i]) - 0.5, closes[i]));
    }
    return candles;
}

// A series with no pattern to it, all in small whole numbers so the
// reference calculation is exact: closes between 89 and 111, uneven wicks,
// volumes between 100 and 149.
Candles irregular(int count)
{
    Candles candles;
    for (int i = 0; i < count; ++i) {
        const double close = 100 + (7 * i * i + 3 * i) % 23 - 11;
        const double open = i == 0 ? close : candles.back().close;
        candles.push_back(bar(i, open, std::max(open, close) + 1 + i % 3, std::min(open, close) - 1 - i % 2, close,
                              100 + i * 37 % 50));
    }
    return candles;
}

// A quiet market for the structure tests: identical dojis, open = close =
// 100, high 101, low 99. Equal highs and lows never make a swing (the rule is
// strict on the left), a doji is neither bullish nor bearish and nothing
// gaps, so whatever an analysis finds comes from the candles a test sets.
Candles quiet(int count)
{
    Candles candles;
    for (int i = 0; i < count; ++i)
        candles.push_back(bar(i, 100, 101, 99, 100));
    return candles;
}

void set(Candles &candles, int index, double open, double high, double low, double close)
{
    candles[static_cast<std::size_t>(index)] = bar(index, open, high, low, close);
}

// Candles that differ in their highs only (or lows only): the other side is
// level, so it makes no swings of its own.
Candles withHighs(const Values &highs)
{
    Candles candles;
    for (std::size_t i = 0; i < highs.size(); ++i)
        candles.push_back(bar(static_cast<int>(i), 1, highs[i], 0, 1));
    return candles;
}

Candles withLows(const Values &lows)
{
    Candles candles;
    for (std::size_t i = 0; i < lows.size(); ++i)
        candles.push_back(bar(static_cast<int>(i), 99, 100, lows[i], 99));
    return candles;
}

std::string name(Smc::Trend trend)
{
    return Smc::trendName(trend);
}

#define SWING(swing, at, level, isHigh) \
    do { \
        EQUAL((swing).index, at); \
        EQUAL((swing).time, timeOf(at)); \
        NEAR((swing).price, level); \
        EQUAL((swing).high, isHigh); \
    } while (0)

#define EVENT(event, at, isBullish, isChange, price, swingAt) \
    do { \
        EQUAL((event).index, at); \
        EQUAL((event).time, timeOf(at)); \
        EQUAL((event).bullish, isBullish); \
        EQUAL((event).changeOfCharacter, isChange); \
        NEAR((event).level, price); \
        EQUAL((event).swingIndex, swingAt); \
    } while (0)

// Order blocks and fair value gaps have the same shape.
#define ZONE(zone, at, isBullish, upper, lower) \
    do { \
        EQUAL((zone).index, at); \
        EQUAL((zone).time, timeOf(at)); \
        EQUAL((zone).bullish, isBullish); \
        NEAR((zone).top, upper); \
        NEAR((zone).bottom, lower); \
    } while (0)

#define SWEEP(sweep, at, isBullish, price) \
    do { \
        EQUAL((sweep).index, at); \
        EQUAL((sweep).time, timeOf(at)); \
        EQUAL((sweep).bullish, isBullish); \
        NEAR((sweep).level, price); \
    } while (0)

// ---------------------------------------------------------------- indicators

void testSma()
{
    // Windows of three: (1 + 2 + 3) / 3 = 2, then 3, 4, 5.
    SERIES(Indicators::sma({1, 2, 3, 4, 5, 6}, 3), kNaN, kNaN, 2, 3, 4, 5);
    // Pairs of an uneven series: (2 + 4) / 2, (4 + 9) / 2, (9 + 1) / 2, (1 + 6) / 2.
    SERIES(Indicators::sma({2, 4, 9, 1, 6}, 2), kNaN, 3, 6.5, 5, 3.5);
    // A period of one is the series itself; the whole length leaves one value.
    SERIES(Indicators::sma({2, 4, 9}, 1), 2, 4, 9);
    SERIES(Indicators::sma({2, 4, 9}, 3), kNaN, kNaN, 5);
    // Longer than the series, or no period at all: same length, no numbers.
    SERIES(Indicators::sma({2, 4, 9}, 4), kNaN, kNaN, kNaN);
    SERIES(Indicators::sma({2, 4, 9}, 0), kNaN, kNaN, kNaN);
    SERIES(Indicators::sma({2, 4, 9}, -1), kNaN, kNaN, kNaN);
    CHECK(Indicators::sma({}, 3).empty());
}

void testEma()
{
    // Period 3 smooths by 2 / (3 + 1) = 0.5: seed (2 + 4 + 6) / 3 = 4 at
    // index 2, then halfway to each new value: 6, 9, 11.5, 13.75.
    SERIES(Indicators::ema({2, 4, 6, 8, 12, 14, 16}, 3), kNaN, kNaN, 4, 6, 9, 11.5, 13.75);
    // Period 4 smooths by 2 / 5 = 0.4: seed (10 + 11 + 12 + 13) / 4 = 11.5,
    // 11.5 + 0.4 * (15 - 11.5) = 12.9, 12.9 + 0.4 * (10 - 12.9) = 11.74.
    SERIES(Indicators::ema({10, 11, 12, 13, 15, 10}, 4), kNaN, kNaN, kNaN, 11.5, 12.9, 11.74);
    // Period 1 smooths by 1, which is the series itself; the whole length
    // leaves only the seed.
    SERIES(Indicators::ema({2, 4, 9}, 1), 2, 4, 9);
    SERIES(Indicators::ema({2, 4, 9}, 3), kNaN, kNaN, 5);
    SERIES(Indicators::ema({2, 4, 9}, 4), kNaN, kNaN, kNaN);
    SERIES(Indicators::ema({2, 4, 9}, 0), kNaN, kNaN, kNaN);
    SERIES(Indicators::ema({2, 4, 9}, -1), kNaN, kNaN, kNaN);
    CHECK(Indicators::ema({}, 3).empty());

    // On a straight line of slope 1 the average trails the line by
    // (period - 1) / 2: the seed (the mean of the first 'period' points) is
    // that far behind, and a lag d becomes (1 - k) * (d + 1) on the next
    // point, which for k = 2 / (period + 1) is d again. The summary tests
    // lean on this.
    Values line;
    for (int i = 0; i < 60; ++i)
        line.push_back(100 + i);
    const Values trailing = Indicators::ema(line, 20);
    EQUAL(trailing.size(), 60u);
    EQUAL(leadingNaNs(trailing), 19u);
    NEAR(trailing[19], 119 - 9.5);
    NEAR(trailing[40], 140 - 9.5);
    NEAR(trailing[59], 159 - 9.5);
}

void testRsi()
{
    // Wilder's worked example, closes rounded to cents.
    const Values wilder = {44.34, 44.09, 44.15, 43.61, 44.33, 44.83, 45.10, 45.42, 45.84, 46.08,
                           45.89, 46.03, 45.61, 46.28, 46.28, 46.00, 46.03, 46.41, 46.22, 45.64};
    const Values classic = Indicators::rsi(wilder); // the default period is 14
    EQUAL(classic.size(), 20u);
    EQUAL(leadingNaNs(classic), 14u);
    // The first 14 changes hold gains of 0.06 + 0.72 + 0.50 + 0.27 + 0.32 +
    // 0.42 + 0.24 + 0.14 + 0.67 = 3.34 and losses of 0.25 + 0.54 + 0.19 +
    // 0.42 = 1.40; the common factor 1 / 14 cancels in the ratio.
    NEAR(classic[14], 100 - 100 / (1 + 3.34 / 1.40));
    EQUAL(std::round(classic[14] * 100), 7046.0);
    // The next change is a loss of 0.28: both averages keep 13 / 14 of
    // themselves and the loss side gains 0.28 / 14.
    NEAR(classic[15], 100 - 100 / (1 + (3.34 * 13 / 14) / (1.40 * 13 / 14 + 0.28)));
    // The rest from the reference.
    NEAR(classic[15], 66.24961855355508);
    NEAR(classic[16], 66.48094183471267);
    NEAR(classic[17], 69.3468531629087);
    NEAR(classic[18], 66.29471265892626);
    NEAR(classic[19], 57.91502067008556);

    // Period 3 on changes +1, -1, +2, -1, +2. Seed: gains (1 + 0 + 2) / 3 = 1,
    // losses 1/3, ratio 3, so 100 - 100 / 4 = 75. Then gains 2/3, losses
    // (2/3 + 1) / 3 = 5/9, ratio 6/5: 100 - 100 / 2.2 = 600/11. Then gains
    // (4/3 + 2) / 3 = 10/9, losses 10/27, ratio 3 again: 75.
    SERIES(Indicators::rsi({10, 11, 10, 12, 11, 13}, 3), kNaN, kNaN, kNaN, 75, 600.0 / 11, 75);

    // The readings the header gives to the windows without a ratio.
    SERIES(Indicators::rsi({1, 2, 3, 4, 5, 6}, 3), kNaN, kNaN, kNaN, 100, 100, 100);
    SERIES(Indicators::rsi({6, 5, 4, 3, 2, 1}, 3), kNaN, kNaN, kNaN, 0, 0, 0);
    SERIES(Indicators::rsi({7, 7, 7, 7, 7, 7}, 3), kNaN, kNaN, kNaN, 50, 50, 50);
    // Gains only, then a loss of 1: gains (2 + 0) / 3 = 2/3 against losses
    // 1/3, ratio 2, so 100 - 100 / 3.
    SERIES(Indicators::rsi({1, 2, 3, 4, 3}, 3), kNaN, kNaN, kNaN, 100, 100 - 100.0 / 3);

    // 'period' changes need period + 1 closes.
    SERIES(Indicators::rsi({1, 2, 3}, 3), kNaN, kNaN, kNaN);
    SERIES(Indicators::rsi({1, 2, 3, 4}, 3), kNaN, kNaN, kNaN, 100);
    SERIES(Indicators::rsi({1, 2, 3}, 0), kNaN, kNaN, kNaN);
    SERIES(Indicators::rsi({1, 2, 3}, -1), kNaN, kNaN, kNaN);
    CHECK(Indicators::rsi({}, 3).empty());
}

void testMacd()
{
    // Fast 2 (smoothing 2/3), slow 3 (1/2), signal 2 (2/3):
    //   closes  2  4  6  8  12    14      16
    //   fast    -  3  5  7  31/3  115/9   403/27
    //   slow    -  -  4  6  9     23/2    55/4
    //   macd    -  -  1  1  4/3   23/18   127/108
    //   signal  -  -  -  1  11/9  34/27   65/54    seed (1 + 1) / 2 at index 3
    //   hist    -  -  -  0  1/9   1/54    -1/36
    const Values closes = {2, 4, 6, 8, 12, 14, 16};
    const Indicators::Macd small = Indicators::macd(closes, 2, 3, 2);
    SERIES(small.macd, kNaN, kNaN, 1, 1, 4.0 / 3, 23.0 / 18, 127.0 / 108);
    SERIES(small.signal, kNaN, kNaN, kNaN, 1, 11.0 / 9, 34.0 / 27, 65.0 / 54);
    SERIES(small.histogram, kNaN, kNaN, kNaN, 0, 1.0 / 9, 1.0 / 54, -1.0 / 36);

    // With the periods the other way round the line is the same with the
    // sign flipped, and still starts where the slower average does.
    const Indicators::Macd swapped = Indicators::macd(closes, 3, 2, 2);
    SERIES(swapped.macd, kNaN, kNaN, -1, -1, -4.0 / 3, -23.0 / 18, -127.0 / 108);
    SERIES(swapped.signal, kNaN, kNaN, kNaN, -1, -11.0 / 9, -34.0 / 27, -65.0 / 54);

    // The defaults, 12 / 26 / 9: the line starts at index 25, the signal
    // line eight later. Values from the reference.
    const Values prices = Indicators::closes(irregular(40));
    const Indicators::Macd lines = Indicators::macd(prices);
    EQUAL(lines.macd.size(), 40u);
    EQUAL(lines.signal.size(), 40u);
    EQUAL(lines.histogram.size(), 40u);
    EQUAL(leadingNaNs(lines.macd), 25u);
    EQUAL(leadingNaNs(lines.signal), 33u);
    EQUAL(leadingNaNs(lines.histogram), 33u);
    CHECK(finiteFrom(lines.macd, 25) && finiteFrom(lines.signal, 33) && finiteFrom(lines.histogram, 33));
    NEAR(lines.macd[25], -0.5378277282060353);
    NEAR(lines.macd[26], -0.9858761469654838);
    NEAR(lines.macd[33], 1.5343124583755585);
    NEAR(lines.signal[33], 0.05948027488064685);
    NEAR(lines.histogram[33], 1.4748321834949116);
    NEAR(lines.signal[34], 0.22052064321877735);
    NEAR(lines.macd[39], -0.8961300448116614);
    NEAR(lines.signal[39], -0.09781222001450207);
    NEAR(lines.histogram[39], -0.7983178247971593);

    // One close short of the slow average: nothing. Exactly enough: the line
    // has its first value and the signal line none.
    const Indicators::Macd shortOne = Indicators::macd(Values(prices.begin(), prices.begin() + 25));
    CHECK(allNaN(shortOne.macd) && allNaN(shortOne.signal) && allNaN(shortOne.histogram));
    const Indicators::Macd justEnough = Indicators::macd(Values(prices.begin(), prices.begin() + 26));
    EQUAL(leadingNaNs(justEnough.macd), 25u);
    NEAR(justEnough.macd[25], -0.5378277282060353);
    CHECK(allNaN(justEnough.signal) && allNaN(justEnough.histogram));

    const Indicators::Macd noFast = Indicators::macd(closes, 0, 3, 2);
    const Indicators::Macd noSlow = Indicators::macd(closes, 2, -1, 2);
    const Indicators::Macd noSignal = Indicators::macd(closes, 2, 3, 0);
    for (const Indicators::Macd *none : {&noFast, &noSlow, &noSignal}) {
        EQUAL(none->macd.size(), 7u);
        EQUAL(none->signal.size(), 7u);
        EQUAL(none->histogram.size(), 7u);
        CHECK(allNaN(none->macd) && allNaN(none->signal) && allNaN(none->histogram));
    }
    const Indicators::Macd empty = Indicators::macd({});
    CHECK(empty.macd.empty() && empty.signal.empty() && empty.histogram.empty());
}

void testBollinger()
{
    // The textbook set: mean 5, squared distances 9 + 1 + 1 + 1 + 0 + 0 + 4 +
    // 16 = 32, and 32 / 8 = 4 gives a deviation of exactly 2. Dividing by 7,
    // as for a sample, would give 2.138 and bands at 9.28 and 0.72.
    const Values textbook = {2, 4, 4, 4, 5, 5, 7, 9};
    const Indicators::Bands whole = Indicators::bollinger(textbook, 8, 2.0);
    SERIES(whole.middle, kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, 5);
    SERIES(whole.upper, kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, 9);
    SERIES(whole.lower, kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, 1);
    const Indicators::Bands narrower = Indicators::bollinger(textbook, 8, 1.5);
    NEAR(narrower.middle[7], 5);
    NEAR(narrower.upper[7], 8);
    NEAR(narrower.lower[7], 2);

    // Windows of four, mean and mean squared distance of each:
    //   2 4 4 4   3.5    (2.25 + 3 * 0.25) / 4 = 0.75
    //   4 4 4 5   4.25   (3 * 0.0625 + 0.5625) / 4 = 0.1875
    //   4 4 5 5   4.5    0.25
    //   4 5 5 7   5.25   (1.5625 + 2 * 0.0625 + 3.0625) / 4 = 1.1875
    //   5 5 7 9   6.5    (2 * 2.25 + 0.25 + 6.25) / 4 = 2.75
    const Indicators::Bands four = Indicators::bollinger(textbook, 4, 2.0);
    SERIES(four.middle, kNaN, kNaN, kNaN, 3.5, 4.25, 4.5, 5.25, 6.5);
    SERIES(four.upper, kNaN, kNaN, kNaN, 3.5 + 2 * std::sqrt(0.75), 4.25 + 2 * std::sqrt(0.1875), 5.5,
           5.25 + 2 * std::sqrt(1.1875), 6.5 + 2 * std::sqrt(2.75));
    SERIES(four.lower, kNaN, kNaN, kNaN, 3.5 - 2 * std::sqrt(0.75), 4.25 - 2 * std::sqrt(0.1875), 3.5,
           5.25 - 2 * std::sqrt(1.1875), 6.5 - 2 * std::sqrt(2.75));

    // The defaults, 20 and 2. Reference: the first twenty irregular closes
    // have mean 494/5 and mean squared distance 1953/50.
    const Indicators::Bands bands = Indicators::bollinger(Indicators::closes(irregular(40)));
    EQUAL(bands.middle.size(), 40u);
    EQUAL(leadingNaNs(bands.middle), 19u);
    EQUAL(leadingNaNs(bands.upper), 19u);
    EQUAL(leadingNaNs(bands.lower), 19u);
    CHECK(finiteFrom(bands.middle, 19) && finiteFrom(bands.upper, 19) && finiteFrom(bands.lower, 19));
    NEAR(bands.middle[19], 98.8);
    NEAR(bands.upper[19], 98.8 + 2 * std::sqrt(39.06));
    NEAR(bands.lower[19], 98.8 - 2 * std::sqrt(39.06));

    // No spread, no width.
    const Indicators::Bands level = Indicators::bollinger({7, 7, 7, 7}, 2, 2.0);
    SERIES(level.upper, kNaN, 7, 7, 7);
    SERIES(level.lower, kNaN, 7, 7, 7);

    for (int period : {0, -1, 9}) {
        const Indicators::Bands none = Indicators::bollinger(textbook, period, 2.0);
        EQUAL(none.middle.size(), 8u);
        EQUAL(none.upper.size(), 8u);
        EQUAL(none.lower.size(), 8u);
        CHECK(allNaN(none.middle) && allNaN(none.upper) && allNaN(none.lower));
    }
    const Indicators::Bands empty = Indicators::bollinger({});
    CHECK(empty.middle.empty() && empty.upper.empty() && empty.lower.empty());
}

void testTrueRangeAndAtr()
{
    Candles candles;
    candles.push_back(bar(0, 9, 10, 8, 9));       // nothing before it: 10 - 8 = 2
    candles.push_back(bar(1, 9, 11, 9, 10));      // the close of 9 is inside 9..11: 2
    candles.push_back(bar(2, 14, 15, 13, 14));    // gapped up from 10: 15 - 10 = 5, not 15 - 13
    candles.push_back(bar(3, 11, 11, 10, 10.5));  // gapped down from 14: 14 - 10 = 4, not 11 - 10
    candles.push_back(bar(4, 11, 12, 10, 11));    // 10.5 is inside 10..12: 2
    SERIES(Indicators::trueRange(candles), 2, 2, 5, 4, 2);

    // Period 3: (2 + 2 + 5) / 3 = 3, then (3 * 2 + 4) / 3 = 10/3 and
    // (10/3 * 2 + 2) / 3 = 26/9. A plain average of the last three would
    // give 11/3 twice.
    SERIES(Indicators::atr(candles, 3), kNaN, kNaN, 3, 10.0 / 3, 26.0 / 9);
    // Period 1 is the true range itself, period 5 only its mean, 15 / 5.
    SERIES(Indicators::atr(candles, 1), 2, 2, 5, 4, 2);
    SERIES(Indicators::atr(candles, 5), kNaN, kNaN, kNaN, kNaN, 3);
    SERIES(Indicators::atr(candles, 6), kNaN, kNaN, kNaN, kNaN, kNaN);
    SERIES(Indicators::atr(candles, 0), kNaN, kNaN, kNaN, kNaN, kNaN);
    SERIES(Indicators::atr(candles, -1), kNaN, kNaN, kNaN, kNaN, kNaN);

    // The default period, 14. Reference: the first fourteen true ranges of
    // the irregular series add up to 125.
    const Values average = Indicators::atr(irregular(70));
    EQUAL(average.size(), 70u);
    EQUAL(leadingNaNs(average), 13u);
    CHECK(finiteFrom(average, 13));
    NEAR(average[13], 125.0 / 14);
    NEAR(average[69], 11.59437896974203);
    CHECK(allNaN(Indicators::atr(irregular(13))));

    CHECK(Indicators::trueRange({}).empty());
    CHECK(Indicators::atr({}).empty());
}

void testCloses()
{
    SERIES(Indicators::closes(ramp(4, 10, 2)), 10, 12, 14, 16);
    // A plain accessor: it does not judge which candles are closed.
    Candles candles = ramp(3, 10, 2);
    candles.back().closed = false;
    SERIES(Indicators::closes(candles), 10, 12, 14);
    CHECK(Indicators::closes({}).empty());
}

// ------------------------------------------------------------------ summary

void testSummaryFlat()
{
    // Sixty identical candles from 99.5 to 100.5 with a volume of 10.
    const Indicators::Summary s = Indicators::summarize(flat(60, 100, 10));
    CHECK(s.valid);
    EQUAL(s.reason, "");
    NEAR(s.price, 100);
    NEAR(s.ema20, 100);
    NEAR(s.ema50, 100);
    IS_NAN(s.ema200);
    NEAR(s.rsi14, 50);
    NEAR(s.macd, 0);
    NEAR(s.macdSignal, 0);
    NEAR(s.macdHistogram, 0);
    NEAR(s.macdHistogramPrevious, 0);
    NEAR(s.bandUpper, 100);
    NEAR(s.bandMiddle, 100);
    NEAR(s.bandLower, 100);
    NEAR(s.bandPosition, 0.5); // bands of no width: the price is on all three
    NEAR(s.bandWidthPct, 0);
    NEAR(s.atr14, 1);
    NEAR(s.atrPct, 1);
    NEAR(s.volume, 10);
    NEAR(s.volumeAverage20, 10);
    NEAR(s.volumeRatio, 1);
    NEAR(s.change24Pct, 0);
    EQUAL(s.trend, "mixed"); // the price is not above its averages, it is on them
    EQUAL(s.momentum, "neutral");
    EQUAL(s.macdState, "flat");
    EQUAL(s.volumeState, "normal");

    // The same at a price of zero with no volume: every ratio in the summary
    // has a zero divisor, and none of them may turn into inf or NaN.
    const Indicators::Summary zero = Indicators::summarize(flat(60, 0, 0));
    CHECK(zero.valid);
    NEAR(zero.price, 0);
    NEAR(zero.bandPosition, 0.5);
    NEAR(zero.bandWidthPct, 0);
    NEAR(zero.atr14, 1);
    NEAR(zero.atrPct, 0);
    NEAR(zero.volumeRatio, 1);
    NEAR(zero.change24Pct, 0);
    EQUAL(zero.volumeState, "normal");
}

void testSummaryRising()
{
    // Closes 100, 101, ... 299. Each average trails by (period - 1) / 2 (see
    // testEma), so the MACD line is 12.5 - 5.5 = 7 and its signal line too.
    const Indicators::Summary s = Indicators::summarize(ramp(200, 100, 1));
    CHECK(s.valid);
    NEAR(s.price, 299);
    NEAR(s.ema20, 299 - 9.5);
    NEAR(s.ema50, 299 - 24.5);
    NEAR(s.ema200, 299 - 99.5);
    NEAR(s.rsi14, 100);
    NEAR(s.macd, 7);
    NEAR(s.macdSignal, 7);
    NEAR(s.macdHistogram, 0);
    NEAR(s.macdHistogramPrevious, 0);
    // Twenty consecutive whole numbers have a variance of (20 * 20 - 1) / 12.
    const double deviation = std::sqrt(399.0 / 12);
    NEAR(s.bandMiddle, 289.5);
    NEAR(s.bandUpper, 289.5 + 2 * deviation);
    NEAR(s.bandLower, 289.5 - 2 * deviation);
    NEAR(s.bandPosition, (9.5 + 2 * deviation) / (4 * deviation));
    NEAR(s.bandWidthPct, 4 * deviation / 289.5 * 100);
    NEAR(s.atr14, 2);
    NEAR(s.atrPct, 2.0 / 299 * 100);
    NEAR(s.change24Pct, (299.0 - 275) / 275 * 100); // 24 candles back, not 25
    EQUAL(s.trend, "bullish");
    EQUAL(s.momentum, "overbought");
    EQUAL(s.volumeState, "normal");

    // One candle less: everything but the 200-candle average.
    const Indicators::Summary shorter = Indicators::summarize(ramp(199, 100, 1));
    CHECK(shorter.valid);
    NEAR(shorter.price, 298);
    NEAR(shorter.ema50, 298 - 24.5);
    IS_NAN(shorter.ema200);
    EQUAL(shorter.trend, "bullish");
}

void testSummaryFalling()
{
    // Closes 200, 199, ... 141: the mirror image, the averages are above.
    const Indicators::Summary s = Indicators::summarize(ramp(60, 200, -1));
    CHECK(s.valid);
    NEAR(s.price, 141);
    NEAR(s.ema20, 141 + 9.5);
    NEAR(s.ema50, 141 + 24.5);
    IS_NAN(s.ema200);
    NEAR(s.rsi14, 0);
    NEAR(s.macd, -7);
    NEAR(s.macdSignal, -7);
    const double deviation = std::sqrt(399.0 / 12);
    NEAR(s.bandMiddle, 150.5);
    NEAR(s.bandPosition, (2 * deviation - 9.5) / (4 * deviation));
    NEAR(s.atr14, 2);
    NEAR(s.atrPct, 2.0 / 141 * 100);
    NEAR(s.change24Pct, (141.0 - 165) / 165 * 100);
    EQUAL(s.trend, "bearish");
    EQUAL(s.momentum, "oversold");
}

void testSummaryJump()
{
    // 59 closes of 100, then one of 110. Until the last candle every average
    // is 100 and the MACD is 0, so the last step can be done by hand:
    // ema(n) = 100 + 10 * 2 / (n + 1).
    Values closes(59, 100.0);
    closes.push_back(110);
    const Indicators::Summary up = Indicators::summarize(fromCloses(closes));
    CHECK(up.valid);
    NEAR(up.price, 110);
    NEAR(up.ema20, 100 + 20.0 / 21);
    NEAR(up.ema50, 100 + 20.0 / 51);
    NEAR(up.rsi14, 100); // one gain, no loss
    const double line = 20.0 / 13 - 20.0 / 27;
    NEAR(up.macd, line);
    NEAR(up.macdSignal, line * 2 / 10); // from 0, one step of 2 / (9 + 1)
    NEAR(up.macdHistogram, line * 8 / 10);
    NEAR(up.macdHistogramPrevious, 0);
    // The last twenty closes: mean 100.5, mean squared distance
    // (19 * 0.25 + 90.25) / 20 = 4.75.
    NEAR(up.bandMiddle, 100.5);
    NEAR(up.bandUpper, 100.5 + 2 * std::sqrt(4.75));
    NEAR(up.bandLower, 100.5 - 2 * std::sqrt(4.75));
    NEAR(up.bandPosition, (9.5 + 2 * std::sqrt(4.75)) / (4 * std::sqrt(4.75))); // above the upper band
    // True ranges of 1, then 110.5 - 99.5 = 11.
    NEAR(up.atr14, (13 * 1 + 11) / 14.0);
    NEAR(up.atrPct, 24.0 / 14 / 110 * 100);
    NEAR(up.change24Pct, 10);
    EQUAL(up.trend, "bullish");
    EQUAL(up.momentum, "overbought");
    EQUAL(up.macdState, "rising");

    closes.back() = 90;
    const Indicators::Summary down = Indicators::summarize(fromCloses(closes));
    NEAR(down.ema20, 100 - 20.0 / 21);
    NEAR(down.ema50, 100 - 20.0 / 51);
    NEAR(down.rsi14, 0);
    NEAR(down.macd, -line);
    NEAR(down.macdSignal, -line * 2 / 10);
    NEAR(down.macdHistogram, -line * 8 / 10);
    NEAR(down.macdHistogramPrevious, 0);
    NEAR(down.atr14, 24.0 / 14);
    NEAR(down.change24Pct, -10);
    EQUAL(down.trend, "bearish");
    EQUAL(down.momentum, "oversold");
    EQUAL(down.macdState, "falling");
}

void testSummaryMixed()
{
    // Down a straight line from 200 to 142, then one candle up to 160: the
    // price is back above the 20-candle average, but that average is still
    // below the 50-candle one.
    Values closes;
    for (int i = 0; i < 59; ++i)
        closes.push_back(200 - i);
    closes.push_back(160);
    const Indicators::Summary s = Indicators::summarize(fromCloses(closes));
    CHECK(s.valid);
    // Before the last candle the averages trail the line: 151.5 and 166.5.
    NEAR(s.ema20, 151.5 + (160 - 151.5) * 2 / 21);
    NEAR(s.ema50, 166.5 + (160 - 166.5) * 2 / 51);
    EQUAL(s.trend, "mixed");
    // After losses of 1 only, a gain of 18: gains 18 / 14, losses 13 / 14.
    NEAR(s.rsi14, 100 - 100 / (1 + 18.0 / 13));
    EQUAL(s.momentum, "neutral");
    // The line was -7 (147.5 - 154.5), like its signal line.
    const double line = (147.5 + (160 - 147.5) * 2 / 13) - (154.5 + (160 - 154.5) * 2 / 27);
    NEAR(s.macd, line);
    NEAR(s.macdSignal, -7 + (line + 7) * 2 / 10);
    NEAR(s.macdHistogram, (line + 7) * 8 / 10);
    NEAR(s.macdHistogramPrevious, 0);
    EQUAL(s.macdState, "rising");

    // The mirror image: up a line from 100 to 158, then one candle down to
    // 140, under the 20-candle average (148.5 before it) that is still
    // above the 50-candle one (133.5).
    closes.clear();
    for (int i = 0; i < 59; ++i)
        closes.push_back(100 + i);
    closes.push_back(140);
    const Indicators::Summary t = Indicators::summarize(fromCloses(closes));
    NEAR(t.ema20, 148.5 + (140 - 148.5) * 2 / 21);
    NEAR(t.ema50, 133.5 + (140 - 133.5) * 2 / 51);
    EQUAL(t.trend, "mixed");
    NEAR(t.rsi14, 100 - 100 / (1 + 13.0 / 18)); // gains 13 / 14, losses 18 / 14
    EQUAL(t.momentum, "neutral");
    const double falling = (152.5 + (140 - 152.5) * 2 / 13) - (145.5 + (140 - 145.5) * 2 / 27);
    NEAR(t.macd, falling);
    NEAR(t.macdHistogram, (falling - 7) * 8 / 10);
    EQUAL(t.macdState, "falling");
}

void testSummaryMomentum()
{
    // Zigzags from 100 that alternate a step up with a step down. The
    // readings are from the reference; they sit on the expected side of the
    // 70 and 30 lines whichever step came last.
    const auto zigzag = [](double up, double down) {
        Values closes = {100};
        for (int i = 1; i < 60; ++i)
            closes.push_back(closes.back() + (i % 2 == 1 ? up : -down));
        return Indicators::summarize(fromCloses(closes));
    };
    const Indicators::Summary strong = zigzag(3, 1);
    NEAR(strong.price, 161);
    NEAR(strong.rsi14, 76.41129457309556);
    EQUAL(strong.momentum, "overbought");
    const Indicators::Summary weak = zigzag(1, 3);
    NEAR(weak.price, 43);
    NEAR(weak.rsi14, 26.466485036385613);
    EQUAL(weak.momentum, "oversold");
    const Indicators::Summary firm = zigzag(2, 1);
    NEAR(firm.rsi14, 68.34986984445503);
    EQUAL(firm.momentum, "neutral");
    const Indicators::Summary soft = zigzag(1, 2);
    NEAR(soft.rsi14, 35.06013487885414);
    EQUAL(soft.momentum, "neutral");
}

void testSummaryVolume()
{
    // Sixty flat candles: forty with a volume of 1000 that the 20-candle
    // average must not see, nineteen with 'recent' and the last with 'last'.
    const auto volumes = [](double earlier, double recent, double last) {
        Candles candles = flat(60, 100, recent);
        for (int i = 0; i < 40; ++i)
            candles[static_cast<std::size_t>(i)].volume = earlier;
        candles.back().volume = last;
        return Indicators::summarize(candles);
    };

    // The average includes the last candle: (19 * 10 + 40) / 20 = 11.5.
    const Indicators::Summary busy = volumes(1000, 10, 40);
    NEAR(busy.volume, 40);
    NEAR(busy.volumeAverage20, 11.5);
    NEAR(busy.volumeRatio, 40 / 11.5);
    EQUAL(busy.volumeState, "high");

    // (19 * 37 + 57) / 20 = 38 and 57 / 38 is exactly 1.5: still "high".
    const Indicators::Summary justHigh = volumes(1000, 37, 57);
    NEAR(justHigh.volumeAverage20, 38);
    EQUAL(justHigh.volumeRatio, 1.5);
    EQUAL(justHigh.volumeState, "high");
    EQUAL(volumes(1000, 37, 56).volumeState, "normal");

    // (19 * 39 + 19) / 20 = 38 and 19 / 38 is exactly 0.5: already "low".
    const Indicators::Summary justLow = volumes(1000, 39, 19);
    NEAR(justLow.volumeAverage20, 38);
    EQUAL(justLow.volumeRatio, 0.5);
    EQUAL(justLow.volumeState, "low");
    EQUAL(volumes(1000, 39, 20).volumeState, "normal");

    const Indicators::Summary thin = volumes(1000, 10, 2);
    NEAR(thin.volumeAverage20, 9.6);
    NEAR(thin.volumeRatio, 2 / 9.6);
    EQUAL(thin.volumeState, "low");

    // A feed without volume: the last candle is as average as the others.
    const Indicators::Summary silent = volumes(0, 0, 0);
    NEAR(silent.volumeAverage20, 0);
    NEAR(silent.volumeRatio, 1);
    EQUAL(silent.volumeState, "normal");
}

void testSummaryIrregular()
{
    // Every number of the summary on a series with no pattern, against the
    // reference.
    const Indicators::Summary s = Indicators::summarize(irregular(70));
    CHECK(s.valid);
    NEAR(s.price, 89);
    NEAR(s.ema20, 98.24993838089522);
    NEAR(s.ema50, 98.79915926711254);
    IS_NAN(s.ema200);
    NEAR(s.rsi14, 45.588886101526796);
    NEAR(s.macd, -0.7328494460985848);
    NEAR(s.macdSignal, -0.12567183690952746);
    NEAR(s.macdHistogram, -0.6071776091890574);
    NEAR(s.macdHistogramPrevious, 0.0690674806313322);
    // The last twenty closes have mean 99.3 and mean squared distance 45.71.
    NEAR(s.bandMiddle, 99.3);
    NEAR(s.bandUpper, 99.3 + 2 * std::sqrt(45.71));
    NEAR(s.bandLower, 99.3 - 2 * std::sqrt(45.71));
    NEAR(s.bandPosition, 0.11913451047468995);
    NEAR(s.bandWidthPct, 27.234308550067066);
    NEAR(s.atr14, 11.59437896974203);
    NEAR(s.atrPct, 13.02739210083374);
    NEAR(s.volume, 103);
    NEAR(s.volumeAverage20, 124);
    NEAR(s.volumeRatio, 103.0 / 124);
    NEAR(s.change24Pct, (89.0 - 93) / 93 * 100);
    EQUAL(s.trend, "bearish");
    EQUAL(s.momentum, "neutral");
    EQUAL(s.macdState, "falling");
    EQUAL(s.volumeState, "normal");
}

void testSummaryValidity()
{
    const Indicators::Summary tooFew = Indicators::summarize(flat(59, 100, 10));
    CHECK(!tooFew.valid);
    CHECK(!tooFew.reason.empty());
    EQUAL(tooFew.trend, "");
    CHECK(Indicators::summarize(flat(60, 100, 10)).valid);

    // A forming candle does not count towards the sixty ...
    Candles candles = flat(59, 100, 10);
    candles.push_back(forming(bar(59, 100, 100.5, 99.5, 100)));
    const Indicators::Summary stillTooFew = Indicators::summarize(candles);
    CHECK(!stillTooFew.valid);
    CHECK(!stillTooFew.reason.empty());

    // ... and is not read: with a wild one after sixty flat candles the
    // summary is that of the flat market.
    candles = flat(60, 100, 10);
    candles.push_back(forming(bar(60, 100, 500, 50, 400, 1e6)));
    const Indicators::Summary s = Indicators::summarize(candles);
    CHECK(s.valid);
    NEAR(s.price, 100);
    NEAR(s.ema20, 100);
    NEAR(s.rsi14, 50);
    NEAR(s.macdHistogram, 0);
    NEAR(s.bandUpper, 100);
    NEAR(s.atr14, 1);
    NEAR(s.volume, 10);
    NEAR(s.volumeAverage20, 10);
    NEAR(s.change24Pct, 0);
    EQUAL(s.trend, "mixed");
    EQUAL(s.momentum, "neutral");
    EQUAL(s.macdState, "flat");
    EQUAL(s.volumeState, "normal");

    // The same candle once it has closed is read like any other.
    candles.back().closed = true;
    const Indicators::Summary closed = Indicators::summarize(candles);
    NEAR(closed.price, 400);
    EQUAL(closed.trend, "bullish");
    EQUAL(closed.volumeState, "high");

    // Should a feed ever deliver two unclosed candles, neither is read.
    candles.back().closed = false;
    candles.push_back(forming(bar(61, 400, 500, 50, 450, 1e6)));
    const Indicators::Summary twoForming = Indicators::summarize(candles);
    CHECK(twoForming.valid);
    NEAR(twoForming.price, 100);
}

// ------------------------------------------------------------------- swings

void testSwings()
{
    // Strength 2: 15 is above the two highs before it and the two after it.
    std::vector<Smc::Swing> found = Smc::swings(withHighs({10, 11, 12, 15, 12, 11, 10}), 2);
    SIZE_OR_RETURN(found, 1u);
    SWING(found[0], 3, 15, true);

    // The default strength is 3 and the same candle passes it.
    found = Smc::swings(withHighs({10, 11, 12, 15, 12, 11, 10}));
    SIZE_OR_RETURN(found, 1u);
    SWING(found[0], 3, 15, true);

    // Not known before 'strength' candles follow: one is too few, two are
    // enough, and a forming candle is not one of them.
    CHECK(Smc::swings(withHighs({10, 11, 12, 15, 12}), 2).empty());
    Candles candles = withHighs({10, 11, 12, 15, 12, 11});
    EQUAL(Smc::swings(candles, 2).size(), 1u);
    candles.back().closed = false;
    CHECK(Smc::swings(candles, 2).empty());
    // Nor is a candle a swing without 'strength' candles before it.
    CHECK(Smc::swings(withHighs({15, 10, 9, 8, 7}), 2).empty());
    CHECK(Smc::swings(withHighs({10, 15, 9, 8, 7}), 2).empty());

    // Equal highs: strictly above the ones before, at or above the ones
    // after, so the first of the two is the swing and the second is not.
    found = Smc::swings(withHighs({10, 11, 15, 15, 11, 10, 9}), 2);
    SIZE_OR_RETURN(found, 1u);
    SWING(found[0], 2, 15, true);
    // The same on a plateau of three.
    found = Smc::swings(withHighs({10, 11, 15, 15, 15, 9, 8}), 2);
    SIZE_OR_RETURN(found, 1u);
    SWING(found[0], 2, 15, true);
    // A higher high among the candles after it disqualifies a candle.
    found = Smc::swings(withHighs({10, 11, 15, 14, 16, 9, 8}), 2);
    SIZE_OR_RETURN(found, 1u);
    SWING(found[0], 4, 16, true);

    // Lows mirror all of it.
    found = Smc::swings(withLows({10, 9, 8, 5, 8, 9, 10}), 2);
    SIZE_OR_RETURN(found, 1u);
    SWING(found[0], 3, 5, false);
    CHECK(Smc::swings(withLows({10, 9, 8, 5, 8}), 2).empty());
    found = Smc::swings(withLows({10, 9, 5, 5, 9, 10, 11}), 2);
    SIZE_OR_RETURN(found, 1u);
    SWING(found[0], 2, 5, false);
    found = Smc::swings(withLows({10, 9, 5, 6, 4, 11, 12}), 2);
    SIZE_OR_RETURN(found, 1u);
    SWING(found[0], 4, 4, false);

    // Two swings in index order; one candle can be both, high first.
    candles = quiet(9);
    set(candles, 2, 100, 104, 99, 100);
    set(candles, 5, 100, 106, 94, 100);
    found = Smc::swings(candles, 2);
    SIZE_OR_RETURN(found, 3u);
    SWING(found[0], 2, 104, true);
    SWING(found[1], 5, 106, true);
    SWING(found[2], 5, 94, false);

    // Strength 1 compares with one neighbour on each side; less than that
    // compares with nothing and finds nothing.
    found = Smc::swings(withHighs({1, 3, 2}), 1);
    SIZE_OR_RETURN(found, 1u);
    SWING(found[0], 1, 3, true);
    CHECK(Smc::swings(withHighs({1, 3, 2}), 0).empty());
    CHECK(Smc::swings(withHighs({1, 3, 2}), -1).empty());
    CHECK(Smc::swings(withHighs({1, 3, 2}), 2).empty());
    CHECK(Smc::swings({}, 3).empty());
}

void testTrendName()
{
    EQUAL(name(Smc::Trend::Ranging), "ranging");
    EQUAL(name(Smc::Trend::Bullish), "bullish");
    EQUAL(name(Smc::Trend::Bearish), "bearish");
}

// ---------------------------------------------------------------- structure

void testBullishBreak()
{
    Candles candles = quiet(40);
    set(candles, 10, 100, 105, 99, 100); // swing high 105
    set(candles, 13, 100, 105, 99, 100); // an equal high, so that 14 is no swing of its own
    set(candles, 14, 100, 105, 99, 105); // closes on the level: not beyond it
    set(candles, 20, 100, 107, 99, 106); // closes above it
    const Smc::Analysis a = Smc::analyze(candles);
    CHECK(a.valid);
    EQUAL(a.reason, "");
    SIZE_OR_RETURN(a.swings, 2u);
    SWING(a.swings[0], 10, 105, true);
    SWING(a.swings[1], 20, 107, true);
    // No trend was set yet, so the first break is a BOS.
    SIZE_OR_RETURN(a.events, 1u);
    EVENT(a.events[0], 20, true, false, 105, 10);
    EQUAL(name(a.trend), "bullish");
    CHECK(a.orderBlocks.empty()); // no bearish candle between swing and break
    CHECK(a.gaps.empty());
    CHECK(a.sweeps.empty());
    // The range: the last swing high, and with no swing low the lowest low.
    NEAR(a.rangeHigh, 107);
    NEAR(a.rangeLow, 99);
    NEAR(a.equilibrium, 103);
    NEAR(a.rangePosition, 1.0 / 8);
    EQUAL(a.zone, "discount");
}

void testBearishBreak()
{
    Candles candles = quiet(40);
    set(candles, 10, 100, 101, 95, 100); // swing low 95
    set(candles, 13, 100, 101, 95, 100);
    set(candles, 14, 100, 101, 95, 95);  // closes on the level
    set(candles, 20, 100, 101, 93, 94);  // closes below it
    const Smc::Analysis a = Smc::analyze(candles);
    CHECK(a.valid);
    SIZE_OR_RETURN(a.swings, 2u);
    SWING(a.swings[0], 10, 95, false);
    SWING(a.swings[1], 20, 93, false);
    SIZE_OR_RETURN(a.events, 1u);
    EVENT(a.events[0], 20, false, false, 95, 10);
    EQUAL(name(a.trend), "bearish");
    CHECK(a.orderBlocks.empty());
    CHECK(a.gaps.empty());
    CHECK(a.sweeps.empty());
    NEAR(a.rangeHigh, 101);
    NEAR(a.rangeLow, 93);
    NEAR(a.equilibrium, 97);
    NEAR(a.rangePosition, 7.0 / 8);
    EQUAL(a.zone, "premium");
}

void testChangeOfCharacter()
{
    Candles candles = quiet(50);
    set(candles, 8, 100, 105, 99, 100);  // swing high 105
    set(candles, 14, 100, 107, 99, 106); // closes above it; its own high is the next swing high
    set(candles, 20, 100, 109, 99, 108); // closes above 107
    set(candles, 26, 100, 101, 95, 100); // swing low 95
    set(candles, 32, 100, 101, 93, 94);  // closes below it; its own low is the next swing low
    set(candles, 38, 100, 101, 91, 92);  // closes below 93
    set(candles, 46, 100, 111, 99, 110); // closes above the high of candle 20
    const Smc::Analysis a = Smc::analyze(candles);
    CHECK(a.valid);
    SIZE_OR_RETURN(a.swings, 7u);
    SWING(a.swings[0], 8, 105, true);
    SWING(a.swings[1], 14, 107, true);
    SWING(a.swings[2], 20, 109, true);
    SWING(a.swings[3], 26, 95, false);
    SWING(a.swings[4], 32, 93, false);
    SWING(a.swings[5], 38, 91, false);
    SWING(a.swings[6], 46, 111, true);
    SIZE_OR_RETURN(a.events, 5u);
    EVENT(a.events[0], 14, true, false, 105, 8);   // no trend yet: BOS
    EVENT(a.events[1], 20, true, false, 107, 14);  // with the bullish trend: BOS
    EVENT(a.events[2], 32, false, true, 95, 26);   // against it: CHoCH
    EVENT(a.events[3], 38, false, false, 93, 32);  // with the now bearish trend: BOS
    EVENT(a.events[4], 46, true, true, 109, 20);   // against it again: CHoCH
    EQUAL(name(a.trend), "bullish");
    CHECK(a.sweeps.empty());
    // Only the last break has a candle of the other colour behind it: the
    // bearish candle 38, the last one between candle 20 and candle 46.
    SIZE_OR_RETURN(a.orderBlocks, 1u);
    ZONE(a.orderBlocks[0], 38, true, 101, 91);
    EQUAL(a.orderBlocks[0].mitigated, true); // candle 47 trades at 99..101
}

void testSwingBrokenOnce()
{
    Candles candles = quiet(40);
    set(candles, 10, 100, 105, 99, 100);   // swing high 105
    set(candles, 16, 100, 106.5, 99, 106); // the break
    set(candles, 17, 100, 106.5, 99, 106.2);
    set(candles, 18, 100, 107.5, 99, 107); // two more closes above 105
    set(candles, 30, 100, 106.5, 99, 106); // and one much later
    const Smc::Analysis a = Smc::analyze(candles);
    SIZE_OR_RETURN(a.events, 1u);
    EVENT(a.events[0], 16, true, false, 105, 10);
    CHECK(a.sweeps.empty());
}

void testOlderSwingBreaksNext()
{
    // Two unbroken swing highs, the older one higher. The level to break is
    // the more recent one; once that is gone it is the older one.
    Candles candles = quiet(40);
    set(candles, 8, 100, 110, 99, 100);     // swing high 110
    set(candles, 16, 100, 105, 99, 100);    // swing high 105
    set(candles, 20, 100.5, 101, 99, 99.5); // the only bearish candle
    set(candles, 24, 100, 107, 99, 106);    // closes above 105 only
    set(candles, 25, 100, 109, 99, 108);
    set(candles, 26, 100, 112, 99, 111);    // closes above 110
    const Smc::Analysis a = Smc::analyze(candles);
    SIZE_OR_RETURN(a.events, 2u);
    EVENT(a.events[0], 24, true, false, 105, 16);
    EVENT(a.events[1], 26, true, false, 110, 8);
    CHECK(a.sweeps.empty());
    // Both breaks lead back to candle 20: one zone, not two.
    SIZE_OR_RETURN(a.orderBlocks, 1u);
    ZONE(a.orderBlocks[0], 20, true, 101, 99);
}

void testOneCloseThroughTwoSwings()
{
    // A single close above both unbroken highs is one event, against the
    // more recent swing, and uses up both: the next close above the older
    // level is not a break of it.
    Candles candles = quiet(40);
    set(candles, 8, 100, 110, 99, 100);
    set(candles, 16, 100, 105, 99, 100);
    set(candles, 24, 100, 113, 99, 112);
    set(candles, 26, 100, 113, 99, 112.5);
    const Smc::Analysis a = Smc::analyze(candles);
    SIZE_OR_RETURN(a.events, 1u);
    EVENT(a.events[0], 24, true, false, 105, 16);
    CHECK(a.sweeps.empty());
}

void testOlderLowerSwing()
{
    // The older swing high is the lower one. A close above it that stays
    // under the more recent swing is no event (the most recent unbroken
    // swing was not passed), but the older level is used up all the same: a
    // later wick through it is no sweep.
    Candles candles = quiet(40);
    set(candles, 8, 100, 103, 99, 100);     // swing high 103
    set(candles, 16, 100, 105, 99, 100);    // swing high 105; its wick sweeps 103
    set(candles, 24, 100, 104.5, 99, 104);  // closes above 103, under 105
    set(candles, 30, 100, 103.5, 99, 100);  // wick above 103
    const Smc::Analysis a = Smc::analyze(candles);
    CHECK(a.events.empty());
    EQUAL(name(a.trend), "ranging");
    SIZE_OR_RETURN(a.sweeps, 1u);
    SWEEP(a.sweeps[0], 16, false, 103);
}

void testSwingStrengthAndTiming()
{
    // Two highs three candles apart. With the default strength of 3 the
    // second one is among the candles after the first and outdoes it; with
    // a strength of 2 both are swings, and the second, the first candle that
    // comes after the two that confirm the first, sweeps it.
    Candles candles = quiet(40);
    set(candles, 10, 100, 105, 99, 100);
    set(candles, 13, 100, 106, 99, 100);
    Smc::Analysis a = Smc::analyze(candles);
    SIZE_OR_RETURN(a.swings, 1u);
    SWING(a.swings[0], 13, 106, true);
    CHECK(a.sweeps.empty());
    a = Smc::analyze(candles, 2);
    SIZE_OR_RETURN(a.swings, 2u);
    SWING(a.swings[0], 10, 105, true);
    SWING(a.swings[1], 13, 106, true);
    SIZE_OR_RETURN(a.sweeps, 1u);
    SWEEP(a.sweeps[0], 13, false, 105);

    // A swing cannot be broken before it is known. Only bad data can try:
    // candle 12 closes above its own high, and above the swing high of
    // candle 10 that needs candle 13 to be confirmed.
    candles = quiet(40);
    set(candles, 10, 100, 105, 99, 100);
    set(candles, 12, 100, 101, 99, 106);
    a = Smc::analyze(candles);
    SIZE_OR_RETURN(a.swings, 1u);
    CHECK(a.events.empty());
    EQUAL(name(a.trend), "ranging");
}

// ------------------------------------------------------------- order blocks

// A swing high of 105 at candle 10, bearish candles at 13 and 16, a bullish
// one at 17, the break at candle 20 and then a market that stays at 110,
// above everything before. 'retestLow' is the low of candle 30.
Candles bullishBlockSeries(double retestLow)
{
    Candles candles = quiet(40);
    set(candles, 10, 100, 105, 99, 100);
    set(candles, 13, 100.5, 101, 99, 99.5);
    set(candles, 16, 100.4, 101, 99, 99.6);
    set(candles, 17, 99.6, 101, 99, 100.4);
    set(candles, 20, 100, 109.5, 99, 109);
    for (int i = 21; i < 40; ++i)
        set(candles, i, 110, 111, 109, 110);
    set(candles, 30, 110, 111, retestLow, 110);
    return candles;
}

void testBullishOrderBlock()
{
    // The last bearish candle before the break is 16, not 13; its range is
    // 99..101. The candles between it and the break overlap that range, but
    // only what happens after the break can mitigate it.
    Smc::Analysis a = Smc::analyze(bullishBlockSeries(109));
    SIZE_OR_RETURN(a.events, 1u);
    EVENT(a.events[0], 20, true, false, 105, 10);
    SIZE_OR_RETURN(a.orderBlocks, 1u);
    ZONE(a.orderBlocks[0], 16, true, 101, 99);
    EQUAL(a.orderBlocks[0].mitigated, false);

    // A dip that stops short of the zone, one that touches its top, one that
    // goes into it.
    a = Smc::analyze(bullishBlockSeries(101.5));
    SIZE_OR_RETURN(a.orderBlocks, 1u);
    EQUAL(a.orderBlocks[0].mitigated, false);
    a = Smc::analyze(bullishBlockSeries(101));
    SIZE_OR_RETURN(a.orderBlocks, 1u);
    ZONE(a.orderBlocks[0], 16, true, 101, 99);
    EQUAL(a.orderBlocks[0].mitigated, true);
    a = Smc::analyze(bullishBlockSeries(100));
    SIZE_OR_RETURN(a.orderBlocks, 1u);
    EQUAL(a.orderBlocks[0].mitigated, true);
}

// The mirror image: a swing low of 95 at candle 10, bullish candles at 13
// and 16, a bearish one at 17, the break at 20 and then a market at 90.
// 'retestHigh' is the high of candle 30.
Candles bearishBlockSeries(double retestHigh)
{
    Candles candles = quiet(40);
    set(candles, 10, 100, 101, 95, 100);
    set(candles, 13, 99.5, 101, 99, 100.5);
    set(candles, 16, 99.6, 101, 99, 100.4);
    set(candles, 17, 100.4, 101, 99, 99.6);
    set(candles, 20, 100, 101, 90.5, 91);
    for (int i = 21; i < 40; ++i)
        set(candles, i, 90, 91, 89, 90);
    set(candles, 30, 90, retestHigh, 89, 90);
    return candles;
}

void testBearishOrderBlock()
{
    Smc::Analysis a = Smc::analyze(bearishBlockSeries(91));
    SIZE_OR_RETURN(a.events, 1u);
    EVENT(a.events[0], 20, false, false, 95, 10);
    SIZE_OR_RETURN(a.orderBlocks, 1u);
    ZONE(a.orderBlocks[0], 16, false, 101, 99);
    EQUAL(a.orderBlocks[0].mitigated, false);

    a = Smc::analyze(bearishBlockSeries(98.5));
    SIZE_OR_RETURN(a.orderBlocks, 1u);
    EQUAL(a.orderBlocks[0].mitigated, false);
    a = Smc::analyze(bearishBlockSeries(99));
    SIZE_OR_RETURN(a.orderBlocks, 1u);
    EQUAL(a.orderBlocks[0].mitigated, true);
    a = Smc::analyze(bearishBlockSeries(100));
    SIZE_OR_RETURN(a.orderBlocks, 1u);
    ZONE(a.orderBlocks[0], 16, false, 101, 99);
    EQUAL(a.orderBlocks[0].mitigated, true);
}

void testOrderBlockSearchLimits()
{
    // The breaking candle is the move itself: it can be bearish (it opens
    // above the level and falls, still closing above it) without being the
    // block.
    Candles candles = quiet(40);
    set(candles, 10, 100, 105, 99, 100);
    set(candles, 16, 100.4, 101, 99, 99.6);
    set(candles, 20, 108, 108.5, 99, 106);
    Smc::Analysis a = Smc::analyze(candles);
    SIZE_OR_RETURN(a.events, 1u);
    SIZE_OR_RETURN(a.orderBlocks, 1u);
    ZONE(a.orderBlocks[0], 16, true, 101, 99);
    EQUAL(a.orderBlocks[0].mitigated, true); // the quiet candles after the break trade at 99..101

    // The swing candle itself can be the block: here it is the last bearish
    // candle before the move up.
    candles = quiet(40);
    set(candles, 10, 104, 105, 99, 100);
    set(candles, 20, 100, 107, 99, 106);
    a = Smc::analyze(candles);
    SIZE_OR_RETURN(a.events, 1u);
    SIZE_OR_RETURN(a.orderBlocks, 1u);
    ZONE(a.orderBlocks[0], 10, true, 105, 99);

    // A bearish candle from before the swing is not between swing and break.
    candles = quiet(40);
    set(candles, 9, 100.5, 101, 99, 99.5);
    set(candles, 10, 100, 105, 99, 100);
    set(candles, 20, 100, 107, 99, 106);
    a = Smc::analyze(candles);
    SIZE_OR_RETURN(a.events, 1u);
    CHECK(a.orderBlocks.empty());
}

// ---------------------------------------------------------- fair value gaps

// A jump from the quiet market at 100 to one at 108: candle 21 has its low
// at 107, above the high of candle 19 at 101. 'laterLow' is the low of
// candle 30.
Candles bullishGapSeries(double laterLow)
{
    Candles candles = quiet(40);
    set(candles, 20, 100, 108, 100, 107.5);
    for (int i = 21; i < 40; ++i)
        set(candles, i, 108, 109, 107, 108);
    set(candles, 30, 108, 109, laterLow, 108);
    return candles;
}

void testBullishGap()
{
    // The quiet candles before the gap have lows under its bottom; only
    // candles after the three that made the gap can fill it.
    Smc::Analysis a = Smc::analyze(bullishGapSeries(107));
    SIZE_OR_RETURN(a.gaps, 1u);
    ZONE(a.gaps[0], 20, true, 107, 101);
    EQUAL(a.gaps[0].filled, false);

    // Into the gap but not through it, then down to its bottom, then beyond.
    a = Smc::analyze(bullishGapSeries(103));
    SIZE_OR_RETURN(a.gaps, 1u);
    EQUAL(a.gaps[0].filled, false);
    a = Smc::analyze(bullishGapSeries(101));
    SIZE_OR_RETURN(a.gaps, 1u);
    ZONE(a.gaps[0], 20, true, 107, 101);
    EQUAL(a.gaps[0].filled, true);
    a = Smc::analyze(bullishGapSeries(100));
    SIZE_OR_RETURN(a.gaps, 1u);
    EQUAL(a.gaps[0].filled, true);

    // The very next candle after the three can already fill it.
    Candles candles = bullishGapSeries(107);
    set(candles, 22, 108, 109, 101, 108);
    a = Smc::analyze(candles);
    SIZE_OR_RETURN(a.gaps, 1u);
    EQUAL(a.gaps[0].filled, true);
}

// The mirror image: down to a market at 92; candle 21 has its high at 93,
// under the low of candle 19 at 99. 'laterHigh' is the high of candle 30.
Candles bearishGapSeries(double laterHigh)
{
    Candles candles = quiet(40);
    set(candles, 20, 100, 100, 92, 92.5);
    for (int i = 21; i < 40; ++i)
        set(candles, i, 92, 93, 91, 92);
    set(candles, 30, 92, laterHigh, 91, 92);
    return candles;
}

void testBearishGap()
{
    Smc::Analysis a = Smc::analyze(bearishGapSeries(93));
    SIZE_OR_RETURN(a.gaps, 1u);
    ZONE(a.gaps[0], 20, false, 99, 93);
    EQUAL(a.gaps[0].filled, false);

    a = Smc::analyze(bearishGapSeries(97));
    SIZE_OR_RETURN(a.gaps, 1u);
    EQUAL(a.gaps[0].filled, false);
    a = Smc::analyze(bearishGapSeries(99));
    SIZE_OR_RETURN(a.gaps, 1u);
    ZONE(a.gaps[0], 20, false, 99, 93);
    EQUAL(a.gaps[0].filled, true);
    a = Smc::analyze(bearishGapSeries(100));
    SIZE_OR_RETURN(a.gaps, 1u);
    EQUAL(a.gaps[0].filled, true);
}

void testGapSizeFilter()
{
    // A gap of half a point, from 101 to 101.5, around a middle candle that
    // closes at 'middleClose'.
    const auto series = [](double middleClose) {
        Candles candles = quiet(40);
        set(candles, 20, 100, std::max(102.0, middleClose), 99, middleClose);
        for (int i = 21; i < 40; ++i)
            set(candles, i, 101.75, 102, 101.5, 101.75);
        return candles;
    };

    // Against a close of 100 the gap is 0.5 %. "Smaller than" the minimum is
    // ignored, so exactly the minimum stays.
    Smc::Analysis a = Smc::analyze(series(100)); // the default minimum, 0.05 %
    SIZE_OR_RETURN(a.gaps, 1u);
    ZONE(a.gaps[0], 20, true, 101.5, 101);
    EQUAL(Smc::analyze(series(100), 3, 0.0).gaps.size(), 1u);
    EQUAL(Smc::analyze(series(100), 3, 0.5).gaps.size(), 1u);
    EQUAL(Smc::analyze(series(100), 3, 0.51).gaps.size(), 0u);
    EQUAL(Smc::analyze(series(100), 3, 5.0).gaps.size(), 0u);

    // The yardstick is the close of the middle candle, not of a neighbour:
    // against a close of 150 the same gap is a third of a percent.
    EQUAL(Smc::analyze(series(150), 3, 0.3).gaps.size(), 1u);
    EQUAL(Smc::analyze(series(150), 3, 0.4).gaps.size(), 0u);

    // Candles that only touch (the low of the third on the high of the
    // first, 101) leave no gap, even without a minimum size.
    Candles touching = quiet(40);
    set(touching, 20, 100, 102, 99, 100);
    for (int i = 21; i < 40; ++i)
        set(touching, i, 101.5, 102, 101, 101.5);
    EQUAL(Smc::analyze(touching, 3, 0.0).gaps.size(), 0u);
    // The same downwards: the high of the third on the low of the first, 99.
    touching = quiet(40);
    set(touching, 20, 100, 101, 98, 100);
    for (int i = 21; i < 40; ++i)
        set(touching, i, 98.5, 99, 98, 98.5);
    EQUAL(Smc::analyze(touching, 3, 0.0).gaps.size(), 0u);
}

void testGapAtTheEdges()
{
    // The first three candles and the last three can hold a gap like any
    // others: here candle 1 jumps and candle 38 drops back.
    Candles candles = quiet(40);
    set(candles, 1, 100, 108, 100, 107.5);
    for (int i = 2; i < 38; ++i)
        set(candles, i, 108, 109, 107, 108);
    set(candles, 38, 108, 108, 100, 100.5);
    const Smc::Analysis a = Smc::analyze(candles);
    SIZE_OR_RETURN(a.gaps, 2u);
    ZONE(a.gaps[0], 1, true, 107, 101);    // between the high of candle 0 and the low of candle 2
    EQUAL(a.gaps[0].filled, true);         // candle 38 trades down to 100
    ZONE(a.gaps[1], 38, false, 107, 101);  // between the low of candle 37 and the high of candle 39
    EQUAL(a.gaps[1].filled, false);
}

// ------------------------------------------------------------------- sweeps

void testBullishSweep()
{
    Candles candles = quiet(40);
    set(candles, 10, 100, 101, 95, 100); // swing low 95
    set(candles, 18, 100, 101, 94, 100); // wick below it, close back above
    set(candles, 20, 100, 101, 92, 93);  // closes below it
    const Smc::Analysis a = Smc::analyze(candles);
    SIZE_OR_RETURN(a.sweeps, 1u);
    SWEEP(a.sweeps[0], 18, true, 95);
    // The sweep left the swing unbroken: the later close is its break.
    SIZE_OR_RETURN(a.events, 1u);
    EVENT(a.events[0], 20, false, false, 95, 10);
}

void testBearishSweep()
{
    Candles candles = quiet(40);
    set(candles, 10, 100, 105, 99, 100); // swing high 105
    set(candles, 18, 100, 106, 99, 100); // wick above it, close back below
    set(candles, 20, 100, 108, 99, 107); // closes above it
    const Smc::Analysis a = Smc::analyze(candles);
    SIZE_OR_RETURN(a.sweeps, 1u);
    SWEEP(a.sweeps[0], 18, false, 105);
    SIZE_OR_RETURN(a.events, 1u);
    EVENT(a.events[0], 20, true, false, 105, 10);
}

void testSweepAge()
{
    // How old a finding is, counted in closed candles from the newest one.
    Candles candles = quiet(40);
    set(candles, 10, 100, 101, 95, 100); // swing low 95
    set(candles, 18, 100, 101, 94, 100); // swept 21 candles before the newest
    set(candles, 30, 100, 106, 99, 100); // swing high 106
    set(candles, 38, 100, 107, 99, 100); // swept 1 candle before the newest
    Smc::Analysis a = Smc::analyze(candles);
    EQUAL(a.closedCandles, 40);
    SIZE_OR_RETURN(a.sweeps, 2u);
    SWEEP(a.sweeps[0], 18, true, 95);
    SWEEP(a.sweeps[1], 38, false, 106);
    EQUAL(Smc::age(a, 39), 0);
    EQUAL(Smc::age(a, 38), 1);
    EQUAL(Smc::age(a, 18), 21);
    int lows = -1, highs = -1;
    EQUAL(Smc::recentSweeps(a, 1, &lows, &highs), 0); // only the newest candle
    EQUAL(lows, 0);
    EQUAL(highs, 0);
    EQUAL(Smc::recentSweeps(a, 2, &lows, &highs), 1);
    EQUAL(lows, 0);
    EQUAL(highs, 1);
    EQUAL(Smc::recentSweeps(a, 21), 1); // 21 candles ago is just outside
    EQUAL(Smc::recentSweeps(a, 22, &lows, &highs), 2);
    EQUAL(lows, 1);
    EQUAL(highs, 1);
    EQUAL(Smc::recentSweeps(a, 0), 0);

    // A forming candle is not read, so it is not counted either.
    candles.back().closed = false;
    a = Smc::analyze(candles);
    EQUAL(a.closedCandles, 39);
    EQUAL(Smc::age(a, 38), 0);
    EQUAL(Smc::recentSweeps(a, 1), 1);

    // Too little history: nothing was read.
    const Smc::Analysis none = Smc::analyze(quiet(10));
    CHECK(!none.valid);
    EQUAL(none.closedCandles, 0);
    EQUAL(Smc::recentSweeps(none, 100), 0);
}

void testSweepNeedsWickAndClose()
{
    // A low exactly on the level did not trade beyond it.
    Candles candles = quiet(40);
    set(candles, 10, 100, 101, 95, 100);
    set(candles, 18, 100, 101, 95, 100);
    Smc::Analysis a = Smc::analyze(candles);
    CHECK(a.sweeps.empty());
    CHECK(a.events.empty());

    // A close exactly on the level is neither back above it nor below it.
    candles = quiet(40);
    set(candles, 10, 100, 101, 95, 100);
    set(candles, 18, 100, 101, 94, 95);
    a = Smc::analyze(candles);
    CHECK(a.sweeps.empty());
    CHECK(a.events.empty());
}

void testSweepWickBecomesTheSwing()
{
    // Left alone for three candles, the sweeping wick is the newer swing low
    // (94) and so the level to break: a close under both lows is one event,
    // against the wick.
    Candles candles = quiet(40);
    set(candles, 10, 100, 101, 95, 100);
    set(candles, 18, 100, 101, 94, 100);
    set(candles, 26, 100, 101, 92, 93);
    Smc::Analysis a = Smc::analyze(candles);
    SIZE_OR_RETURN(a.sweeps, 1u);
    SWEEP(a.sweeps[0], 18, true, 95);
    SIZE_OR_RETURN(a.events, 1u);
    EVENT(a.events[0], 26, false, false, 94, 18);

    // A close between the two lows passes the swept level only: no event,
    // and that level is used up, so the wick of candle 34 sweeps nothing.
    candles = quiet(40);
    set(candles, 10, 100, 101, 95, 100);
    set(candles, 18, 100, 101, 94, 100);
    set(candles, 26, 100, 101, 94.2, 94.5);
    set(candles, 34, 100, 101, 94.8, 100);
    a = Smc::analyze(candles);
    CHECK(a.events.empty());
    SIZE_OR_RETURN(a.sweeps, 1u);
    SWEEP(a.sweeps[0], 18, true, 95);
}

void testSweepOfTwoSwings()
{
    // One wick through two unbroken lows is a sweep of each, the more
    // recent swing first.
    Candles candles = quiet(40);
    set(candles, 8, 100, 101, 95, 100);  // swing low 95
    set(candles, 16, 100, 101, 96, 100); // swing low 96, does not reach 95
    set(candles, 24, 100, 101, 94, 100); // through both
    const Smc::Analysis a = Smc::analyze(candles);
    CHECK(a.events.empty());
    SIZE_OR_RETURN(a.sweeps, 2u);
    SWEEP(a.sweeps[0], 24, true, 96);
    SWEEP(a.sweeps[1], 24, true, 95);
}

// -------------------------------------------------------------------- range

// A swing high of 110 and a swing low of 90, and a last candle that closes
// where the test wants it.
Candles rangeSeries(double lastClose)
{
    Candles candles = quiet(40);
    set(candles, 10, 100, 110, 99, 100);
    set(candles, 20, 100, 101, 90, 100);
    set(candles, 39, 100, std::max(101.0, lastClose), std::min(99.0, lastClose), lastClose);
    return candles;
}

void testZones()
{
    Smc::Analysis a = Smc::analyze(rangeSeries(100));
    NEAR(a.rangeHigh, 110);
    NEAR(a.rangeLow, 90);
    NEAR(a.equilibrium, 100);
    NEAR(a.rangePosition, 0.5);
    EQUAL(a.zone, "equilibrium");

    a = Smc::analyze(rangeSeries(108));
    NEAR(a.rangePosition, 0.9);
    EQUAL(a.zone, "premium");
    a = Smc::analyze(rangeSeries(92));
    NEAR(a.rangePosition, 0.1);
    EQUAL(a.zone, "discount");

    // The middle band runs from 45 % to 55 % with both ends in it: 99 is at
    // 9 / 20 and 101 at 11 / 20 of the range.
    EQUAL(Smc::analyze(rangeSeries(101)).zone, "equilibrium");
    EQUAL(Smc::analyze(rangeSeries(101.5)).zone, "premium");
    EQUAL(Smc::analyze(rangeSeries(99)).zone, "equilibrium");
    EQUAL(Smc::analyze(rangeSeries(98.5)).zone, "discount");

    // A close outside the range puts the position outside 0..1.
    a = Smc::analyze(rangeSeries(120));
    NEAR(a.rangeHigh, 110);
    NEAR(a.rangePosition, 1.5);
    EQUAL(a.zone, "premium");
    a = Smc::analyze(rangeSeries(80));
    NEAR(a.rangeLow, 90);
    NEAR(a.rangePosition, -0.5);
    EQUAL(a.zone, "discount");
}

void testRangeLevels()
{
    // The most recent swing of each kind, not the most extreme one.
    Candles candles = quiet(40);
    set(candles, 10, 100, 110, 99, 100);
    set(candles, 16, 100, 101, 90, 100);
    set(candles, 24, 100, 106, 99, 100);
    set(candles, 30, 100, 101, 94, 100);
    Smc::Analysis a = Smc::analyze(candles);
    EQUAL(a.swings.size(), 4u);
    NEAR(a.rangeHigh, 106);
    NEAR(a.rangeLow, 94);
    NEAR(a.equilibrium, 100);
    NEAR(a.rangePosition, 0.5);

    // No swings at all (the last two candles have too little after them to
    // be swings): the highest high and the lowest low of the series.
    candles = quiet(40);
    set(candles, 38, 100, 104, 99, 100);
    set(candles, 39, 100, 103, 96, 103);
    a = Smc::analyze(candles);
    CHECK(a.swings.empty());
    NEAR(a.rangeHigh, 104);
    NEAR(a.rangeLow, 96);
    NEAR(a.equilibrium, 100);
    NEAR(a.rangePosition, 7.0 / 8);
    EQUAL(a.zone, "premium");

    // A market that never moved has a range of no width; that is the middle.
    candles.clear();
    for (int i = 0; i < 40; ++i)
        candles.push_back(bar(i, 100, 100, 100, 100));
    a = Smc::analyze(candles);
    CHECK(a.valid);
    NEAR(a.rangeHigh, 100);
    NEAR(a.rangeLow, 100);
    NEAR(a.rangePosition, 0.5);
    EQUAL(a.zone, "equilibrium");
}

void testRangeStaysUpright()
{
    // A swing high of 105, then a steady climb with rising highs (no new
    // swing high) and one long lower wick that makes a swing low of 110,
    // above the old high. The range is the span between the two levels,
    // lower one at the bottom, and the price at 126 is far above it.
    Candles candles = quiet(40);
    set(candles, 10, 100, 105, 99, 100);
    for (int i = 14; i < 40; ++i) {
        const double close = 87 + i;
        set(candles, i, close, close + 1, close - 1, close);
    }
    set(candles, 30, 117, 118, 110, 117);
    const Smc::Analysis a = Smc::analyze(candles);
    SIZE_OR_RETURN(a.swings, 2u);
    SWING(a.swings[0], 10, 105, true);
    SWING(a.swings[1], 30, 110, false);
    NEAR(a.rangeHigh, 110);
    NEAR(a.rangeLow, 105);
    NEAR(a.equilibrium, 107.5);
    NEAR(a.rangePosition, (126.0 - 105) / 5);
    EQUAL(a.zone, "premium");
}

// ----------------------------------------------------------------- validity

void testAnalysisValidity()
{
    const Smc::Analysis tooFew = Smc::analyze(quiet(29));
    CHECK(!tooFew.valid);
    CHECK(!tooFew.reason.empty());
    EQUAL(name(tooFew.trend), "ranging");
    EQUAL(tooFew.zone, "");
    CHECK(tooFew.swings.empty() && tooFew.events.empty() && tooFew.orderBlocks.empty() && tooFew.gaps.empty()
          && tooFew.sweeps.empty());

    const Smc::Analysis enough = Smc::analyze(quiet(30));
    CHECK(enough.valid);
    EQUAL(enough.reason, "");
    EQUAL(enough.zone, "equilibrium");

    // A forming candle does not count towards the thirty.
    Candles candles = quiet(29);
    candles.push_back(forming(bar(29, 100, 101, 99, 100)));
    CHECK(!Smc::analyze(candles).valid);
    candles = quiet(30);
    candles.push_back(forming(bar(30, 100, 101, 99, 100)));
    CHECK(Smc::analyze(candles).valid);
}

void testFormingCandleIgnored()
{
    // A swing high of 105 and a last candle that closes above it and leaves
    // a gap behind (its low of 102 is above the high of candle 38): none of
    // it counts while that candle is still forming.
    Candles candles = quiet(41);
    set(candles, 10, 100, 105, 99, 100);
    set(candles, 39, 100, 101, 99, 100.5);
    set(candles, 40, 103, 107, 102, 106);
    candles[40].closed = false;
    Smc::Analysis a = Smc::analyze(candles);
    CHECK(a.valid);
    CHECK(a.events.empty());
    CHECK(a.gaps.empty());
    EQUAL(name(a.trend), "ranging");
    EQUAL(a.swings.size(), 1u);
    NEAR(a.rangeHigh, 105);
    NEAR(a.rangeLow, 99);
    NEAR(a.rangePosition, 1.5 / 6); // from the close of candle 39, 100.5

    candles[40].closed = true;
    a = Smc::analyze(candles);
    SIZE_OR_RETURN(a.events, 1u);
    EVENT(a.events[0], 40, true, false, 105, 10);
    SIZE_OR_RETURN(a.gaps, 1u);
    ZONE(a.gaps[0], 39, true, 102, 101);
    EQUAL(name(a.trend), "bullish");
    NEAR(a.rangePosition, 7.0 / 6);

    // Should a feed ever deliver two unclosed candles, neither is read.
    candles[40].closed = false;
    candles.push_back(forming(bar(41, 106, 109, 105, 108)));
    a = Smc::analyze(candles);
    CHECK(a.valid);
    CHECK(a.events.empty());
    CHECK(a.gaps.empty());

    // A forming candle neither mitigates an order block nor fills a gap.
    // Here it dips to 100: into the block at 99..101 and through the gap
    // that the break left between 101 and 109.
    candles = bullishBlockSeries(109);
    candles.push_back(forming(bar(40, 110, 111, 100, 110)));
    a = Smc::analyze(candles);
    SIZE_OR_RETURN(a.orderBlocks, 1u);
    EQUAL(a.orderBlocks[0].mitigated, false);
    SIZE_OR_RETURN(a.gaps, 1u);
    ZONE(a.gaps[0], 20, true, 109, 101);
    EQUAL(a.gaps[0].filled, false);
    candles.back().closed = true;
    a = Smc::analyze(candles);
    SIZE_OR_RETURN(a.orderBlocks, 1u);
    EQUAL(a.orderBlocks[0].mitigated, true);
    SIZE_OR_RETURN(a.gaps, 1u);
    EQUAL(a.gaps[0].filled, true);

    // Its extremes are not the range of a market without swings.
    candles = quiet(41);
    set(candles, 40, 100, 150, 50, 100);
    candles[40].closed = false;
    a = Smc::analyze(candles);
    NEAR(a.rangeHigh, 101);
    NEAR(a.rangeLow, 99);
    candles[40].closed = true;
    a = Smc::analyze(candles);
    NEAR(a.rangeHigh, 150);
    NEAR(a.rangeLow, 50);

    // A swing is not confirmed by a forming candle either: candle 37 needs
    // 38, 39 and 40 closed.
    candles = quiet(41);
    set(candles, 37, 100, 105, 99, 100);
    candles[40].closed = false;
    CHECK(Smc::analyze(candles).swings.empty());
    candles[40].closed = true;
    EQUAL(Smc::analyze(candles).swings.size(), 1u);
}

// --------------------------------------------------------------- robustness

void testEmptyAndTiny()
{
    const Candles none;
    CHECK(Indicators::trueRange(none).empty());
    CHECK(Indicators::atr(none).empty());
    CHECK(Indicators::closes(none).empty());
    const Indicators::Summary emptySummary = Indicators::summarize(none);
    CHECK(!emptySummary.valid);
    CHECK(!emptySummary.reason.empty());
    CHECK(Smc::swings(none).empty());
    const Smc::Analysis emptyAnalysis = Smc::analyze(none);
    CHECK(!emptyAnalysis.valid);
    CHECK(!emptyAnalysis.reason.empty());

    const Candles one = {bar(0, 10, 12, 9, 11)};
    SERIES(Indicators::closes(one), 11);
    SERIES(Indicators::trueRange(one), 3);
    SERIES(Indicators::atr(one), kNaN);
    SERIES(Indicators::atr(one, 1), 3);
    SERIES(Indicators::sma({11}, 20), kNaN);
    SERIES(Indicators::ema({11}, 20), kNaN);
    SERIES(Indicators::rsi({11}), kNaN);
    SERIES(Indicators::rsi({11}, 1), kNaN);
    SERIES(Indicators::macd({11}).histogram, kNaN);
    SERIES(Indicators::bollinger({11}).upper, kNaN);
    CHECK(!Indicators::summarize(one).valid);
    CHECK(Smc::swings(one).empty());
    CHECK(Smc::swings(one, 1).empty());
    CHECK(!Smc::analyze(one).valid);

    // Nothing but forming candles is nothing at all.
    Candles unclosed = quiet(70);
    for (Candle &candle : unclosed)
        candle.closed = false;
    CHECK(!Indicators::summarize(unclosed).valid);
    CHECK(Smc::swings(unclosed).empty());
    CHECK(!Smc::analyze(unclosed).valid);

    // Periods and strengths at the ends of int must not overflow anything.
    const Values some = {1, 2, 3, 4, 5};
    for (int extreme : {std::numeric_limits<int>::min(), std::numeric_limits<int>::max()}) {
        CHECK(allNaN(Indicators::sma(some, extreme)));
        CHECK(allNaN(Indicators::ema(some, extreme)));
        CHECK(allNaN(Indicators::rsi(some, extreme)));
        CHECK(allNaN(Indicators::macd(some, extreme, extreme, extreme).histogram));
        CHECK(allNaN(Indicators::bollinger(some, extreme).upper));
        CHECK(allNaN(Indicators::atr(quiet(5), extreme)));
        CHECK(Smc::swings(quiet(40), extreme).empty());
        const Smc::Analysis a = Smc::analyze(quiet(40), extreme);
        CHECK(a.valid);
        CHECK(a.swings.empty() && a.events.empty());
    }
}

// A fixed linear congruential generator (the Numerical Recipes constants):
// the same walk on every run and every machine.
struct Lcg
{
    std::uint32_t state;

    double next() // in [0, 1)
    {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) / 16777216.0;
    }
};

Candles randomWalk(int count, std::uint32_t seed)
{
    Lcg random{seed};
    Candles candles;
    double price = 1000;
    for (int i = 0; i < count; ++i) {
        const double open = price;
        const double close = open + (random.next() - 0.5) * 20;
        const double high = std::max(open, close) + random.next() * 3;
        const double low = std::min(open, close) - random.next() * 3;
        candles.push_back(bar(i, open, high, low, close, 100 + random.next() * 900));
        price = close;
    }
    return candles;
}

bool oneOf(const std::string &value, std::initializer_list<const char *> allowed)
{
    return std::any_of(allowed.begin(), allowed.end(), [&value](const char *item) { return value == item; });
}

void testWalkIndicators(const Candles &walk)
{
    const std::size_t size = walk.size();
    const Values prices = Indicators::closes(walk);

    // Each series: as long as the input, NaN up to its first index and
    // finite numbers from there on.
    const auto shaped = [size](const Values &series, std::size_t first) {
        return series.size() == size && leadingNaNs(series) == first && finiteFrom(series, first);
    };
    CHECK(shaped(prices, 0));
    CHECK(shaped(Indicators::sma(prices, 20), 19));
    CHECK(shaped(Indicators::ema(prices, 200), 199));
    CHECK(shaped(Indicators::rsi(prices), 14));
    const Indicators::Macd lines = Indicators::macd(prices);
    CHECK(shaped(lines.macd, 25));
    CHECK(shaped(lines.signal, 33));
    CHECK(shaped(lines.histogram, 33));
    const Indicators::Bands bands = Indicators::bollinger(prices);
    CHECK(shaped(bands.middle, 19));
    CHECK(shaped(bands.upper, 19));
    CHECK(shaped(bands.lower, 19));
    CHECK(shaped(Indicators::trueRange(walk), 0));
    CHECK(shaped(Indicators::atr(walk), 13));

    const Indicators::Summary s = Indicators::summarize(walk);
    CHECK(s.valid);
    for (double value : {s.price, s.ema20, s.ema50, s.ema200, s.rsi14, s.macd, s.macdSignal, s.macdHistogram,
                         s.macdHistogramPrevious, s.bandUpper, s.bandMiddle, s.bandLower, s.bandPosition,
                         s.bandWidthPct, s.atr14, s.atrPct, s.volume, s.volumeAverage20, s.volumeRatio,
                         s.change24Pct})
        CHECK(std::isfinite(value));
    EQUAL(s.price, walk.back().close);
    EQUAL(s.volume, walk.back().volume);
    CHECK(s.rsi14 >= 0 && s.rsi14 <= 100);
    CHECK(s.bandLower < s.bandMiddle && s.bandMiddle < s.bandUpper);
    CHECK(s.atr14 > 0 && s.volumeAverage20 > 0);
    CHECK(oneOf(s.trend, {"bullish", "bearish", "mixed"}));
    CHECK(oneOf(s.momentum, {"overbought", "oversold", "neutral"}));
    CHECK(oneOf(s.macdState, {"rising", "falling", "flat"}));
    CHECK(oneOf(s.volumeState, {"high", "low", "normal"}));
}

// The swing rule again, written from the header with the extremes of the two
// windows instead of candle-by-candle comparisons.
bool isSwing(const Candles &candles, int index, int strength, bool high)
{
    const int count = static_cast<int>(candles.size());
    if (index < strength || index + strength >= count)
        return false;
    const auto less = [high](const Candle &a, const Candle &b) { return high ? a.high < b.high : a.low > b.low; };
    const auto at = candles.begin() + index;
    const Candle &before = *std::max_element(at - strength, at, less);
    const Candle &after = *std::max_element(at + 1, at + strength + 1, less);
    return high ? at->high > before.high && at->high >= after.high : at->low < before.low && at->low <= after.low;
}

// True when a close in [from, to) lies beyond 'level' on the breaking side.
bool closedBeyond(const Candles &candles, int from, int to, double level, bool high)
{
    for (int i = from; i < to; ++i) {
        const double close = candles[static_cast<std::size_t>(i)].close;
        if (high ? close > level : close < level)
            return true;
    }
    return false;
}

void testWalkStructure(const Candles &walk)
{
    const int count = static_cast<int>(walk.size());
    const int strength = 3;
    const Smc::Analysis a = Smc::analyze(walk);
    CHECK(a.valid);
    const auto inRange = [count](int index) { return index >= 0 && index < count; };
    const auto candle = [&walk](int index) -> const Candle & { return walk[static_cast<std::size_t>(index)]; };

    // A walk this long has some of everything; otherwise the loops below
    // would check nothing.
    const bool populated = !a.swings.empty() && !a.events.empty() && !a.orderBlocks.empty() && !a.gaps.empty()
        && !a.sweeps.empty();
    CHECK(populated);
    if (!populated)
        return;

    // Swings: exactly the candles that pass the rule, in order.
    std::size_t expectedSwings = 0;
    for (int i = 0; i < count; ++i)
        expectedSwings += (isSwing(walk, i, strength, true) ? 1u : 0u) + (isSwing(walk, i, strength, false) ? 1u : 0u);
    EQUAL(a.swings.size(), expectedSwings);
    int previous = -1;
    for (const Smc::Swing &swing : a.swings) {
        CHECK(inRange(swing.index) && swing.index >= previous);
        if (!inRange(swing.index))
            return;
        previous = swing.index;
        CHECK(isSwing(walk, swing.index, strength, swing.high));
        EQUAL(swing.price, swing.high ? candle(swing.index).high : candle(swing.index).low);
        EQUAL(swing.time, candle(swing.index).openTime);
    }

    // Events: the first close beyond a known swing, in order, each swing at
    // most once; a change of character exactly when the direction turns.
    previous = -1;
    const Smc::StructureEvent *before = nullptr;
    for (const Smc::StructureEvent &event : a.events) {
        CHECK(inRange(event.index) && inRange(event.swingIndex) && event.index >= previous);
        if (!inRange(event.index) || !inRange(event.swingIndex))
            return;
        previous = event.index;
        CHECK(event.swingIndex + strength < event.index);
        CHECK(isSwing(walk, event.swingIndex, strength, event.bullish));
        EQUAL(event.level, event.bullish ? candle(event.swingIndex).high : candle(event.swingIndex).low);
        EQUAL(event.time, candle(event.index).openTime);
        CHECK(closedBeyond(walk, event.index, event.index + 1, event.level, event.bullish));
        CHECK(!closedBeyond(walk, event.swingIndex + strength + 1, event.index, event.level, event.bullish));
        EQUAL(event.changeOfCharacter, before != nullptr && before->bullish != event.bullish);
        before = &event;
    }
    EQUAL(name(a.trend), a.events.back().bullish ? "bullish" : "bearish");

    // Order blocks: a candle of the other colour, its whole range.
    for (const Smc::OrderBlock &block : a.orderBlocks) {
        CHECK(inRange(block.index));
        if (!inRange(block.index))
            return;
        const Candle &source = candle(block.index);
        CHECK(block.bullish ? source.close < source.open : source.close > source.open);
        EQUAL(block.top, source.high);
        EQUAL(block.bottom, source.low);
        EQUAL(block.time, source.openTime);
    }

    // Gaps: exactly the three-candle imbalances of at least 0.05 %.
    std::size_t expectedGaps = 0;
    for (int i = 1; i + 1 < count; ++i) {
        const double up = candle(i + 1).low - candle(i - 1).high;
        const double down = candle(i - 1).low - candle(i + 1).high;
        if (std::max(up, down) > 0 && std::max(up, down) >= candle(i).close * 0.05 / 100)
            ++expectedGaps;
    }
    EQUAL(a.gaps.size(), expectedGaps);
    for (const Smc::FairValueGap &gap : a.gaps) {
        CHECK(gap.index >= 1 && gap.index + 1 < count);
        if (gap.index < 1 || gap.index + 1 >= count)
            return;
        CHECK(gap.top > gap.bottom);
        EQUAL(gap.top, gap.bullish ? candle(gap.index + 1).low : candle(gap.index - 1).low);
        EQUAL(gap.bottom, gap.bullish ? candle(gap.index - 1).high : candle(gap.index + 1).high);
        EQUAL(gap.time, candle(gap.index).openTime);
        bool through = false;
        for (int i = gap.index + 2; i < count; ++i)
            through = through || (gap.bullish ? candle(i).low <= gap.bottom : candle(i).high >= gap.top);
        EQUAL(gap.filled, through);
    }

    // Sweeps: a wick beyond the level of a swing that was known and had not
    // been closed through, and a close back inside.
    previous = -1;
    for (const Smc::Sweep &sweep : a.sweeps) {
        CHECK(inRange(sweep.index) && sweep.index >= previous);
        if (!inRange(sweep.index))
            return;
        previous = sweep.index;
        const Candle &source = candle(sweep.index);
        EQUAL(sweep.time, source.openTime);
        CHECK(sweep.bullish ? source.low < sweep.level && source.close > sweep.level
                            : source.high > sweep.level && source.close < sweep.level);
        const bool unbrokenSwing = std::any_of(a.swings.begin(), a.swings.end(), [&](const Smc::Swing &swing) {
            return swing.high != sweep.bullish && swing.price == sweep.level
                && swing.index + strength < sweep.index
                && !closedBeyond(walk, swing.index + strength + 1, sweep.index, sweep.level, swing.high);
        });
        CHECK(unbrokenSwing);
    }

    for (double value : {a.rangeHigh, a.rangeLow, a.equilibrium, a.rangePosition})
        CHECK(std::isfinite(value));
    CHECK(a.rangeHigh > a.rangeLow);
    NEAR(a.equilibrium, (a.rangeHigh + a.rangeLow) / 2);
    NEAR(a.rangePosition, (walk.back().close - a.rangeLow) / (a.rangeHigh - a.rangeLow));
    CHECK(oneOf(a.zone, {"premium", "discount", "equilibrium"}));

    // A forming candle at the end changes nothing, whatever it holds.
    Candles longer = walk;
    longer.push_back(forming(bar(count, 5000, 9000, 1, 5000)));
    const Smc::Analysis same = Smc::analyze(longer);
    EQUAL(same.swings.size(), a.swings.size());
    EQUAL(same.events.size(), a.events.size());
    EQUAL(same.orderBlocks.size(), a.orderBlocks.size());
    EQUAL(same.gaps.size(), a.gaps.size());
    EQUAL(same.sweeps.size(), a.sweeps.size());
    EQUAL(same.rangePosition, a.rangePosition);
    EQUAL(Indicators::summarize(longer).price, walk.back().close);
}

void testRandomWalk()
{
    const Candles walk = randomWalk(500, 20260u);
    EQUAL(walk.size(), 500u);
    testWalkIndicators(walk);
    testWalkStructure(walk);
}

} // namespace

int main()
{
    std::cout << std::boolalpha << std::setprecision(15);
    testSma();
    testEma();
    testRsi();
    testMacd();
    testBollinger();
    testTrueRangeAndAtr();
    testCloses();
    testSummaryFlat();
    testSummaryRising();
    testSummaryFalling();
    testSummaryJump();
    testSummaryMixed();
    testSummaryMomentum();
    testSummaryVolume();
    testSummaryIrregular();
    testSummaryValidity();
    testSwings();
    testTrendName();
    testBullishBreak();
    testBearishBreak();
    testChangeOfCharacter();
    testSwingBrokenOnce();
    testOlderSwingBreaksNext();
    testOneCloseThroughTwoSwings();
    testOlderLowerSwing();
    testSwingStrengthAndTiming();
    testBullishOrderBlock();
    testBearishOrderBlock();
    testOrderBlockSearchLimits();
    testBullishGap();
    testBearishGap();
    testGapSizeFilter();
    testGapAtTheEdges();
    testBullishSweep();
    testBearishSweep();
    testSweepAge();
    testSweepNeedsWickAndClose();
    testSweepWickBecomesTheSwing();
    testSweepOfTwoSwings();
    testZones();
    testRangeLevels();
    testRangeStaysUpright();
    testAnalysisValidity();
    testFormingCandleIgnored();
    testEmptyAndTiny();
    testRandomWalk();
    std::cout << g_checks << " checks, " << g_failed << " failed\n";
    return g_failed ? 1 : 0;
}
