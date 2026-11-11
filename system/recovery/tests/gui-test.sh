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

# Buttons of the right column in the 980x1500 window, as laid out before any
# action has run. A finished action adds a result line to its own row, which
# moves the rows BELOW it — so the actions are used from the bottom up.
BTN_X=868; Y_UNLOCK=221; Y_MOUNT=344; Y_CHECK=458; Y_BOOT=547; Y_PASSWORD=740; Y_LOGS=844
if [ "${GUI_TEST_ONLY_FIRST:-0}" != 1 ]; then
    note "-- Save logs to… (dialog with a suggested file; mounts the installation first)"
    click $BTN_X $Y_LOGS 1.5
    shot recovery-gui-2-logs-dialog.png
    xdo key Return
    sleep 6
    archive="$(ls "$HOME"/nexus-recovery-logs-*.tar.gz 2>/dev/null | head -n1)"
    check "mounted by the front-end" "$(grep -c " $MP " /proc/self/mounts)" 1
    check "an archive was written to the suggested place" "$([ -n "$archive" ] && tar -tzf "$archive" | grep -c '^target/etc/fstab$')" 1
    shot recovery-gui-3-logs-done.png

    note "-- Set a new password (dialog; typed twice)"
    old="$(grep '^alice:' "$MP/etc/shadow" | cut -d: -f2)"
    click $BTN_X $Y_PASSWORD 1.5
    xdo type 'gui-Test-pw1'; xdo key Return; sleep 0.3; xdo type 'gui-Test-pw1'
    sleep 0.5
    shot recovery-gui-4-password-dialog.png
    xdo key Return
    sleep 5
    new="$(grep '^alice:' "$MP/etc/shadow" | cut -d: -f2)"
    check "the password of the account offered first (alice, not root) changed" "$([ "$old" != "$new" ] && echo changed)" changed
    check "root's entry is untouched" "$(grep -c '^root:!:' "$MP/etc/shadow")" 1
    check "the typed password verifies against the new hash" "$(perl -e 'print crypt($ARGV[0], $ARGV[1]) eq $ARGV[1] ? "match" : "no match"' 'gui-Test-pw1' "$new")" match
    check "the password is not in the engine log" "$(grep -c 'gui-Test-pw1' "$WORK"/log/*.log)" 0
    check "the password is not in the front-end's output" "$(grep -c 'gui-Test-pw1' "$WORK/gui.log")" 0
    shot recovery-gui-5-password-done.png

    note "-- Repair the boot loader (stand-in tools in the fake system)"
    click $BTN_X $Y_BOOT 7
    check "the three boot steps ran in the installation" "$(tr '\n' ';' < "$MP/var/log/stub-calls.log")" "update-initramfs -u -k all;nexus-grub-install --verbose;update-grub ;"
    shot recovery-gui-6-boot.png

    note "-- Check the file systems (unmounts first)"
    click $BTN_X $Y_CHECK 9
    check "unmounted by the front-end before the check" "$(grep -c " $WORK/state" /proc/self/mounts)" 0
    check "the engine ran the read-only check" "$(grep -c "RUN e2fsck -f -n $ROOT_DEV" "$WORK"/log/*.log)" 1
    check "no repair was run" "$(grep -c "RUN e2fsck -f -y" "$WORK"/log/*.log)" 0
    shot recovery-gui-7-fsck.png

    note "-- Mount / Unmount buttons"
    click $BTN_X $Y_MOUNT 6
    check "Mount" "$(grep -c " $MP " /proc/self/mounts)" 1
    click $BTN_X $Y_MOUNT 6
    check "Unmount" "$(grep -c " $WORK/state" /proc/self/mounts)" 0

    note "-- Unlock… (this kernel has no device-mapper: an honest failure is expected)"
    click $BTN_X $Y_UNLOCK 1.5
    xdo type 'correct horse'; sleep 0.3; xdo key Return
    sleep 5
    check "the engine was asked to unlock, passphrase not logged" "$(grep -c "RUN cryptsetup open --type luks --key-file - $LUKS_DEV .* <stdin: redacted>" "$WORK"/log/*.log)/$(grep -c 'correct horse' "$WORK"/log/*.log)" 1/0
    shot recovery-gui-8-unlock.png
fi

kill "$gui" 2>/dev/null; wait "$gui" 2>/dev/null
kill "$xvfb" 2>/dev/null
grep -E "QML|qml:|Warning|rror" "$WORK/gui.log" | head -n 10
cp "$WORK/gui.log" /shots/nested-logs/recovery/gui-test.log 2>/dev/null
note "== $pass ok, $fail failed"
[ "$fail" -eq 0 ]
