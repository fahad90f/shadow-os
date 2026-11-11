#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs nexus-monitor for a while and reports what it costs itself: CPU time
# (user + system, from /proc/<pid>/stat), resident memory, thread and file
# descriptor counts, every 30 seconds.
#
#   sudo scripts/dev.sh exec --dir monitor -- \
#        bash /src/applications/monitor/tests/soak.sh [seconds=300] [page=overview] [/shots/name.png]
#
# ("dev.sh run" ends every program after 60 seconds, so this script is started
# with "dev.sh exec" and sets up the same environment itself: a virtual X
# server, a session bus, software GL and the build tree /build/monitor, or
# the one named in NEXUS_BUILD_DIR.)
#
# Fails if resident memory or the number of file descriptors keeps growing
# over the second half of the run (a leak), or if QML warnings were logged.
# The CPU figure is printed, not judged: under the software renderer of the
# test environment it mostly measures llvmpipe.
#
# shellcheck disable=SC2317  # the EXIT trap is invoked indirectly
set -u

SECONDS_TOTAL="${1:-300}"
PAGE="${2:-overview}"
SHOT="${3:-}"
if [ -z "${DISPLAY:-}" ]; then
    B="${NEXUS_BUILD_DIR:-/build/monitor}"
    HOME="$(mktemp -d /tmp/nexus-soak-home.XXXXXX)"
    export HOME XDG_RUNTIME_DIR="$HOME/run"
    mkdir -m 700 -p "$XDG_RUNTIME_DIR"; mkdir -p "$HOME/.config/nexus"
    export QML_IMPORT_PATH="$B/qml" LD_LIBRARY_PATH="$B/lib"
    export QT_QPA_PLATFORM=xcb LIBGL_ALWAYS_SOFTWARE=1 QT_QUICK_CONTROLS_STYLE=org.nexus.style
    export QT_LOGGING_RULES="qt.qpa.*=false"
    xvfb-run -a -s "-screen 0 1920x1200x24 -nolisten tcp" dbus-run-session -- bash "$0" "$@"
    status=$?
    rm -rf "$HOME"
    exit "$status"
fi
B="${LD_LIBRARY_PATH%%/lib*}"
APP="$B/bin/nexus-monitor"
[ -x "$APP" ] || { echo "soak: $APP not found (run through scripts/dev.sh run)"; exit 2; }
export NEXUS_WINDOW_SIZE="${NEXUS_WINDOW_SIZE:-1280x860}"
LOG=/tmp/soak-$$.log

"$APP" --page "$PAGE" > "$LOG" 2>&1 &
PID=$!
trap 'kill -9 "$PID" 2>/dev/null; rm -f "$LOG"' EXIT
xdotool search --sync --onlyvisible --name "System Monitor" > /dev/null
sleep 5

TICK=$(getconf CLK_TCK)
cpu_ticks() { awk '{print $14 + $15}' "/proc/$PID/stat"; }
rss_kb() { awk '/^VmRSS/ {print $2}' "/proc/$PID/status"; }
fds() { find "/proc/$PID/fd" -mindepth 1 | wc -l; }
threads() { awk '/^Threads/ {print $2}' "/proc/$PID/status"; }

echo "page: $PAGE   window: $NEXUS_WINDOW_SIZE   interval: 30 s   cpus: $(nproc)"
printf '%8s %10s %10s %6s %8s\n' "time[s]" "cpu[%]" "rss[MiB]" "fds" "threads"
start=$(date +%s); prev=$(cpu_ticks); RSS=(); FDS=()
while :; do
    sleep 30
    kill -0 "$PID" 2>/dev/null || { echo "soak: the monitor exited"; cat "$LOG"; exit 1; }
    now=$(cpu_ticks); r=$(rss_kb); f=$(fds)
    RSS+=("$r"); FDS+=("$f")
    printf '%8d %10s %10s %6d %8d\n' $(( $(date +%s) - start )) \
        "$(awk -v d=$((now - prev)) -v t="$TICK" 'BEGIN {printf "%.1f", d / t / 30 * 100}')" \
        "$(awk -v r="$r" 'BEGIN {printf "%.1f", r / 1024}')" "$f" "$(threads)"
    prev=$now
    [ $(( $(date +%s) - start )) -ge "$SECONDS_TOTAL" ] && break
done
echo "CPU time by thread over the whole run [s]:"
for task in /proc/"$PID"/task/*; do
    printf '  %-24s %s\n' "$(cat "$task/comm")" "$(awk -v t="$TICK" '{printf "%.1f", ($14 + $15) / t}' "$task/stat")"
done | sort -k2 -n -r | head -6
[ -n "$SHOT" ] && import -window root -crop "${NEXUS_WINDOW_SIZE}+0+0" +repage "$SHOT" 2>/dev/null

FAILED=0
n=${#RSS[@]}; half=$(( n / 2 ))
if [ "$n" -ge 4 ]; then
    growth=$(( RSS[n-1] - RSS[half] ))
    echo "resident memory over the second half: $growth kB"
    # More than 4 MiB of growth after the histories have filled is a leak.
    [ "$growth" -gt 4096 ] && { echo "  FAIL  resident memory keeps growing"; FAILED=1; }
    [ "${FDS[n-1]}" -gt $(( FDS[half] + 4 )) ] && { echo "  FAIL  file descriptors keep growing"; FAILED=1; }
fi
if grep -q "qml\|QML\|TypeError\|ReferenceError" "$LOG"; then
    echo "  FAIL  the application logged QML problems:"; sed 's/^/        /' "$LOG" | head -20; FAILED=1
fi
[ "$FAILED" = 0 ] && echo "soak $PAGE: passed" || echo "soak $PAGE: FAILED"
exit "$FAILED"
