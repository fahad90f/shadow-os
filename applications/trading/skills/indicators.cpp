// SPDX-License-Identifier: GPL-3.0-or-later
#include "indicators.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace Trading::Indicators {

namespace {

// summarize() reads the histogram one candle back and the close 24 candles
// back, and the header's limit of 60 closed candles covers both, as well as
// every default period except the 200 of the long average.
constexpr std::size_t kMinimumCandles = 60;
constexpr std::size_t kChangeLookback = 24;

std::vector<double> allNaN(std::size_t size)
{
    return std::vector<double>(size, std::numeric_limits<double>::quiet_NaN());
}

// The candles to read: everything but a forming tail. Only the end of a
// series can still be forming, so dropping from the back keeps positions.
std::size_t closedCount(const Candles &candles)
{
    std::size_t count = candles.size();
    while (count > 0 && !candles[count - 1].closed)
        --count;
    return count;
}

// The exponential average of values[first..]. 'first' exists for the MACD
// signal line, which smooths a series that starts with NaN.
std::vector<double> emaFrom(const std::vector<double> &values, int period, std::size_t first)
{
    std::vector<double> out = allNaN(values.size());
    if (period <= 0)
        return out;
    const std::size_t window = static_cast<std::size_t>(period);
    if (values.size() < first || values.size() - first < window)
        return out;

    double value = 0;
    for (std::size_t i = first; i < first + window; ++i)
        value += values[i];
    value /= period;
    out[first + window - 1] = value;

    const double smoothing = 2.0 / (period + 1.0);
    for (std::size_t i = first + window; i < values.size(); ++i) {
        // A step towards the new value rather than a weighted sum of the two:
        // when the input equals the average the step is exactly zero, so a
        // flat market reads as flat and not as rounding noise.
        value += smoothing * (values[i] - value);
        out[i] = value;
    }
    return out;
}

double rsiValue(double averageGain, double averageLoss)
{
    // Without losses the ratio gain / loss has no value; the header gives
    // the two cases their own readings.
    if (averageLoss == 0)
        return averageGain == 0 ? 50.0 : 100.0;
    return 100.0 - 100.0 / (1.0 + averageGain / averageLoss);
}

// A division whose divisor can be zero on flat or empty data (a band with no
// width, a market with no volume): 'otherwise' keeps inf and NaN out of the
// summary.
double divided(double numerator, double denominator, double otherwise)
{
    return denominator == 0 ? otherwise : numerator / denominator;
}

} // namespace

std::vector<double> sma(const std::vector<double> &values, int period)
{
    std::vector<double> out = allNaN(values.size());
    if (period <= 0)
        return out;
    const std::size_t window = static_cast<std::size_t>(period);
    for (std::size_t i = window - 1; i < values.size(); ++i) {
        // Every window is summed afresh. A running sum would be cheaper, but
        // it drifts over a long series and one NaN would spoil all that
        // follows; the windows here are a few hundred values at most.
        double sum = 0;
        for (std::size_t j = i + 1 - window; j <= i; ++j)
            sum += values[j];
        out[i] = sum / period;
    }
    return out;
}

std::vector<double> ema(const std::vector<double> &values, int period)
{
    return emaFrom(values, period, 0);
}

std::vector<double> rsi(const std::vector<double> &closes, int period)
{
    std::vector<double> out = allNaN(closes.size());
    if (period <= 0)
        return out;
    const std::size_t window = static_cast<std::size_t>(period);
    if (closes.size() <= window)
        return out;

    double averageGain = 0;
    double averageLoss = 0;
    for (std::size_t i = 1; i <= window; ++i) {
        const double change = closes[i] - closes[i - 1];
        averageGain += std::max(change, 0.0);
        averageLoss += std::max(-change, 0.0);
    }
    averageGain /= period;
    averageLoss /= period;
    out[window] = rsiValue(averageGain, averageLoss);

    for (std::size_t i = window + 1; i < closes.size(); ++i) {
        const double change = closes[i] - closes[i - 1];
        averageGain = (averageGain * (period - 1) + std::max(change, 0.0)) / period;
        averageLoss = (averageLoss * (period - 1) + std::max(-change, 0.0)) / period;
        out[i] = rsiValue(averageGain, averageLoss);
    }
    return out;
}

Macd macd(const std::vector<double> &closes, int fast, int slow, int signalPeriod)
{
    const std::size_t size = closes.size();
    Macd out{allNaN(size), allNaN(size), allNaN(size)};
    if (fast <= 0 || slow <= 0 || signalPeriod <= 0)
        return out;

    const std::vector<double> fastAverage = ema(closes, fast);
    const std::vector<double> slowAverage = ema(closes, slow);
    // NaN minus anything is NaN, so the positions where either average or
    // the signal line is still missing stay NaN without being singled out.
    for (std::size_t i = 0; i < size; ++i)
        out.macd[i] = fastAverage[i] - slowAverage[i];
    // The line starts where the slower of the two averages does, and the
    // signal line must be seeded from there, not from the NaN before it.
    out.signal = emaFrom(out.macd, signalPeriod, static_cast<std::size_t>(std::max(fast, slow)) - 1);
    for (std::size_t i = 0; i < size; ++i)
        out.histogram[i] = out.macd[i] - out.signal[i];
    return out;
}

Bands bollinger(const std::vector<double> &closes, int period, double k)
{
    const std::size_t size = closes.size();
    Bands out{sma(closes, period), allNaN(size), allNaN(size)};
    if (period <= 0)
        return out;
    const std::size_t window = static_cast<std::size_t>(period);
    for (std::size_t i = window - 1; i < size; ++i) {
        const double mean = out.middle[i];
        double squares = 0;
        for (std::size_t j = i + 1 - window; j <= i; ++j)
            squares += (closes[j] - mean) * (closes[j] - mean);
        // Divided by N, not N - 1: the bands describe these 'period' closes
        // themselves, they do not estimate the spread of a larger population.
        const double deviation = std::sqrt(squares / period);
        out.upper[i] = mean + k * deviation;
        out.lower[i] = mean - k * deviation;
    }
    return out;
}

std::vector<double> trueRange(const Candles &candles)
{
    std::vector<double> out(candles.size());
    for (std::size_t i = 0; i < candles.size(); ++i) {
        const Candle &candle = candles[i];
        out[i] = candle.high - candle.low;
        if (i > 0) {
            // A gap between candles is movement too: measure from the
            // previous close when it lies outside this candle's own range.
            const double previousClose = candles[i - 1].close;
            out[i] = std::max({out[i], std::abs(candle.high - previousClose), std::abs(candle.low - previousClose)});
        }
    }
    return out;
}

std::vector<double> atr(const Candles &candles, int period)
{
    const std::vector<double> ranges = trueRange(candles);
    std::vector<double> out = allNaN(ranges.size());
    if (period <= 0)
        return out;
    const std::size_t window = static_cast<std::size_t>(period);
    if (ranges.size() < window)
        return out;

    double value = 0;
    for (std::size_t i = 0; i < window; ++i)
        value += ranges[i];
    value /= period;
    out[window - 1] = value;

    for (std::size_t i = window; i < ranges.size(); ++i) {
        value = (value * (period - 1) + ranges[i]) / period;
        out[i] = value;
    }
    return out;
}

std::vector<double> closes(const Candles &candles)
{
    std::vector<double> out;
    out.reserve(candles.size());
    for (const Candle &candle : candles)
        out.push_back(candle.close);
    return out;
}

Summary summarize(const Candles &candles)
{
    Summary summary;
    const Candles closed(candles.begin(), candles.begin() + static_cast<std::ptrdiff_t>(closedCount(candles)));
    if (closed.size() < kMinimumCandles) {
        summary.reason = "Need at least " + std::to_string(kMinimumCandles) + " closed candles, have "
            + std::to_string(closed.size()) + ".";
        return summary;
    }
    summary.valid = true;

    const std::size_t last = closed.size() - 1;
    const std::vector<double> prices = closes(closed);
    summary.price = prices[last];
    summary.ema20 = ema(prices, 20)[last];
    summary.ema50 = ema(prices, 50)[last];
    summary.ema200 = ema(prices, 200)[last];
    summary.rsi14 = rsi(prices, 14)[last];

    const Macd lines = macd(prices);
    summary.macd = lines.macd[last];
    summary.macdSignal = lines.signal[last];
    summary.macdHistogram = lines.histogram[last];
    summary.macdHistogramPrevious = lines.histogram[last - 1];

    const Bands bands = bollinger(prices);
    summary.bandUpper = bands.upper[last];
    summary.bandMiddle = bands.middle[last];
    summary.bandLower = bands.lower[last];
    const double bandWidth = summary.bandUpper - summary.bandLower;
    // With no width the price sits on all three lines at once: the middle.
    summary.bandPosition = divided(summary.price - summary.bandLower, bandWidth, 0.5);
    summary.bandWidthPct = divided(bandWidth, summary.bandMiddle, 0) * 100.0;

    summary.atr14 = atr(closed, 14)[last];
    summary.atrPct = divided(summary.atr14, summary.price, 0) * 100.0;

    std::vector<double> volumes;
    volumes.reserve(closed.size());
    for (const Candle &candle : closed)
        volumes.push_back(candle.volume);
    summary.volume = volumes[last];
    summary.volumeAverage20 = sma(volumes, 20)[last];
    // No volume at all in the window: the last candle is exactly average.
    summary.volumeRatio = divided(summary.volume, summary.volumeAverage20, 1.0);

    const double earlier = prices[last - kChangeLookback];
    summary.change24Pct = divided(summary.price - earlier, earlier, 0) * 100.0;

    if (summary.price > summary.ema20 && summary.ema20 > summary.ema50)
        summary.trend = "bullish";
    else if (summary.price < summary.ema20 && summary.ema20 < summary.ema50)
        summary.trend = "bearish";
    else
        summary.trend = "mixed";

    if (summary.rsi14 >= 70)
        summary.momentum = "overbought";
    else if (summary.rsi14 <= 30)
        summary.momentum = "oversold";
    else
        summary.momentum = "neutral";

    if (summary.macdHistogram > summary.macdHistogramPrevious)
        summary.macdState = "rising";
    else if (summary.macdHistogram < summary.macdHistogramPrevious)
        summary.macdState = "falling";
    else
        summary.macdState = "flat";

    if (summary.volumeRatio >= 1.5)
        summary.volumeState = "high";
    else if (summary.volumeRatio <= 0.5)
        summary.volumeState = "low";
    else
        summary.volumeState = "normal";

    return summary;
}

} // namespace Trading::Indicators
