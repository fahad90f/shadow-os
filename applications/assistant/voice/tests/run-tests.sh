#!/bin/bash
# Run the voice engine tests (Python unittest, no pytest needed).
#
#   tests/run-tests.sh                 everything (protocol tests need an installed data root)
#   tests/run-tests.sh -k Wake         only tests whose name matches (unittest -k)
#   tests/run-tests.sh test_wake       one module
#
# Environment: SHADOW_VOICED (helper to test, default ../shadow-voiced), SHADOW_VOICE_DATA
# (data root, default: the helper's lookup order), VOICE_TEST_TMP (fixtures; default a new
# temp dir, removed afterwards unless KEEP_TMP=1). Fixtures (spoken session WAVs) are
# synthesised with the engine's own voices at test time and never written into the repo.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
own_tmp=0
if [ -z "${VOICE_TEST_TMP:-}" ]; then
    VOICE_TEST_TMP=$(mktemp -d "${TMPDIR:-/tmp}/shadow-voice-tests.XXXXXX")
    export VOICE_TEST_TMP
    own_tmp=1
fi
cleanup() { if [ "$own_tmp" = 1 ] && [ "${KEEP_TMP:-0}" != 1 ]; then rm -rf -- "$VOICE_TEST_TMP"; fi; }
trap cleanup EXIT

args=()
pattern="test_*.py"
while [ $# -gt 0 ]; do
    case "$1" in
        -k) args+=(-k "$2"); shift ;;
        test_*) pattern="$1.py"; [[ "$1" == *.py ]] && pattern="$1" ;;
        *) args+=("$1") ;;
    esac
    shift
done
echo "voice tests: helper=${SHADOW_VOICED:-$here/../shadow-voiced} fixtures=$VOICE_TEST_TMP" >&2
cd "$here"
python3 -I -m unittest discover -s "$here" -t "$here" -p "$pattern" -v "${args[@]}"
