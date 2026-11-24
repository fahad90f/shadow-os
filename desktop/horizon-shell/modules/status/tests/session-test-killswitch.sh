# SPDX-License-Identifier: GPL-3.0-or-later
#
# The kill switch indicator, the quick-settings tile and the panic action of
# the shell, in a nested session: a real KWin, a real horizon-shell, a real
# wl-clipboard against KWin's clipboard; and, standing in for systemd,
# NetworkManager, logind and the screen locker, fake-panic-services.py on a
# PRIVATE bus that the shell is told is the system bus. The state files the
# root tools would write to /run/shadow are written by this script into a
# directory of its own (SHADOW_STATUS_RUN_DIR). Nothing touches a network.
#
# Sourced inside the session, with the helpers shot and xdo, SHADOW_LOGS and
# (optional) TREE:
#
#   any session runner that sources a file:  . "$TREE/desktop/horizon-shell/modules/status/tests/session-test-killswitch.sh"
#   dev.sh:    SHADOW_RUN_TIMEOUT=400 scripts/dev.sh nested --dir modstatus --wait 1 -- \
#                  '. /src/desktop/horizon-shell/modules/status/tests/session-test-killswitch.sh'
#
# Needs: wl-clipboard (wl-copy, wl-paste), python3-gi, qdbus6, xdotool.
# Results: $SHADOW_LOGS/session-test-killswitch.log (one "ok"/"FAIL" line per
# check, "FAILED: n" at the end; the function returns non-zero on a failure),
# fake-services.log, pkexec.log, dump-*.json, and the screenshots ks-*.png.
# The click positions assume the default 1600x900 screen.
: "${STATUS_TEST_SIZE:=1600}"
KS_TESTS="${TREE:-/src}/desktop/horizon-shell/modules/status/tests"
KS_LOG="$SHADOW_LOGS/session-test-killswitch.log"
: > "$KS_LOG"
ks_failures=0
note() { echo "$*" | tee -a "$KS_LOG"; }
check() {   # check <description> <actual> <expected>
    if [ "$2" = "$3" ]; then note "ok    $1 ($2)"
    else note "FAIL  $1: got '$2', expected '$3'"; ks_failures=$((ks_failures + 1)); fi
}
field() { python3 -c 'import json,sys
value = json.load(sys.stdin)
for key in sys.argv[1].split("."):
    value = value[key]
print(json.dumps(value) if not isinstance(value, str) else value)' "$1"; }
RUN="$HOME/run-shadow"                    # created later: the shell must notice that too
ksdump() { SHADOW_STATUS_RUN_DIR="$RUN" QT_QPA_PLATFORM=offscreen horizon-shell --dump status 2>/dev/null; }
# State files, written like the tools do (temporary file, then rename).
put() {   # put <name> <json>
    mkdir -p "$RUN"
    printf '%s\n' "$2" > "$RUN/.$1.tmp" && mv -f "$RUN/.$1.tmp" "$RUN/$1"
}
ks_state() {   # ks_state <mode> <loaded> [error]
    local active=true err=null
    [ "$1" = off ] && active=false
    [ -n "${3:-}" ] && err="\"$3\""
    put killswitch.json "{\"version\": 1, \"mode\": \"$1\", \"active\": $active, \"loaded\": $2, \"since\": \"2026-11-23T10:00:00Z\", \"lan\": false, \"endpoints\": 0, \"error\": $err, \"escape_hatch\": false, \"persistent_mode\": \"$1\", \"runtime_override\": false, \"tunnel_ifaces\": 0, \"skipped_hostnames\": 0, \"dropped_packets\": null, \"updated\": \"2026-11-23T10:00:01Z\"}"
}
fx() { echo "$*" > "$HOME/fake.ctl"; sleep 0.3; }
calls() { grep -c "^call $1" "$HOME/fake-services.log"; }
invoke() { qdbus6 org.kde.kglobalaccel /component/shadow_horizon org.kde.kglobalaccel.Component.invokeShortcut "$1"; }
clip() { wl-paste --no-newline 2>&1; }
clip_primary() { wl-paste --primary --no-newline 2>&1; }
W=$STATUS_TEST_SIZE
# The panic banner as seen on the screen: red | amber | none. Its colours do
# not follow the theme (#B00020 cut or cutting, #FFC23D a problem); the
# middle of its top edge is its background whenever it is shown.
banner() {   # banner NAME.png
    shot "$1" "${W}x140+0+0" >&2 || { echo no-screenshot; return; }
    case "$(python3 "$KS_TESTS/png-pixel.py" "${SHADOW_SHOTS:-$OUT}/$1" $((W / 2)) 3)" in
        "#B00020") echo red ;;
        "#FFC23D") echo amber ;;
        "#"*) echo none ;;
        *) echo unreadable ;;
    esac
}

# ---- fakes: a private "system" bus, the services, pkexec ---------------------------------
dbus-daemon --session --address="unix:path=$HOME/system-bus" --nofork --nopidfile > "$HOME/system-bus.log" 2>&1 &
sleep 1
mkfifo "$HOME/fake.ctl"
DBUS_SYSTEM_BUS_ADDRESS="unix:path=$HOME/system-bus" python3 "$KS_TESTS/fake-panic-services.py" --screensaver \
    --control "$HOME/fake.ctl" --log "$HOME/fake-services.log" --run-dir "$RUN" > "$HOME/fake-services.out" 2>&1 &
sleep 1.5
note "fakes: $(head -3 "$HOME/fake-services.log" | tr '\n' ' ')"
mkdir -p "$HOME/fakebin"
# pkexec as far as the shell can tell: records the command, then plays
# "shadow-panic release" per $HOME/pkexec.mode (ok | cancel | fail).
cat > "$HOME/fakebin/pkexec" <<'FAKE'
#!/bin/sh
echo "pkexec $*" >> "$HOME/pkexec.log"
case "$(cat "$HOME/pkexec.mode" 2>/dev/null)" in
    cancel) exit 126 ;;
    fail) echo "shadow-panic: could not delete the panic table: nft failed" >&2; exit 3 ;;
esac
dir="${SHADOW_STATUS_RUN_DIR:?}"
printf '{"version": 1, "engaged": false, "loaded": false, "error": null, "since": null, "released": "2026-11-23T10:30:00Z", "warnings": []}\n' > "$dir/.panic.tmp"
mv -f "$dir/.panic.tmp" "$dir/panic.json"
FAKE
chmod 755 "$HOME/fakebin/pkexec"
: > "$HOME/pkexec.log"

# ---- the shell under test (replacing one the session may have started) ---------------------
if [ -n "${SHELL_PID:-}" ] && kill -0 "$SHELL_PID" 2>/dev/null; then
    kill "$SHELL_PID"; wait "$SHELL_PID" 2>/dev/null
fi
pkill -x horizon-shell 2>/dev/null; sleep 1
PANIC_CONFIG="$HOME/panic-config.json"   # absent at first: the defaults (lock, clear the clipboard)
DBUS_SYSTEM_BUS_ADDRESS="unix:path=$HOME/system-bus" SHADOW_STATUS_RUN_DIR="$RUN" SHADOW_PANIC_CONFIG="$PANIC_CONFIG" \
    PATH="$HOME/fakebin:$PATH" horizon-shell >> "$SHADOW_LOGS/shell.log" 2>&1 &
SHELL_PID=$!
sleep 8

# ---- the shortcut -------------------------------------------------------------------------
check "panic shortcut registered with its default key" \
    "$(sed -n 's/^panic=\([^,]*\),.*/\1/p' "$HOME/.config/kglobalshortcutsrc")" "Ctrl+Alt+Shift+Esc"
check "no other component holds Ctrl+Alt+Shift+Esc" \
    "$(grep -v '^panic=' "$HOME/.config/kglobalshortcutsrc" | grep -c 'Ctrl+Alt+Shift+Esc')" 0
check "no unbound-shortcut warning" "$(grep -c 'has no key' "$SHADOW_LOGS/shell.log")" 0
SHADOW_PANIC_CONFIG="$PANIC_CONFIG" QT_QPA_PLATFORM=offscreen horizon-shell --dump panic > "$SHADOW_LOGS/dump-panic-config.json" 2>/dev/null
check "dump panic: defaults without a configuration file" \
    "$(field lockSession < "$SHADOW_LOGS/dump-panic-config.json"):$(field clearClipboard < "$SHADOW_LOGS/dump-panic-config.json")" "true:true"
check "dump panic: release command" "$(field releaseCommand < "$SHADOW_LOGS/dump-panic-config.json")" "pkexec /usr/sbin/shadow-panic release"

# ---- kill switch: no file, then on, error, not loaded, off ---------------------------------
ksdump > "$SHADOW_LOGS/dump-ks-0.json"
check "no status file: state" "$(field killSwitch.state < "$SHADOW_LOGS/dump-ks-0.json")" unknown
check "no status file: not available" "$(field killSwitch.available < "$SHADOW_LOGS/dump-ks-0.json")" false
shot ks-0-no-file.png
ks_state tor true; sleep 1
ksdump > "$SHADOW_LOGS/dump-ks-1.json"
check "tor, loaded: state" "$(field killSwitch.state < "$SHADOW_LOGS/dump-ks-1.json")" on
check "tor, loaded: protected" "$(field killSwitch.protected < "$SHADOW_LOGS/dump-ks-1.json")" true
shot ks-1-active.png
shot ks-1-active-panel.png "420x34+$((W - 420))+0"
ks_state tor true "the configuration cannot be used"; sleep 1
ksdump > "$SHADOW_LOGS/dump-ks-2.json"
check "error set: state" "$(field killSwitch.state < "$SHADOW_LOGS/dump-ks-2.json")" error
check "error set: not protected" "$(field killSwitch.protected < "$SHADOW_LOGS/dump-ks-2.json")" false
shot ks-2-error.png
shot ks-2-error-panel.png "420x34+$((W - 420))+0"
ks_state vpn false; sleep 1
ksdump > "$SHADOW_LOGS/dump-ks-3.json"
check "vpn, not loaded: state" "$(field killSwitch.state < "$SHADOW_LOGS/dump-ks-3.json")" error
shot ks-3-not-loaded-panel.png "420x34+$((W - 420))+0"

# ---- quick settings: the tile and the full error text ---------------------------------------
xdo key super+a; sleep 1.5
shot ks-4-quick-settings-not-loaded.png
ks_state tor-or-vpn true; sleep 1
shot ks-5-quick-settings-on.png
xdo key Escape; sleep 0.8
put killswitch.json '{"mode": "tor", "active": true, "loaded": tru'; sleep 1
ksdump > "$SHADOW_LOGS/dump-ks-6.json"
check "broken file: state" "$(field killSwitch.state < "$SHADOW_LOGS/dump-ks-6.json")" error
shot ks-6-broken-file-panel.png "420x34+$((W - 420))+0"
ks_state off false; sleep 1
ksdump > "$SHADOW_LOGS/dump-ks-7.json"
check "off: state" "$(field killSwitch.state < "$SHADOW_LOGS/dump-ks-7.json")" off
check "off: no indicator" "$(field killSwitch.indicatorVisible < "$SHADOW_LOGS/dump-ks-7.json")" false
shot ks-7-off-panel.png "420x34+$((W - 420))+0"
ks_state tor true; sleep 1

# ---- panic: the real key, through Xvfb -> KWin -> kglobalaccel -> shell -----------------------
if command -v wl-copy > /dev/null; then
    wl-copy "clipboard test text"; wl-copy --primary "primary test text"; sleep 0.5
    check "clipboard holds the test text before the panic" "$(clip)" "clipboard test text"
    check "primary selection holds the test text before the panic" "$(clip_primary)" "primary test text"
else
    note "SKIP  clipboard checks: wl-clipboard is not installed"
fi
xdo key ctrl+alt+shift+Escape; sleep 3
check "StartUnit of shadow-panic.service (no interactive authorisation)" \
    "$(grep -c '^call systemd StartUnit shadow-panic.service replace interactive=false' "$HOME/fake-services.log")" 1
check "screen locked through the screen locker" "$(calls 'screensaver Lock')" 1
check "no fallback without a failure" "$(calls 'nm Enable')" 0
if command -v wl-copy > /dev/null; then
    check "clipboard empty after the panic" "$(clip)" "Nothing is copied"
    check "primary selection empty after the panic" "$(clip_primary)" "Nothing is copied"
fi
ksdump > "$SHADOW_LOGS/dump-panic-1.json"
check "panic engaged per the status file" "$(field killSwitch.panic.state < "$SHADOW_LOGS/dump-panic-1.json")" engaged
shot ks-8-panic-banner.png
shot ks-8-panic-banner-top.png "${W}x140+0+0"

# Restore: the administrator cancels, then the tool fails, then it works.
restore_x=$((W - 24 - 84)); restore_y=49
echo cancel > "$HOME/pkexec.mode"
xdo mousemove "$restore_x" "$restore_y" click 1; sleep 2
check "release runs pkexec shadow-panic release" "$(tail -1 "$HOME/pkexec.log")" "pkexec /usr/sbin/shadow-panic release"
shot ks-9-release-cancelled.png "${W}x140+0+0"
echo fail > "$HOME/pkexec.mode"
xdo mousemove "$restore_x" "$restore_y" click 1; sleep 2
shot ks-10-release-failed.png "${W}x140+0+0"
echo ok > "$HOME/pkexec.mode"
xdo mousemove "$restore_x" "$restore_y" click 1; sleep 2
check "pkexec called three times" "$(grep -c '^pkexec /usr/sbin/shadow-panic release$' "$HOME/pkexec.log")" 3
ksdump > "$SHADOW_LOGS/dump-panic-2.json"
check "released per the status file" "$(field killSwitch.panic.state < "$SHADOW_LOGS/dump-panic-2.json")" released
shot ks-11-released.png
check "released from the banner: banner gone" "$(banner ks-11-released-top.png)" none

# ---- panic without the service: NetworkManager fallback, then nothing works -------------------
# (and the screen locker refuses: logind is asked instead)
fx systemd deny
fx screensaver deny
invoke panic; sleep 2
check "fallback: NetworkManager networking off" "$(grep -c '^call nm Enable false interactive=false' "$HOME/fake-services.log")" 1
check "screen locker refused: logind asked to lock" "$(calls 'logind Session.Lock')" 1
shot ks-12-fallback.png "${W}x140+0+0"
xdo mousemove "$restore_x" "$restore_y" click 1; sleep 2
check "fallback restore: NetworkManager networking on, no pkexec" \
    "$(grep -c '^call nm Enable true' "$HOME/fake-services.log"):$(grep -c . "$HOME/pkexec.log")" "1:3"
shot ks-13-fallback-restored.png "${W}x140+0+0"
fx nm deny
fx logind deny
invoke panic; sleep 2
shot ks-14-failed.png "${W}x140+0+0"
dismiss_x=$((W - 24 - 50))
xdo mousemove "$dismiss_x" "$restore_y" click 1; sleep 1.5
shot ks-15-dismissed.png "${W}x140+0+0"
fx nm accept
fx logind accept
fx screensaver accept

# ---- accepted, but never confirmed; a status file that reports an error ----------------------
fx systemd silent
invoke panic; sleep 17
shot ks-16-unconfirmed.png "${W}x140+0+0"
xdo mousemove "$dismiss_x" "$restore_y" click 1; sleep 1.5
fx systemd accept
put panic.json '{"version": 1, "engaged": true, "loaded": false, "error": null, "since": "2026-11-23T10:40:00Z"}'; sleep 1.5
ksdump > "$SHADOW_LOGS/dump-panic-3.json"
check "engaged but not loaded: state" "$(field killSwitch.panic.state < "$SHADOW_LOGS/dump-panic-3.json")" error
shot ks-17-panic-not-loaded.png "${W}x140+0+0"
put panic.json '{"version": 1, "engaged": false, "loaded": false, "error": null, "released": "2026-11-23T10:45:00Z"}'; sleep 1

# ---- the configuration: no lock, clipboard kept -------------------------------------------------
printf '{"rfkill": true, "lock_session": false, "clear_clipboard": false}\n' > "$PANIC_CONFIG"
locks_before="$(calls 'screensaver Lock'):$(calls 'logind Session.Lock')"
if command -v wl-copy > /dev/null; then wl-copy "kept text"; sleep 0.5; fi
invoke panic; sleep 3
check "lock_session false: no lock request" "$(calls 'screensaver Lock'):$(calls 'logind Session.Lock')" "$locks_before"
if command -v wl-copy > /dev/null; then
    check "clear_clipboard false: clipboard kept" "$(clip)" "kept text"
fi
shot ks-18-config-no-lock.png "${W}x140+0+0"
xdo mousemove "$restore_x" "$restore_y" click 1; sleep 2
check "released again" "$(ksdump | field killSwitch.panic.state)" released
rm -f "$PANIC_CONFIG"

# ---- released elsewhere: the Control Center's "Reconnect…" or sudo shadow-panic release ---------
# The shell's request must end with panic.json's "released": no banner, and no
# "the panic service has not confirmed the cut" alarm once 15 s have passed.
invoke panic; sleep 3
check "panic: banner red" "$(banner ks-20-engaged.png)" red
put panic.json '{"version": 1, "engaged": false, "loaded": false, "error": null, "since": null, "released": "2026-11-23T10:50:00Z", "warnings": [], "clear_clipboard": true, "lock_session": true, "dropped_packets": null, "updated": "2026-11-23T10:50:00Z"}'
sleep 2
check "released elsewhere: banner gone at once" "$(banner ks-21-released-elsewhere.png)" none
sleep 15
check "released elsewhere: still no banner 17 s later (no false alarm)" "$(banner ks-22-released-elsewhere-17s.png)" none

# ---- the banner's buttons from the keyboard: Meta+A, Tab, Space ---------------------------------
# The banner itself gets the keyboard only when clicked; quick settings has
# its buttons, first in the popup.
echo ok > "$HOME/pkexec.mode"
invoke panic; sleep 3
check "panic again: banner red" "$(banner ks-23-engaged.png)" red
pkexec_before=$(grep -c . "$HOME/pkexec.log")
xdo key super+a; sleep 1.5
shot ks-24-quick-settings-panic.png
xdo key Tab; sleep 0.5
shot ks-25-quick-settings-panic-focus.png
xdo key space; sleep 2
check "keyboard: Restore network… in quick settings runs pkexec shadow-panic release" \
    "$(grep -c . "$HOME/pkexec.log")" $((pkexec_before + 1))
check "keyboard release: released per the status file" "$(ksdump | field killSwitch.panic.state)" released
check "keyboard release: banner gone" "$(banner ks-26-keyboard-released.png)" none
xdo key Escape; sleep 0.8

# ---- the tile opens the Control Center's privacy page ----------------------------------------
ks_state tor true; sleep 1
xdo key super+a; sleep 1.5
xdo mousemove $((W - 115)) 86 click 1; sleep 2
check "tile starts shadow-control-center privacy" \
    "$(pgrep -fa 'shadow-control-center privacy' | grep -c -v pgrep)" 1
sleep 10
shot ks-19-control-center-privacy.png
pkill -f 'shadow-control-center privacy'

cp "$HOME/fake-services.log" "$SHADOW_LOGS/fake-services.log"
cp "$HOME/pkexec.log" "$SHADOW_LOGS/pkexec.log"
note "shell warnings: $(grep -c -i 'TypeError\|ReferenceError\|qrc:/' "$SHADOW_LOGS/shell.log")"
note "FAILED: $ks_failures"
[ "$ks_failures" -eq 0 ]
