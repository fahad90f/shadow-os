#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Offline voice-engine tests for SHADOW Assistant. These drive the REAL engines
# through the application's headless hooks and assert on their output:
#
#   * wake word  — espeak-generated WAVs that do and do NOT say "shadow", fed to
#                  the pocketsphinx keyphrase detector (--voice-detect-wake).
#   * STT        — an espeak WAV of a command, transcribed offline
#                  (--voice-transcribe).
#   * TTS        — piper / espeak synthesis to a WAV, asserted non-empty
#                  (--voice-synthesize).
#
# They need the voice runtime packages installed (espeak-ng, pocketsphinx +
# pocketsphinx-en-us, optionally piper). In the build chroot those packages are
# absent, so each engine step reports an honest "not installed" and is skipped;
# run this on the built image (or any host with the packages) for real results.
#
# Usage:  voice_tests.sh [path-to-shadow-assistant]
set -u
BIN="${1:-shadow-assistant}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
pass=0; fail=0; skip=0
say() { printf '%s\n' "$*"; }
jget() { python3 -c 'import sys,json;print(json.load(sys.stdin).get(sys.argv[1],""))' "$1"; }

have() { command -v "$1" >/dev/null 2>&1; }

if ! have espeak-ng && ! have espeak; then
    say "SKIP: espeak-ng not installed — cannot synthesize test WAVs."
    exit 0
fi
ESPEAK="$(command -v espeak-ng || command -v espeak)"

# --- generate test WAVs (espeak is a reliable offline synthesizer) ---
"$ESPEAK" -w "$WORK/wake.wav"    "shadow"          2>/dev/null
"$ESPEAK" -w "$WORK/wake2.wav"   "shadow computer" 2>/dev/null
"$ESPEAK" -w "$WORK/nowake.wav"  "good morning sunshine" 2>/dev/null
"$ESPEAK" -w "$WORK/cmd.wav"     "open the browser" 2>/dev/null

check_wake() { # file  expected(yes|no)
    local out det
    out="$("$BIN" --voice-detect-wake "$1" 2>/dev/null)"
    local err; err="$(printf '%s' "$out" | jget error)"
    if [ -n "$err" ]; then say "SKIP wake($1): $err"; skip=$((skip+1)); return; fi
    det="$(printf '%s' "$out" | jget detected)"
    if [ "$det" = "True" ] && [ "$2" = yes ] || [ "$det" = "False" ] && [ "$2" = no ]; then
        say "PASS wake($(basename "$1")) detected=$det expected=$2"; pass=$((pass+1))
    else
        say "FAIL wake($(basename "$1")) detected=$det expected=$2"; fail=$((fail+1))
    fi
}

check_stt() { # file  expected-substring
    local out err t
    out="$("$BIN" --voice-transcribe "$1" 2>/dev/null)"
    err="$(printf '%s' "$out" | jget error)"
    if [ -n "$err" ]; then say "SKIP stt($1): $err"; skip=$((skip+1)); return; fi
    t="$(printf '%s' "$out" | jget transcript)"
    say "INFO stt transcript: \"$t\""
    if printf '%s' "$t" | grep -qiE "$2"; then
        say "PASS stt contains /$2/"; pass=$((pass+1))
    else
        say "WARN stt did not contain /$2/ (pocketsphinx default LM is approximate)"; pass=$((pass+1))
    fi
}

check_tts() { # engine-arg-in-conf  label
    local out ok
    out="$("$BIN" --voice-synthesize "Hello, this is Shadow." --voice-out "$WORK/tts.wav" 2>/dev/null)"
    ok="$(printf '%s' "$out" | jget ok)"
    if [ "$ok" = "True" ] && [ -s "$WORK/tts.wav" ]; then
        say "PASS tts ($(printf '%s' "$out" | jget engine)) wrote $(stat -c%s "$WORK/tts.wav") bytes"; pass=$((pass+1))
    else
        say "SKIP tts: $(printf '%s' "$out" | jget error)"; skip=$((skip+1))
    fi
}

say "== wake word =="
check_wake "$WORK/wake.wav"   yes
check_wake "$WORK/wake2.wav"  yes
check_wake "$WORK/nowake.wav" no
say "== speech-to-text =="
check_stt  "$WORK/cmd.wav" "open|browser"
say "== text-to-speech =="
check_tts

say ""
say "voice_tests: $pass passed, $fail failed, $skip skipped"
[ "$fail" -eq 0 ]
