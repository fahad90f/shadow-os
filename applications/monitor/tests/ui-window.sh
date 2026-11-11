#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Two behaviours of the monitor's window, checked on the running program
# under the virtual X server of "scripts/dev.sh run":
#
#   sudo scripts/dev.sh run --dir monitor bash /src/applications/monitor/tests/ui-window.sh
#
#   sampling stops while the window is not on screen and resumes with it —
#   measured as scheduler time slices of the sampling threads
#   (/proc/<pid>/task/<tid>/schedstat) while the window is mapped, unmapped
#   and mapped again
#   the "System Information" button starts nexus-sysinfo (needs it built into
#   the same build directory: dev.sh build --dir monitor --only "monitor;sysinfo")
#
# Needs xdotool (development tools of the build chroot).
#
# shellcheck disable=SC2317  # helpers are called through check() and the EXIT trap
set -u

B="${LD_LIBRARY_PATH%%/lib*}"
APP="$B/bin/nexus-monitor"
[ -x "$APP" ] || { echo "ui-window: $APP not found (run through scripts/dev.sh run)"; exit 2; }
export NEXUS_WINDOW_SIZE=1240x1300
FAILED=0

check() {
    local what="$1"; shift
    if "$@"; then echo "  ok    $what"; else echo "  FAIL  $what"; FAILED=1; fi
}
running() { pgrep -x "$1" > /dev/null; }
# Sum of the time slices the sampling threads have run so far.
slices() {
    local total=0 task
    for task in /proc/"$APPPID"/task/*; do
        case "$(cat "$task/comm" 2>/dev/null)" in
            nexus-monitor-s*|nexus-monitor-p*) total=$((total + $(awk '{print $3}' "$task/schedstat"))) ;;
        esac
    done
    echo "$total"
}

"$APP" > /tmp/ui-window.log 2>&1 &
APPPID=$!
trap 'kill -9 "$APPPID" 2>/dev/null; pkill -9 -x nexus-sysinfo 2>/dev/null; rm -f /tmp/ui-window.log' EXIT
WID=$(xdotool search --sync --onlyvisible --name "System Monitor" | head -1)
xdotool windowfocus --sync "$WID"
xdotool mousemove 1200 20
sleep 3

a=$(slices); sleep 4; b=$(slices)
echo "  visible: $((b - a)) time slices in 4 s"
check "the sampling threads run while the window is visible" test $((b - a)) -ge 3

xdotool windowunmap "$WID"; sleep 1.5
a=$(slices); sleep 4; b=$(slices)
echo "  hidden:  $((b - a)) time slices in 4 s"
check "nothing is sampled while the window is hidden" test $((b - a)) -eq 0

xdotool windowmap "$WID"; sleep 1.5
xdotool windowfocus --sync "$WID"
a=$(slices); sleep 4; b=$(slices)
echo "  visible: $((b - a)) time slices in 4 s"
check "sampling resumes when the window is shown again" test $((b - a)) -ge 3

if [ -x "$B/bin/nexus-sysinfo" ]; then
    xdotool mousemove 1123 977 click 1; sleep 3     # "System Information"
    import -window root -crop 1240x1300+0+0 +repage /shots/ui-window-sysinfo.png 2>/dev/null
    check "the System Information button started nexus-sysinfo" running nexus-sysinfo
else
    echo "  skip  nexus-sysinfo is not built into $B/bin"
fi

if grep -q "qml\|QML\|TypeError\|ReferenceError" /tmp/ui-window.log; then
    echo "  FAIL  the application logged QML problems:"; sed 's/^/        /' /tmp/ui-window.log | head -20; FAILED=1
else
    echo "  ok    no QML warnings were logged"
fi
[ "$FAILED" = 0 ] && echo "ui-window: passed" || echo "ui-window: FAILED"
exit "$FAILED"
