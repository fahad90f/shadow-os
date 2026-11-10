#!/bin/bash
# GUI smoke test: launch every NEXUS application headlessly inside the build
# chroot and fail if it cannot load its UI or the QML engine reports any
# warning or error.
#
#   tests/desktop/smoke.sh [--dir BUILD_DIR_NAME] [--shots] [component...]
#
# Applications opt in through a smoke-tests.list file next to their sources
# (one "<binary> [args...]" per line). With --shots a screenshot of each main
# window is saved to build/work/shots/smoke-<binary>.png for visual review.
#
# This test runs without system services, so it proves the UI loads and its
# "unavailable" states are sound — not that the back-ends work. Those are
# covered by tests/integration/ inside a booted VM.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DIR="dev"; SHOTS=0; ONLY=()
while [ $# -gt 0 ]; do
    case "$1" in
        --dir) DIR="$2"; shift 2 ;;
        --shots) SHOTS=1; shift ;;
        *) ONLY+=("$1"); shift ;;
    esac
done

pass=0; fail=0; failed=()
while IFS= read -r list; do
    comp="$(basename "$(dirname "$list")")"
    if [ "${#ONLY[@]}" -gt 0 ] && [[ ! " ${ONLY[*]} " == *" $comp "* ]]; then continue; fi
    while IFS= read -r line; do
        line="${line%%#*}"; [ -z "${line// }" ] && continue
        # shellcheck disable=SC2086
        set -- $line
        bin="$1"
        args=(--dir "$DIR" --smoke)
        [ "$SHOTS" -eq 1 ] && args+=(--shot "smoke-$bin.png")
        out="$("$ROOT/scripts/dev.sh" run "${args[@]}" "$@" 2>&1)"; rc=$?
        if [ $rc -eq 0 ] && grep -q "NEXUS-SMOKE-OK" <<<"$out"; then
            echo "PASS  $comp: $line"; pass=$((pass+1))
        else
            echo "FAIL  $comp: $line (exit $rc)"
            grep -E "QML-WARNING|QML-ERROR|NEXUS-|error|Segmentation|Aborted" <<<"$out" | head -15 | sed 's/^/      /'
            fail=$((fail+1)); failed+=("$comp:$bin")
        fi
    done < "$list"
done < <(find "$ROOT/applications" "$ROOT/desktop" "$ROOT/system" "$ROOT/arsenal" -name smoke-tests.list 2>/dev/null | sort)

echo "smoke: $pass passed, $fail failed${failed:+ (${failed[*]})}"
[ "$fail" -eq 0 ]
