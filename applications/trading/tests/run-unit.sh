#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# run-unit.sh skills|core
#
# Compiles and runs one of Shadow Trading's unit test programs: the analysis
# skills (indicators, patterns) or the decision core (consensus, risk rules,
# paper account). Plain C++17, no Qt: a compiler is all it needs. Built with
# the address and undefined-behaviour sanitizers where the compiler has them.
# Exit status: the test program's; 77 (skipped) when there is no C++ compiler.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
case "${1:-}" in
skills) sources=(skills/indicators.cpp skills/smc.cpp tests/test_skills.cpp) ;;
core)   sources=(core/signal.cpp core/risk.cpp core/paper.cpp tests/test_core.cpp) ;;
*) echo "usage: $0 skills|core" >&2; exit 2 ;;
esac
cxx="${CXX:-}"
[ -n "$cxx" ] || cxx="$(command -v g++ || command -v clang++ || true)"
[ -n "$cxx" ] || { echo "SKIP: no C++ compiler (g++ or clang++)"; exit 77; }
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
cd "$here"
flags=(-std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Wshadow -Werror)
# The sanitizers need their runtime libraries; without them, build plainly.
if ! "$cxx" "${flags[@]}" -fsanitize=address,undefined "${sources[@]}" -o "$out/test" 2>"$out/log"; then
    if grep -q -E 'cannot find|sanitize|asan|ubsan' "$out/log"; then
        "$cxx" "${flags[@]}" "${sources[@]}" -o "$out/test"
    else
        cat "$out/log" >&2
        exit 1
    fi
fi
"$out/test"
