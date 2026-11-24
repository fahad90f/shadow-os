// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vector>

// The analysis skills (indicators.h, smc.h) are plain C++17 with no Qt, so
// they can be tested anywhere with nothing but a compiler.
namespace Trading {

// One candle of a price series. Series are ordered oldest first.
struct Candle
{
    long long openTime = 0; // ms since the epoch, start of the candle
    double open = 0;
    double high = 0;
    double low = 0;
    double close = 0;
    double volume = 0;
    bool closed = true; // false for the candle that is still forming
};

using Candles = std::vector<Candle>;

} // namespace Trading
