# SPDX-License-Identifier: GPL-3.0-or-later
#
# Session test of the "power" shell module. Sourced in the nested session:
#
#   SHADOW_RUN_TIMEOUT=600 scripts/dev.sh nested --dir modpower --wait 1 -- \
#       '. /src/desktop/horizon-shell/modules/power/tests/session-test.sh'
#
# The nested session's KWin runs on a virtual X server without a screen
# locker and without DPMS. So every scenario runs in a SECOND KWin nested in
# the first (Wayland-in-Wayland, where KWin does implement DPMS), started
# with its screen locker, on a session bus of its own and with a PRIVATE bus
# as "system bus" that carries tests/fake-services.py (UPower, logind,
# NetworkManager; plus a notification daemon on the session bus, because the
# shell is built with this module only). Input for the inner compositor is
# sent with xdotool to the virtual X server and reaches it through the outer
# KWin. SHADOW_POWER_TIME_SCALE shortens the minutes of power.conf.
#
# Results: build/work/shots/nested-logs/modpower/session-test.log (one line
# per check), the logs and state files of every scenario next to it, and the
# screenshots build/work/shots/modpower-*.png.
TESTS=/src/desktop/horizon-shell/modules/power/tests

if [ "${1:-}" != "--inner" ]; then
    # ---- outer part: one inner session per scenario ---------------------------
    export POWER_TEST_LOG="$SHADOW_LOGS/session-test.log"
    : > "$POWER_TEST_LOG"
    for scenario in ${POWER_TEST_SCENARIOS:-idle updates noresumelock}; do
        dbus-run-session -- bash "$TESTS/session-test.sh" --inner "$scenario"
    done
    echo "== $(grep -c '^ok' "$POWER_TEST_LOG") ok, $(grep -c '^FAIL' "$POWER_TEST_LOG") failed" | tee -a "$POWER_TEST_LOG"
    return 0 2>/dev/null || exit 0
fi

# ---- inner part ---------------------------------------------------------------
scenario="$2"
LOG="$POWER_TEST_LOG"
W="$HOME/power-test-$scenario"; mkdir -p "$W"
STATE="$XDG_RUNTIME_DIR/shadow-horizon-power.json"
note() { echo "$*" | tee -a "$LOG"; }
check() {
    if [ "$2" = "$3" ]; then note "ok    [$scenario] $1 ($2)"
    else note "FAIL  [$scenario] $1: got '$2', expected '$3'"; fi
}
contains() {
    case "$2" in *"$3"*) note "ok    [$scenario] $1 (… $3 …)" ;;
    *) note "FAIL  [$scenario] $1: '$2' does not contain '$3'" ;; esac
}
field() { python3 -c 'import json,sys
value = json.load(sys.stdin)
try:
    for key in sys.argv[1].split("."):
        value = value[int(key)] if isinstance(value, list) else value[key]
except (KeyError, IndexError):
    value = "<missing>"
print(value if isinstance(value, str) else json.dumps(value))' "$1"; }
st() { field "$1" < "$STATE"; }                         # the running shell's state
dump() { horizon-shell --dump power 2>/dev/null | field "$1"; }   # a second process
xdo() { DISPLAY="$SHADOW_NESTED_X" xdotool "$@"; }
shot() { DISPLAY="$SHADOW_NESTED_X" import -window root "$SHADOW_SHOTS/$1"; }
poke() { xdo mousemove 780 440 mousemove 800 450 mousemove 790 445; sleep 0.3; }     # user activity
ctl() { echo "$*" > "$W/ctl"; sleep 0.4; }
dpms() { kscreen-doctor --dpms show 2>&1 | sed -n 's/.*: //p' | head -n1; }
locked() { gdbus call --session -d org.freedesktop.ScreenSaver -o /ScreenSaver -m org.freedesktop.ScreenSaver.GetActive 2>&1; }
power_conf() {      # screenOff lock suspend  screenOffBattery lockBattery suspendBattery
    printf '[Power]\nIdleScreenOffMinutes=%s\nIdleLockMinutes=%s\nIdleSuspendMinutes=%s\nIdleScreenOffMinutesOnBattery=%s\nIdleLockMinutesOnBattery=%s\nIdleSuspendMinutesOnBattery=%s\n' \
        "$@" > "$HOME/.config/shadow/power.conf.new"
    mv "$HOME/.config/shadow/power.conf.new" "$HOME/.config/shadow/power.conf"     # atomically, like the Control Center
}
start_session() {   # time-scale lock-on-resume
    dbus-daemon --session --address="unix:path=$W/system-bus" --nofork --nopidfile > "$W/system-bus.log" 2>&1 &
    sleep 1
    export DBUS_SYSTEM_BUS_ADDRESS="unix:path=$W/system-bus"
    mkfifo "$W/ctl"
    # The stand-in notification daemon only where the shell has none of its own.
    fake_notifications=--notifications
    horizon-shell --dump modules 2>/dev/null | field modules | grep -q notifications && fake_notifications=
    python3 "$TESTS/fake-services.py" --control "$W/ctl" --log "$W/fakes.log" $fake_notifications &
    sleep 1.5
    printf '[Daemon]\nAutolock=false\nLockOnResume=%s\nLockGrace=0\n' "$2" > "$HOME/.config/kscreenlockerrc"
    # The inner compositor: a client of the outer one, with its screen locker.
    kwin_wayland --wayland-display "$WAYLAND_DISPLAY" --width 1200 --height 700 --socket "wayland-power-$scenario" > "$W/kwin-inner.log" 2>&1 &
    for i in $(seq 80); do [ -S "$XDG_RUNTIME_DIR/wayland-power-$scenario" ] && break; sleep 0.25; done
    export WAYLAND_DISPLAY="wayland-power-$scenario"
    sleep 2
    poke
    WAYLAND_DEBUG="${POWER_TEST_WAYLAND_DEBUG:-}" SHADOW_POWER_TIME_SCALE="$1" horizon-shell > "$W/shell.log" 2>&1 &
    shell=$!
    sleep 6
}
finish() {
    cp "$STATE" "$W/final-state.json" 2>/dev/null
    horizon-shell --dump power > "$W/final-dump.json" 2>/dev/null
    kill $(jobs -p) 2>/dev/null
    wait 2>/dev/null
    mkdir -p "$SHADOW_LOGS/$scenario"
    cp "$W"/*.log "$W"/*.json "$W"/runs "$SHADOW_LOGS/$scenario/" 2>/dev/null
}

case "$scenario" in
idle)
    # 1 minute = 2 seconds. AC: screens off after 2 s. Battery: off 4 s, suspend 6 s.
    power_conf 1 0 0  2 0 3
    start_session 30 true
    poke
    check "module loaded" "$(horizon-shell --dump modules | field modules)" '["power"]'
    check "dump: shell state" "$(dump shellState)" running
    check "time scale" "$(st settings.timeScale)" 30
    check "dump: UPower read" "$(dump powerSource.upowerAvailable)" true
    check "dump: source" "$(dump effective.source)" ac
    check "dump: effective screen-off minutes" "$(dump effective.screenOffMinutes)" 1
    check "dump: logind read" "$(dump inhibitors.logind.available)" true
    check "PolicyAgent name owned" "$(st inhibitors.applications.serving)" true
    check "DPMS protocol offered" "$(st screens.protocolAvailable)" true
    check "DPMS supported by the output" "$(st screens.canSwitchOff)" true
    poke
    check "timer armed" "$(st idle.timers.screenOff.state)" armed
    check "timer length (s)" "$(st idle.timers.screenOff.timeoutSeconds)" 2
    check "lock timer off (0 = never)" "$(st idle.timers.lock.state)" off
    check "session can suspend (logind CanSuspend)" "$(st canSuspend)" true
    check "session has a locker" "$(st canLock)" true

    note "-- an application inhibits through org.freedesktop.ScreenSaver (KWin)"
    poke
    python3 "$TESTS/inhibit-client.py" screensaver > "$W/inhibit1.out" & client=$!
    sleep 4
    check "inhibition forwarded by KWin and listed" "$(st inhibitors.applications.inhibitions.0.application)" test-player
    check "screen-off held back" "$(st idle.timers.screenOff.state)" held
    contains "… by the application" "$(st idle.timers.screenOff.detail)" test-player
    check "screen still on" "$(dpms)" on
    check "PolicyAgent.HasInhibition(4)" "$(gdbus call --session -d org.kde.Solid.PowerManagement.PolicyAgent -o /org/kde/Solid/PowerManagement/PolicyAgent -m org.kde.Solid.PowerManagement.PolicyAgent.HasInhibition 4)" "(true,)"
    contains "PolicyAgent.ListInhibitions" "$(gdbus call --session -d org.kde.Solid.PowerManagement.PolicyAgent -o /org/kde/Solid/PowerManagement/PolicyAgent -m org.kde.Solid.PowerManagement.PolicyAgent.ListInhibitions)" "('test-player', 'playing a film')"
    kill -USR1 $client          # UnInhibit
    sleep 0.8
    check "inhibition released" "$(st inhibitors.applications.inhibitions)" '[]'
    check "timers restarted on release" "$(st idle.armedBecause)" "inhibitor released"
    check "not yet off right after the release" "$(dpms)" on
    sleep 2.5
    check "screen-off done after a full timeout" "$(st idle.timers.screenOff.state)" done
    check "screen off (kscreen-doctor --dpms show)" "$(dpms)" off
    check "screen off (module)" "$(st screens.off)" true
    shot modpower-idle-screen-off.png
    kill $client 2>/dev/null

    note "-- user activity"
    poke; sleep 0.7
    check "user no longer idle" "$(st idle.userIdle)" false
    check "screen on again" "$(dpms)" on
    check "timer armed again" "$(st idle.timers.screenOff.state)" armed
    shot modpower-idle-screen-on.png

    note "-- an application that leaves the bus without releasing"
    python3 "$TESTS/inhibit-client.py" agent 4 > "$W/inhibit2.out" & client=$!
    sleep 1
    check "direct PolicyAgent inhibition listed" "$(st inhibitors.applications.inhibitions.0.application)" test-download
    kill $client; sleep 0.8
    check "dropped when its owner left" "$(st inhibitors.applications.inhibitions)" '[]'

    note "-- an inhibition that only forbids suspend (type 1) does not keep the screen on"
    poke
    python3 "$TESTS/inhibit-client.py" agent 1 > "$W/inhibit3.out" & client=$!
    sleep 3.5
    check "screen-off done" "$(st idle.timers.screenOff.state)" done
    kill $client; poke

    if command -v power-test-idle-inhibit >/dev/null; then
        note "-- a window with a Wayland idle inhibitor (zwp_idle_inhibit_manager_v1)"
        poke
        power-test-idle-inhibit > "$W/inhibit-window.out" 2>&1 & client=$!
        sleep 2.5
        check "inhibitor created" "$(cat "$W/inhibit-window.out")" inhibiting
        events=$(st idle.events | python3 -c 'import json,sys; print(len(json.load(sys.stdin)))')
        sleep 5
        check "the compositor reports no idleness: no timeout arrived" "$(st idle.events | python3 -c 'import json,sys; print(len(json.load(sys.stdin)))')" "$events"
        check "screen on" "$(dpms)" on
        check "user not idle" "$(st idle.userIdle)" false
        kill $client
        sleep 3.5
        check "screen-off done once the window is gone" "$(st idle.timers.screenOff.state)" done
        poke
    else
        note "-- (power-test-idle-inhibit not built: Wayland idle inhibitor not exercised)"
    fi

    note "-- logind idle inhibitor (block)"
    poke
    ctl logind block idle
    sleep 3.5
    check "screen-off held back" "$(st idle.timers.screenOff.state)" held
    contains "… by logind" "$(st idle.timers.screenOff.detail)" "logind idle inhibitor"
    check "dump: BlockInhibited" "$(dump inhibitors.logind.blockInhibited)" '["idle"]'
    check "screen still on" "$(dpms)" on
    ctl logind block -
    sleep 3.2
    check "screen-off done after the lock went away" "$(st idle.timers.screenOff.state)" done
    check "screen off" "$(dpms)" off
    poke

    note "-- settings change (file replaced atomically)"
    power_conf 0 0 0  2 0 3
    sleep 1.2
    check "effective screen-off minutes" "$(st effective.screenOffMinutes)" 0
    check "timer off" "$(st idle.timers.screenOff.state)" off
    events=$(st idle.events | python3 -c 'import json,sys; print(len(json.load(sys.stdin)))')
    sleep 3.5
    check "nothing happens with everything set to never" "$(st idle.events | python3 -c 'import json,sys; print(len(json.load(sys.stdin)))')" "$events"
    check "screen on" "$(dpms)" on
    # edited in place, with values a reader must not accept
    printf '[Power]\nIdleScreenOffMinutes=banana\nIdleLockMinutes=5000\nIdleSuspendMinutes=7\n' > "$HOME/.config/shadow/power.conf"
    sleep 1.2
    check "invalid value → default" "$(st effective.screenOffMinutes)" 10
    check "out-of-range value → default" "$(st effective.lockMinutes)" 15
    check "valid value read" "$(st effective.suspendMinutes)" 7
    power_conf 0 0 0  2 0 3
    sleep 1.2
    check "replaced again" "$(st effective.suspendMinutes)" 0

    note "-- on battery: the battery values; suspend with a logind sleep inhibitor"
    ctl logind block sleep
    ctl upower on-battery true
    poke
    check "source" "$(st effective.source)" battery
    check "effective suspend minutes" "$(st effective.suspendMinutes)" 3
    check "dump agrees" "$(dump effective.suspendMinutes)" 3
    sleep 7.5
    check "screen-off done (not held by a sleep inhibitor)" "$(st idle.timers.screenOff.state)" done
    check "suspend held back" "$(st idle.timers.suspend.state)" held
    contains "… by logind" "$(st idle.timers.suspend.detail)" "logind sleep inhibitor"
    check "logind.Suspend not called" "$(grep -c 'logind Suspend' "$W/fakes.log")" 0
    check "not locked" "$(locked)" "(false,)"
    ctl logind block -
    sleep 7.5
    check "suspend done" "$(st idle.timers.suspend.state)" done
    check "logind.Suspend called once, interactive" "$(grep -c 'logind Suspend interactive=True' "$W/fakes.log")" 1
    check "module did not lock (lock timer is off)" "$(st idle.timers.lock.state)" off
    check "KWin's locker locked on PrepareForSleep (LockOnResume=true)" "$(locked)" "(true,)"
    shot modpower-idle-locked-on-suspend.png
    ctl logind wake
    sleep 0.5
    check "timers restarted after wake-up" "$(st idle.armedBecause)" "woke up from suspend"
    finish
    ;;

updates)
    # 1 minute = 0.1 s: first check 0.3 s after start, network retry 0.5 s, interval 1 h = 6 s.
    power_conf 0 0 0  0 0 0
    printf '[Updates]\nAutoCheck=true\nCheckIntervalHours=1\n' > "$HOME/.config/shadow/updater.conf"
    mkdir -p "$W/bin" "$W/upd"
    cp "$TESTS/fake-updater.sh" "$W/bin/shadow-updater"
    export FAKE_UPDATER_DIR="$W/upd"
    echo security > "$W/upd/result"; : > "$W/upd/runs"; ln -s "$W/upd/runs" "$W/runs"
    REAL_UPDATER="$(command -v shadow-updater || true)"
    # Only the test double and this build on PATH (the harness adds every build tree).
    PATH="$W/bin:$(dirname "$(command -v horizon-shell)"):/usr/sbin:/usr/bin:/sbin:/bin"
    notifies() { grep -c "^notify id" "$W/fakes.log"; }
    runs() { grep -c -- "--check" "$W/upd/runs"; }
    (sleep 2.5; echo "nm state 20" > "$W/ctl") &        # network down before the shell is up
    start_session 600 true
    check "auto check on" "$(st updates.autoCheck)" true
    check "updater found" "$(st updates.updaterInstalled)" true
    check "no check while the network is down" "$(runs)" 0
    check "waiting for the network" "$(st updates.waitingForNetwork)" true
    contains "network state seen" "$(st updates.network)" "NetworkManager state 20"

    note "-- network comes up: check runs, security updates are announced"
    ctl nm state 70
    sleep 2
    check "one check" "$(runs)" 1
    check "result" "$(st updates.lastCheck.result)" security
    check "counts from the updater's JSON" "$(st updates.lastCheck.updates)/$(st updates.lastCheck.securityUpdates)" 5/2
    check "one notification" "$(notifies)" 1
    line="$(grep '^notify id' "$W/fakes.log" | tail -n1)"
    contains "summary" "$line" "summary='Security updates are available'"
    contains "body" "$line" "5 updates can be installed. 2 of them fix security problems."
    contains "action" "$line" "open|Open Updater"
    id="$(echo "$line" | sed -n 's/^notify id=\([0-9]*\).*/\1/p')"
    ctl notify invoke "$id" open
    sleep 1
    check "action started shadow-updater (no arguments)" "$(grep -c '^run $' "$W/upd/runs")" 1
    check "notification closed after the action" "$(grep -c "notify close id=$id" "$W/fakes.log")" 1

    note "-- next interval: nothing pending → no notification"
    echo none > "$W/upd/result"
    sleep 6.5
    check "second check" "$(runs)" 2
    check "result" "$(st updates.lastCheck.result)" none
    check "still one notification" "$(notifies)" 1

    note "-- next interval: ordinary updates"
    echo updates > "$W/upd/result"
    sleep 6
    check "third check" "$(runs)" 3
    check "two notifications" "$(notifies)" 2
    contains "summary" "$(grep '^notify id' "$W/fakes.log" | tail -n1)" "summary='Updates are available'"
    contains "body" "$(grep '^notify id' "$W/fakes.log" | tail -n1)" "body='1 update can be installed.'"

    note "-- next interval: the check fails → no notification"
    echo failed > "$W/upd/result"
    sleep 6
    check "fourth check" "$(runs)" 4
    check "result" "$(st updates.lastCheck.result)" failed
    contains "error kept for the dump" "$(st updates.lastCheck.error)" "test: the check failed"
    check "still two notifications" "$(notifies)" 2

    if [ -n "$REAL_UPDATER" ]; then
        note "-- the real shadow-updater --check ($REAL_UPDATER; no PackageKit on the test bus)"
        ln -sf "$REAL_UPDATER" "$W/bin/shadow-updater"
        sleep 7
        note "      real updater: result=$(st updates.lastCheck.result) exit=$(st updates.lastCheck.exitCode) error=$(st updates.lastCheck.error)"
        check "still two notifications" "$(notifies)" 2
    else
        note "-- (no built shadow-updater found; the real binary was not run)"
    fi

    note "-- updater not installed"
    rm -f "$W/bin/shadow-updater"
    sleep 6.5
    check "updater missing" "$(st updates.updaterInstalled)" false
    check "nothing run" "$(st updates.lastCheck.result)" not-run
    check "still two notifications" "$(notifies)" 2
    cp "$TESTS/fake-updater.sh" "$W/bin/shadow-updater"

    note "-- AutoCheck=false"
    printf '[Updates]\nAutoCheck=false\nCheckIntervalHours=1\n' > "$HOME/.config/shadow/updater.conf"
    sleep 1; before=$(runs)
    check "auto check off" "$(st updates.autoCheck)" false
    check "no next check" "$(st updates.nextCheck)" "<missing>"
    sleep 6.5
    check "no further checks" "$(runs)" "$before"

    note "-- low battery (UPower warning levels)"
    ctl upower on-battery true
    ctl battery 9 3
    line="$(grep '^notify id' "$W/fakes.log" | tail -n1)"
    contains "low: summary" "$line" "summary='Battery low'"
    contains "low: body" "$line" "9% remaining."
    contains "low: normal urgency" "$line" "urgency=1"
    check "notice recorded" "$(st batteryNotice)" low
    ctl battery 8 3
    check "no second notice for the same level" "$(notifies)" 3
    ctl battery 4 4
    line="$(grep '^notify id' "$W/fakes.log" | tail -n1)"
    contains "critical: summary" "$line" "summary='Battery critically low'"
    contains "critical: UPower's action (HybridSleep) announced" "$line" "will suspend (with a hibernation image)"
    contains "critical: urgency" "$line" "urgency=2"
    contains "critical: replaces the low notice" "$line" "replaces=3"
    ctl battery 2 5
    line="$(grep '^notify id' "$W/fakes.log" | tail -n1)"
    contains "action: summary" "$line" "summary='Battery empty'"
    contains "action: body" "$line" "The computer is suspending now."
    ctl upower on-battery false
    ctl battery 3 1
    check "notice withdrawn when charging" "$(grep -c 'notify close id=3' "$W/fakes.log")" 1
    check "five notifications in all" "$(notifies)" 5

    note "-- lock when idle (10 minutes = 1 s)"
    check "not locked" "$(locked)" "(false,)"
    poke
    power_conf 0 10 0  0 0 0
    sleep 3.5
    check "lock done" "$(st idle.timers.lock.state)" done
    check "locked (org.freedesktop.ScreenSaver.GetActive)" "$(locked)" "(true,)"
    shot modpower-idle-locked.png
    finish
    ;;

popup)
    # Only with a shell built with --modules "power;notifications": the
    # notices in the shell's own notification daemon (same process, same bus
    # connection). POWER_TEST_CLICK="X Y" clicks there (the action button).
    power_conf 0 0 0  0 0 0
    printf '[Updates]\nAutoCheck=true\nCheckIntervalHours=24\n' > "$HOME/.config/shadow/updater.conf"
    mkdir -p "$W/bin" "$W/upd"
    cp "$TESTS/fake-updater.sh" "$W/bin/shadow-updater"
    export FAKE_UPDATER_DIR="$W/upd"
    echo security > "$W/upd/result"; : > "$W/upd/runs"; ln -s "$W/upd/runs" "$W/runs"
    PATH="$W/bin:$(dirname "$(command -v horizon-shell)"):/usr/sbin:/usr/bin:/sbin:/bin"
    start_session 600 true
    contains "shell has its own notification daemon" "$(horizon-shell --dump modules | field modules)" notifications
    check "check ran" "$(grep -c -- --check "$W/upd/runs")" 1
    check "notification accepted by the shell's daemon" "$(st notifications.sent)/$(st notifications.lastError)" "1/<missing>"
    shot modpower-popup-updates.png
    if [ -n "${POWER_TEST_CLICK:-}" ]; then
        xdo mousemove $POWER_TEST_CLICK click 1
        sleep 1.5
        check "clicking the action started shadow-updater" "$(grep -c '^run $' "$W/upd/runs")" 1
        shot modpower-popup-after-click.png
    fi
    ctl upower on-battery true
    ctl battery 4 4
    sleep 1
    check "battery notice sent" "$(st notifications.sent)" 2
    shot modpower-popup-battery.png
    finish
    ;;

noresumelock)
    # LockOnResume=false: nobody locks when the computer suspends.
    # Also records which ext-idle-notify request KIdleTime uses.
    power_conf 0 0 0  0 0 0
    POWER_TEST_WAYLAND_DEBUG=1 start_session 30 false
    poke
    power_conf 0 0 1  0 0 0
    sleep 4.5
    check "suspend done" "$(st idle.timers.suspend.state)" done
    check "logind.Suspend called" "$(grep -c 'logind Suspend interactive=True' "$W/fakes.log")" 1
    check "not locked (LockOnResume=false)" "$(locked)" "(false,)"
    check "idle request used: ext_idle_notifier_v1.get_idle_notification (honours Wayland idle inhibitors)" \
        "$(grep -o 'ext_idle_notifier_v1#[0-9]*\.get_[a-z_]*' "$W/shell.log" | sed 's/#[0-9]*//' | sort -u | tr '\n' ' ')" \
        "ext_idle_notifier_v1.get_idle_notification "
    grep -E 'ext_idle_notif|org_kde_kwin_dpms' "$W/shell.log" > "$W/wayland-idle-dpms.log"
    : > "$W/shell.log"
    finish
    ;;
esac
