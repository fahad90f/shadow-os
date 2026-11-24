// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "candle.h"

#include <string>
#include <vector>

// The "TA Indicators" skill: classic technical indicators computed from a
// candle series. Every function returns a series as long as its input; a
// position that cannot be computed yet (not enough history) holds NaN.
// Nothing here predicts anything: these are measurements of past prices.
namespace Trading::Indicators {

// Simple moving average over 'period' values.
std::vector<double> sma(const std::vector<double> &values, int period);

// Exponential moving average, smoothing 2 / (period + 1), seeded with the
// simple average of the first 'period' values (so index period-1 is the first
// number).
std::vector<double> ema(const std::vector<double> &values, int period);

// Wilder's relative strength index, 0..100. The first value is at index
// 'period' (it needs 'period' price changes); the averages are seeded with the
// simple mean of the first 'period' gains and losses and then smoothed with
// Wilder's method. A window with no losses gives 100, one with no gains 0, a
// completely flat one 50.
std::vector<double> rsi(const std::vector<double> &closes, int period = 14);

struct Macd
{
    std::vector<double> macd;      // ema(fast) - ema(slow)
    std::vector<double> signal;    // ema(signalPeriod) of the macd line
    std::vector<double> histogram; // macd - signal
};
Macd macd(const std::vector<double> &closes, int fast = 12, int slow = 26, int signalPeriod = 9);

struct Bands
{
    std::vector<double> middle; // sma(period)
    std::vector<double> upper;  // middle + k * population standard deviation
    std::vector<double> lower;  // middle - k * population standard deviation
};
Bands bollinger(const std::vector<double> &closes, int period = 20, double k = 2.0);

// True range: max(high - low, |high - previous close|, |low - previous
// close|); the first candle has only high - low.
std::vector<double> trueRange(const Candles &candles);

// Wilder's average true range. The first value is at index period-1 (the
// simple mean of the first 'period' true ranges), then
// atr = (previous * (period - 1) + tr) / period.
std::vector<double> atr(const Candles &candles, int period = 14);

std::vector<double> closes(const Candles &candles);

// The latest reading of everything, for the analysts and the UI. Computed
// from CLOSED candles only (a forming last candle is ignored). 'valid' is
// false, and 'reason' says why, when there are fewer than 60 closed candles.
struct Summary
{
    bool valid = false;
    std::string reason;

    double price = 0; // close of the last closed candle
    double ema20 = 0, ema50 = 0, ema200 = 0; // ema200 is NaN with < 200 candles
    double rsi14 = 0;
    double macd = 0, macdSignal = 0, macdHistogram = 0;
    double macdHistogramPrevious = 0;
    double bandUpper = 0, bandMiddle = 0, bandLower = 0;
    double bandPosition = 0;  // 0 = at the lower band, 1 = at the upper band
    double bandWidthPct = 0;  // (upper - lower) / middle * 100
    double atr14 = 0;
    double atrPct = 0;        // atr14 / price * 100
    double volume = 0;        // of the last closed candle
    double volumeAverage20 = 0;
    double volumeRatio = 0;   // volume / volumeAverage20
    double change24Pct = 0;   // close vs the close 24 candles back (not 24 hours)

    // Derived states, each one a plain reading of the numbers above:
    // "bullish" when price > ema20 > ema50, "bearish" when price < ema20 <
    // ema50, otherwise "mixed".
    std::string trend;
    // "overbought" (rsi >= 70), "oversold" (rsi <= 30), otherwise "neutral".
    std::string momentum;
    // "rising" when the histogram is above its previous value, "falling" when
    // below, "flat" when equal.
    std::string macdState;
    // "high" when volumeRatio >= 1.5, "low" when <= 0.5, otherwise "normal".
    std::string volumeState;
};
Summary summarize(const Candles &candles);

} // namespace Trading::Indicators
