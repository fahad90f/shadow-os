#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Drives nexus-monitor with real key presses and mouse clicks (xdotool) under
# the virtual X server of "scripts/dev.sh run" and checks the effect on real
# processes. One scenario per invocation ("dev.sh run" allows 60 seconds):
#
#   for s in terminate kill button keyboard suspend renice foreign views launch; do
#       sudo scripts/dev.sh run --dir monitor bash /src/applications/monitor/tests/ui-processes.sh $s
#   done
#
# The scenarios that end processes run the monitor as the unprivileged user
# "nobody", so a misplaced click can never end anything but the test's own
# processes:
#   terminate  Ctrl+F focuses the search field and typing filters the table;
#              Delete in the search field edits the text; Delete on a selected
#              row asks, Escape cancels, "End process" sends SIGTERM
#   kill       a process ignoring SIGTERM survives "End process";
#              Shift+Delete asks and "Force kill" sends SIGKILL
#   button     the "End process" button does the same as Delete
#   keyboard   the arrow keys move the selection, which then survives the
#              periodic refreshes; Delete acts on the row they reached
#   suspend    the row's context menu: Suspend asks and sends SIGSTOP, the
#              entry then reads Continue and sends SIGCONT
#   renice     Change priority…: the slider is operated with the arrow keys;
#              a higher nice value is applied directly, a lower one needs an
#              administrator (pkexec), which fails here without changing anything
#   foreign    another user's process offers "End as administrator" (pkexec);
#              without a polkit authority the failure is reported and the
#              process keeps running; PID 1 is refused with an explanation
#   views      sorting by a column, selection following its process, context
#              menu, view options, tree view
# As root:
#   launch     a second launch with "--page <key>" or "<key>" switches the
#              page of the open window instead of starting another instance;
#              an unknown page is rejected; the update interval is stored
#
# Screenshots of the steps go to /shots/ui-<scenario>-*.png (build/work/shots/
# on the host); positions are those of the 1240x800 window the test forces.
# Needs xdotool and imagemagick (development tools of the build chroot).
#
# shellcheck disable=SC2317  # helpers are called through check() and the EXIT trap
set -u

SCENARIO="${1:-}"
B="${LD_LIBRARY_PATH%%/lib*}"
APP="$B/bin/nexus-monitor"
[ -x "$APP" ] || { echo "ui-processes: $APP not found (run through scripts/dev.sh run)"; exit 2; }
[ "$(id -u)" = 0 ] || { echo "ui-processes: must run as root (it switches to nobody itself)"; exit 2; }
export NEXUS_WINDOW_SIZE=1240x800

TOKEN="nxv${RANDOM}x${RANDOM}"      # appears in no other command line
FAILED=0
PIDS=()
LOG=/tmp/ui-processes.log

cleanup() {
    [ "${#PIDS[@]}" -gt 0 ] && kill -9 "${PIDS[@]}" 2>/dev/null
    rm -rf "${UHOME:-}" "$LOG"
}
trap cleanup EXIT

shot() { import -window root -crop 1240x800+0+0 +repage "/shots/ui-$SCENARIO-$1.png" 2>/dev/null; }
check() {   # check <description> <command...>
    local what="$1"; shift
    if "$@"; then echo "  ok    $what"; else echo "  FAIL  $what"; FAILED=1; fi
}
alive() { kill -0 "$1" 2>/dev/null && [ "$(ps -o stat= -p "$1" | cut -c1)" != Z ]; }
dead() { ! alive "$1"; }
state() { ps -o stat= -p "$1" | cut -c1; }
nice_of() { ps -o ni= -p "$1" | tr -d ' '; }
click() {   # click "<x> <y>" [button]
    local x y
    read -r x y <<<"$1"
    xdotool mousemove "$x" "$y" click "${2:-1}"
    sleep 0.4
}
search() {  # replace the search text
    xdotool key ctrl+f; sleep 0.3
    xdotool key ctrl+a BackSpace; sleep 0.2
    [ -n "$1" ] && xdotool type --delay 20 "$1"
    sleep 1.6
}
window() {  # wait for the monitor's window and give it the keyboard
    local wid
    wid=$(xdotool search --sync --onlyvisible --name "System Monitor" | head -1)
    xdotool windowfocus --sync "$wid"
    xdotool mousemove 1200 790
    sleep 2.5
}

# The unprivileged side: nobody, with a home of its own and access to the test X server.
UHOME="$(mktemp -d /tmp/ui-user.XXXXXX)"
mkdir -p "$UHOME/run"
chown -R 65534:65534 "$UHOME"
chmod 700 "$UHOME/run"
chmod 755 "$(dirname "$XAUTHORITY")"; chmod 644 "$XAUTHORITY"
AS_NOBODY=(setpriv --reuid=65534 --regid=65534 --clear-groups env "HOME=$UHOME" "XDG_RUNTIME_DIR=$UHOME/run")

victim() {  # victim <suffix>: an idle process of nobody's; its PID is left in $VICTIM
    "${AS_NOBODY[@]}" bash -c "exec -a $TOKEN-$1 sleep 300" &
    VICTIM=$!
    PIDS+=("$VICTIM")
}
start_unprivileged() {
    "${AS_NOBODY[@]}" "$APP" --page processes > "$LOG" 2>&1 &
    PIDS+=("$!")
    window
}
start_privileged() {
    "$APP" --page processes > "$LOG" 2>&1 &
    PIDS+=("$!")
    window
}

# Positions in the 1240x800 window.
ROW1="400 144"
ROW3="400 212"
CONFIRM="771 442"
CONFIRM_TALL="746 466"              # confirm button of the two-paragraph dialog
END_BUTTON="985 47"
OPTIONS_BUTTON="893 47"
# Context menu opened on ROW1: its entries relative to the click.
MENU_SUSPEND="460 237"
MENU_PRIORITY="460 269"

case "$SCENARIO" in
terminate)
    victim polite
    start_unprivileged
    shot 1-start
    xdotool key ctrl+f; sleep 0.4
    xdotool type --delay 20 "${TOKEN}zz"; sleep 0.4
    xdotool key Left Left; sleep 0.2
    xdotool key Delete Delete; sleep 1.6        # edits the text: "…zz" → "…"
    shot 2-search
    check "Delete in the search field did not end anything" alive "$VICTIM"
    click "$ROW1"
    shot 3-selected
    xdotool key Delete; sleep 0.8
    shot 4-confirm
    xdotool key Escape; sleep 0.6
    check "Escape dismissed the confirmation without a signal" alive "$VICTIM"
    xdotool key Delete; sleep 0.8
    click "$CONFIRM"; sleep 1.4
    shot 5-ended
    check "End process terminated the selected process (SIGTERM)" dead "$VICTIM"
    ;;
kill)
    "${AS_NOBODY[@]}" bash -c "trap '' TERM; exec -a $TOKEN-stubborn bash -c 'trap \"\" TERM; for i in \$(seq 300); do sleep 1; done'" &
    VICTIM=$!
    PIDS+=("$VICTIM")
    start_unprivileged
    search "$TOKEN"
    click "$ROW1"
    xdotool key Delete; sleep 0.8
    click "$CONFIRM"; sleep 1.6
    shot 1-survived
    check "a process ignoring SIGTERM survives End process" alive "$VICTIM"
    click "$ROW1"
    xdotool key shift+Delete; sleep 0.8
    shot 2-confirm
    click "$CONFIRM"; sleep 1.6
    shot 3-killed
    check "Force kill removed it (SIGKILL)" dead "$VICTIM"
    ;;
button)
    victim button
    start_unprivileged
    search "$TOKEN"
    click "$ROW1"
    click "$END_BUTTON"
    shot 1-confirm
    click "$CONFIRM"; sleep 1.4
    shot 2-ended
    check "the End process button terminated the selected process" dead "$VICTIM"
    ;;
keyboard)
    # Rows are ordered by CPU share, then PID: four idle victims appear in the
    # order they were started.
    VICTIMS=()
    for n in 1 2 3 4; do
        victim "$n"
        VICTIMS+=("$VICTIM")
    done
    start_unprivileged
    search "$TOKEN"
    click "$ROW1"
    xdotool key Down Down; sleep 2.6            # two rows down; the selection survives two refreshes
    shot 1-third-row
    xdotool key Delete; sleep 0.8
    click "$CONFIRM"; sleep 1.4
    shot 2-ended
    check "the arrow keys moved the selection: the third process was ended" dead "${VICTIMS[2]}"
    check "the first process is untouched" alive "${VICTIMS[0]}"
    check "the second process is untouched" alive "${VICTIMS[1]}"
    check "the fourth process is untouched" alive "${VICTIMS[3]}"
    ;;
foreign)
    bash -c "exec -a $TOKEN-rootowned sleep 300" &
    VICTIM=$!
    PIDS+=("$VICTIM")
    start_unprivileged
    search "$TOKEN"
    click "$ROW1"
    xdotool key Delete; sleep 0.8
    shot 1-confirm-administrator
    click "$CONFIRM_TALL"; sleep 2.5            # "End as administrator"
    shot 2-administrator-failed
    check "another user's process survives when authentication is unavailable" alive "$VICTIM"
    search "$(cat /proc/1/comm)"                # PID 1 has the lowest PID: first row
    click "$ROW1"
    xdotool key Delete; sleep 0.8
    shot 3-init-refused
    check "PID 1 is still running" alive 1
    ;;
views)
    start_unprivileged
    click "830 108"; sleep 1.4                  # "Memory" header: largest first
    shot 1-sorted-by-memory
    click "$ROW3" 3; sleep 0.6                  # the context menu selects the row
    shot 2-context-menu
    xdotool key Escape; sleep 3.2               # several refreshes with a selection
    shot 3-selection-kept
    click "$OPTIONS_BUTTON"; sleep 0.6          # view options
    shot 4-view-options
    click "966 121"; sleep 1.6                  # "Show kernel threads"
    shot 5-kernel-threads
    click "$OPTIONS_BUTTON"; sleep 0.6
    click "966 153"; sleep 1.6                  # "Show as tree"
    click "310 108"; sleep 1.6                  # "Name" header: siblings by name
    shot 6-tree
    ;;
suspend)
    victim pausable
    start_unprivileged
    search "$TOKEN"
    click "$ROW1" 3; sleep 0.4                  # context menu
    shot 1-menu
    click "$MENU_SUSPEND"; sleep 0.6
    shot 2-confirm-suspend
    xdotool key Escape; sleep 0.6
    check "Escape dismissed the confirmation: the process still runs" test "$(state "$VICTIM")" != T
    click "$ROW1" 3; sleep 0.4
    click "$MENU_SUSPEND"; sleep 0.6
    xdotool key Tab Tab space; sleep 1.8        # Cancel → Suspend → activate
    shot 3-suspended
    check "Suspend stopped the selected process (SIGSTOP)" test "$(state "$VICTIM")" = T
    click "$ROW1" 3; sleep 0.4
    shot 4-menu-continue
    click "$MENU_SUSPEND"; sleep 0.6            # the entry now reads "Continue"
    xdotool key Tab Tab space; sleep 1.8
    shot 5-continued
    check "Continue resumed it (SIGCONT)" test "$(state "$VICTIM")" = S
    check "the process is still alive" alive "$VICTIM"
    ;;
renice)
    victim nice
    bash -c "exec -a $TOKEN-zroot sleep 300" &  # root's: second row (same CPU share, higher PID)
    ROOTVICTIM=$!
    PIDS+=("$ROOTVICTIM")
    start_unprivileged
    search "$TOKEN"
    click "$ROW1" 3; sleep 0.4
    click "$MENU_PRIORITY"; sleep 0.8
    shot 1-dialog
    xdotool key Right Right Right Right Right; sleep 0.4
    shot 2-nice-5
    xdotool key Tab Tab space; sleep 1.8        # Cancel → Apply → activate
    shot 3-applied
    check "Change priority set nice 5 on the selected process" test "$(nice_of "$VICTIM")" = 5
    click "$ROW1" 3; sleep 0.4
    click "$MENU_PRIORITY"; sleep 0.8
    xdotool key Left Left Left; sleep 0.4       # 2: a higher priority needs an administrator
    shot 4-needs-administrator
    xdotool key Tab Tab space; sleep 2.5        # "Apply as administrator" (pkexec)
    shot 5-administrator-failed
    check "without a polkit authority the priority is left as it was" test "$(nice_of "$VICTIM")" = 5
    check "the other process was never touched" test "$(nice_of "$ROOTVICTIM")" = 0
    ;;
launch)
    start_privileged
    "$APP" --page sensors; sleep 1.5
    shot 1-sensors
    "$APP" disks; sleep 1.5
    shot 2-disks
    check "a second launch did not start a second instance" test "$(pgrep -c -x nexus-monitor)" = 1
    "$APP" --page nonsense 2>/dev/null
    check "an unknown page is rejected with exit code 2" test $? = 2
    click "93 767"; sleep 0.8                   # "2 s"
    shot 3-interval
    check "the update interval was stored" grep -q "intervalMs=2000" "$HOME/.config/nexus/monitor.conf"
    ;;
*)
    echo "usage: ui-processes.sh terminate|kill|button|keyboard|suspend|renice|foreign|views|launch"; exit 2 ;;
esac

if grep -q "qml\|QML\|TypeError\|ReferenceError" "$LOG"; then
    echo "  FAIL  the application logged QML problems:"; sed 's/^/        /' "$LOG" | head -20; FAILED=1
else
    echo "  ok    no QML warnings were logged"
fi
[ "$FAILED" = 0 ] && echo "ui-processes $SCENARIO: passed" || echo "ui-processes $SCENARIO: FAILED"
exit "$FAILED"
