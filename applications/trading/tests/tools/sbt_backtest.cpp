// SPDX-License-Identifier: GPL-3.0-or-later
//
// sbt_backtest FILE.csv [name=value ...]
//
// Runs the SBT Model 1 engine (core/sbt.cpp) over a file of closed candles,
// oldest first, one per line: time_ms,open,high,low,close[,volume]. Candle by
// candle, as it would have seen them: nothing is decided from later candles.
// Prints what it set up, what it entered and how the entries ended. The
// rules are the engine's defaults unless given as name=value: leg_atr,
// min_pullback_candles, extension_atr, entry_zone_atr, first_return,
// supersede, target (body|wick), zone_move_atr, gap_atr, gap_pause_candles,
// stop_buffer_atr, min_reward_risk, max_wait_candles.
//
// This measures the rules on the past. It says nothing about the future,
// and the result depends on the stop and target far more than on the
// pattern: a trade with reward:risk r wins about 1/(1+r) of the time in a
// market that drifts nowhere ("coin flip" below).
#include "../../core/sbt.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace Trading;
using namespace Trading::Sbt;

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s FILE.csv [name=value ...]\n", argv[0]);
        return 2;
    }
    Settings s;
    s.keepFinished = 1000000;
    bool lines = false;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        const size_t eq = arg.find('=');
        const std::string name = arg.substr(0, eq), value = eq == std::string::npos ? "" : arg.substr(eq + 1);
        const double number = std::atof(value.c_str());
        if (name == "leg_atr") s.legAtr = number;
        else if (name == "min_pullback_candles") s.minPullbackCandles = int(number);
        else if (name == "extension_atr") s.extensionAtr = number;
        else if (name == "entry_zone_atr") s.entryZoneAtr = number;
        else if (name == "first_return") s.firstReturn = number != 0;
        else if (name == "supersede") s.supersede = number != 0;
        else if (name == "target") s.targetBody = value != "wick";
        else if (name == "zone_move_atr") s.zoneMoveAtr = number;
        else if (name == "gap_atr") s.gapAtr = number;
        else if (name == "gap_pause_candles") s.gapPauseCandles = int(number);
        else if (name == "stop_buffer_atr") s.stopBufferAtr = number;
        else if (name == "min_reward_risk") s.minRewardRisk = number;
        else if (name == "max_wait_candles") s.maxArmedCandles = int(number);
        else if (name == "--setups") lines = true;
        else {
            std::fprintf(stderr, "unknown setting '%s'\n", name.c_str());
            return 2;
        }
    }
    std::ifstream in(argv[1]);
    if (!in) {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    Engine engine(s);
    engine.setLive(false);
    std::string line;
    long long candles = 0, first = 0, last = 0;
    while (std::getline(in, line)) {
        Candle c;
        char comma = 0;
        double t = 0;
        std::stringstream ss(line);
        if (!(ss >> t >> comma >> c.open >> comma >> c.high >> comma >> c.low >> comma >> c.close))
            continue;
        c.openTime = static_cast<long long>(t);
        c.closed = true;
        if (candles == 0)
            first = c.openTime;
        last = c.openTime;
        ++candles;
        engine.close(c);
    }
    int won = 0, lost = 0, open = 0;
    double total = 0, coin = 0, rrSum = 0;
    std::map<std::string, int> left;
    for (const Setup &x : engine.setups()) {
        if (lines)
            std::printf("%s\t%s\t%lld\tentry %.5f\tstop %.5f\ttarget %.5f\trr %.2f\t%s\n", directionName(x.direction),
                        statusName(x.status), x.entryTime, x.entry, x.stop, x.target, x.rewardRisk, x.note.c_str());
        if (x.status == Status::Target || x.status == Status::Stopped) {
            const bool win = x.status == Status::Target;
            (win ? won : lost) += 1;
            total += win ? x.rewardRisk : -1.0;
            coin += 1.0 / (1.0 + x.rewardRisk);
            rrSum += x.rewardRisk;
        } else if (x.status == Status::Entered) {
            ++open;
        } else if (x.status != Status::Armed && x.status != Status::Triggered) {
            // Why it was left out: the note up to its first number or bracket.
            std::string why = x.note.substr(0, x.note.find_first_of("(0123456789"));
            while (!why.empty() && (why.back() == ' ' || why.back() == ':'))
                why.pop_back();
            left[why] += 1;
        }
    }
    const int closed = won + lost;
    std::printf("%lld candles, %.0f days\n", candles, double(last - first) / 86400000.0);
    std::printf("structures (a close beyond a confirmed high or low): %zu\n", engine.setups().size());
    std::printf("entries: %d   won (target, the area of 3): %d   lost (stop): %d   still open: %d\n", closed, won, lost, open);
    if (closed > 0) {
        std::printf("win rate: %.1f %%   coin flip at these reward:risk ratios: %.1f %%\n", 100.0 * won / closed,
                    100.0 * coin / closed);
        std::printf("average reward:risk: %.2f   result: %+.1f R in total, %+.2f R per trade\n", rrSum / closed, total,
                    total / closed);
    }
    std::printf("left out:\n");
    for (const auto &entry : left)
        std::printf("  %5d  %s\n", entry.second, entry.first.c_str());
    return 0;
}
