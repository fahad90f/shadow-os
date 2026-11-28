#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# run-backtest.sh FILE.csv [name=value ...]
#
# Compiles tests/tools/sbt_backtest.cpp against the strategy engine and runs
# it over a file of candles (time_ms,open,high,low,close). See the tool's
# header for the settings; tests/tools/fetch_klines.py fetches such a file.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[ $# -ge 1 ] || { sed -n '3,9p' "$0"; exit 2; }
cxx="${CXX:-$(command -v g++ || command -v clang++ || true)}"
[ -n "$cxx" ] || { echo "no C++ compiler (g++ or clang++)" >&2; exit 77; }
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
"$cxx" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Wshadow -Werror "$here/tests/tools/sbt_backtest.cpp" "$here/core/sbt.cpp" -o "$out/backtest"
"$out/backtest" "$@"
