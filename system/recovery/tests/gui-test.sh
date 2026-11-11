#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Drives the graphical front-end against the real engine and a fake installed
# system on loop devices (tests/fake-system.sh: same guards as engine-test.sh).
# Run as root inside the build chroot, in ONE invocation:
#
#   scripts/dev.sh exec --dir recovery -- bash /src/system/recovery/tests/gui-test.sh
#
# The window runs on a virtual X server and is operated with xdotool at fixed
# coordinates (window 980x1500 at 0,0, dark theme, default font). As root the
# front-end starts the engine directly; pkexec is not involved. Screenshots:
# build/work/shots/recovery-gui-*.png. Checks are made on the fake system
# itself (shadow file, archive, mount table), not on what the window says.
set -u
. "$(dirname "$0")/fake-system.sh"
pass=0; fail=0
ok() { note "ok    $1"; pass=$((pass + 1)); }
bad() { note "FAIL  $1"; fail=$((fail + 1)); }
check() { if [ "$2" = "$3" ]; then ok "$1 ($2)"; else bad "$1: got '$2', expected '$3'"; fi; }

export HOME="$WORK/home"; mkdir -p "$HOME"
export XDG_RUNTIME_DIR="$HOME/run"; mkdir -m 700 -p "$XDG_RUNTIME_DIR"
export QT_QPA_PLATFORM=xcb LIBGL_ALWAYS_SOFTWARE=1 QT_QUICK_CONTROLS_STYLE=org.nexus.style QT_LOGGING_RULES="qt.qpa.*=false"
export NEXUS_WINDOW_SIZE=980x1500
display=":$(( 300 + RANDOM % 600 ))"
Xvfb "$display" -screen 0 980x1500x24 -nolisten tcp > /dev/null 2>&1 & xvfb=$!
sleep 1.5
export DISPLAY="$display"
xdo() { xdotool "$@"; }
shot() { import -window root "/shots/$1"; }
click() { xdo mousemove "$1" "$2" click 1; sleep "${3:-1}"; }
MP="$WORK/state/mnt-$ROOT_UUID"
for device in ${NEXUS_RECOVERY_TEST_DEVICES//,/ }; do guard "$device"; done

dbus-run-session -- nexus-recovery --engine "$ENGINE" > "$WORK/gui.log" 2>&1 & gui=$!
sleep 7
shot recovery-gui-1-scanned.png

# Coordinates of the buttons (right column) in the 980x1500 window.
BTN_X=868
eval "${GUI_TEST_STEPS:-:}"         # extra xdotool steps for exploring the layout

if [ "${GUI_TEST_ONLY_FIRST:-0}" != 1 ]; then
    note "-- Check the file systems"
    click $BTN_X "$Y_CHECK" 6
    shot recovery-gui-2-fsck.png

    note "-- Repair the boot loader (mounts first; stand-in tools in the fake system)"
    click $BTN_X "$Y_BOOT" 7
    check "mounted by the front-end's repair" "$(grep -c " $MP " /proc/self/mounts)" 1
    check "the three boot steps ran in the installation" "$(tr '\n' ';' < "$MP/var/log/stub-calls.log")" "update-initramfs -u -k all;nexus-grub-install --verbose;update-grub ;"
    shot recovery-gui-3-boot.png

    note "-- Set a new password (dialog; typed twice)"
    old="$(grep '^alice:' "$MP/etc/shadow" | cut -d: -f2)"
    click $BTN_X "$Y_PASSWORD" 1.5
    shot recovery-gui-4-password-dialog.png
    eval "$PASSWORD_STEPS"
    sleep 4
    new="$(grep '^alice:' "$MP/etc/shadow" | cut -d: -f2)"
    check "the password of the chosen account changed" "$([ "$old" != "$new" ] && echo changed)" changed
    check "the typed password verifies against the new hash" "$(perl -e 'print crypt($ARGV[0], $ARGV[1]) eq $ARGV[1] ? "match" : "no match"' 'gui-Test-pw1' "$new")" match
    check "the password is not in the engine log" "$(grep -c 'gui-Test-pw1' "$WORK"/log/*.log)" 0
    check "the password is not in the front-end's output" "$(grep -c 'gui-Test-pw1' "$WORK/gui.log")" 0
    shot recovery-gui-5-password-done.png

    note "-- Save logs to… (dialog with a suggested file)"
    click $BTN_X "$Y_LOGS" 1.5
    shot recovery-gui-6-logs-dialog.png
    eval "$LOGS_STEPS"
    sleep 5
    archive="$(ls "$HOME"/nexus-recovery-logs-*.tar.gz 2>/dev/null | head -n1)"
    check "an archive was written to the suggested place" "$([ -n "$archive" ] && tar -tzf "$archive" | grep -c '^target/etc/fstab$')" 1
    shot recovery-gui-7-logs-done.png

    note "-- Unlock… (this kernel has no device-mapper: an honest failure is expected)"
    click $BTN_X "$Y_UNLOCK" 1.5
    eval "$UNLOCK_STEPS"
    sleep 5
    shot recovery-gui-8-unlock.png

    note "-- Unmount"
    click $BTN_X "$Y_MOUNT" 5
    check "unmounted by the front-end" "$(grep -c " $WORK/state" /proc/self/mounts)" 0
    shot recovery-gui-9-unmounted.png
fi

kill "$gui" 2>/dev/null; wait "$gui" 2>/dev/null
kill "$xvfb" 2>/dev/null
grep -E "QML|qml:|Warning|rror" "$WORK/gui.log" | head -n 10
cp "$WORK/gui.log" /shots/nested-logs/recovery/gui-test.log 2>/dev/null
note "== $pass ok, $fail failed"
[ "$fail" -eq 0 ]
