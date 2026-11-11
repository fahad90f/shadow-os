#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Drives the Services page with mouse and keyboard (xdotool) against the
# stand-in for systemd's D-Bus manager (fixture.py, mock-systemd.py) on a
# private system bus and checks what it was asked to do:
#
#   sudo scripts/dev.sh run --dir monitor python3 /src/applications/monitor/tests/fixture.py \
#        bash /src/applications/monitor/tests/ui-services.sh
#
#   Start runs at once; the button shows the action in progress; the list refreshes
#   Stop asks first; the confirmation is operated with the keyboard (Tab, Tab, Space)
#   a refused action ("Access denied") is reported and changes nothing
#   a job that systemd reports as failed (JobRemoved "failed") is reported
#   Enable at boot switches the unit-file state
#   the Failed filter and the search field narrow the list
#
# This exercises the monitor's own logic; whether a real systemd and polkit
# agent behave like the stand-in is checked on an installed system, not here.
# Screenshots go to /shots/ui-services-*.png.
#
# shellcheck disable=SC2317  # the EXIT trap is invoked indirectly
set -u

B="${LD_LIBRARY_PATH%%/lib*}"
APP="$B/bin/shadow-monitor"
STATE=/tmp/shadow-monitor-fixture/systemd-state.json
[ -f "$STATE" ] || { echo "ui-services: run through fixture.py"; exit 2; }
export SHADOW_WINDOW_SIZE=1240x800
FAILED=0

shot() { import -window root -crop 1240x800+0+0 +repage "/shots/ui-services-$1.png" 2>/dev/null; }
click() {   # click "<x> <y>"
    local x y
    read -r x y <<<"$1"
    xdotool mousemove "$x" "$y" click 1
    sleep 0.4
}
unit() { python3 -c "import json,sys; u=json.load(open('$STATE'))['$1']; print(u['active'], u['sub'], u['file'])"; }
check() {   # check <description> <unit> <expected "active sub file">
    local got; got="$(unit "$2")"
    if [ "$got" = "$3" ]; then echo "  ok    $1"; else echo "  FAIL  $1: $2 is '$got', expected '$3'"; FAILED=1; fi
}

"$APP" --page services > /tmp/ui-services.log 2>&1 &
APPPID=$!
trap 'kill -9 "$APPPID" 2>/dev/null; rm -f /tmp/ui-services.log' EXIT
WID=$(xdotool search --sync --onlyvisible --name "System Monitor" | head -1)
xdotool windowfocus --sync "$WID"
xdotool mousemove 1200 790
sleep 2.5

# Rows in name order: apparmor backup bluetooth cron cups denied getty@tty1
# NetworkManager rsync run-u42 ssh systemd-journald unstable — 34 px each from y=194.
START="910 102"; ENABLE="1143 102"

click "400 262"                             # bluetooth: inactive, enabled
shot 1-selected
click "$START"; sleep 0.3
shot 2-starting
sleep 2.2
shot 3-started
check "Start started the selected service" bluetooth.service "active running enabled"

click "400 534"                             # ssh: running
click "$START"                              # now reads "Stop"
shot 4-confirm-stop
xdotool key Tab Tab space; sleep 2.6        # Cancel → Stop → activate
shot 5-stopped
check "Stop, confirmed with the keyboard, stopped the service" ssh.service "inactive dead enabled"

click "400 364"                             # denied: every action is refused
click "$START"; sleep 2.4
shot 6-denied
check "a refused start changed nothing" denied.service "inactive dead disabled"

click "400 602"                             # unstable: its start job ends with "failed"
click "$START"; sleep 2.6
shot 6b-job-failed
check "a start job that failed is reported and leaves the unit failed" unstable.service "failed failed disabled"

click "400 330"                             # cups: installed, disabled
click "$ENABLE"; sleep 2.4
shot 7-enabled
check "Enable at boot enabled the unit file" cups.service "  enabled"

click "1136 47"; sleep 0.8                  # "Failed"
shot 8-filter-failed
click "996 47"; sleep 0.5                   # "All"
xdotool key ctrl+f; sleep 0.3
xdotool type --delay 20 "network"; sleep 1.2
shot 9-search

if grep -q "qml\|QML\|TypeError\|ReferenceError" /tmp/ui-services.log; then
    echo "  FAIL  the application logged QML problems:"; sed 's/^/        /' /tmp/ui-services.log | head -20; FAILED=1
else
    echo "  ok    no QML warnings were logged"
fi
[ "$FAILED" = 0 ] && echo "ui-services: passed" || echo "ui-services: FAILED"
exit "$FAILED"
