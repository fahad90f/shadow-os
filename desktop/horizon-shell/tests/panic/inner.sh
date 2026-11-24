#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds and runs the panic test (test.cpp) INSIDE a build chroot (run.sh
# does that; any other chroot with the build dependencies will do, given SRC
# and WORK as below). Run as root: it installs the
# stand-in for an authorised pkexec (root-pkexec.c) setuid root in WORK, then
# runs everything else as the unprivileged TEST_UID (default 65534), so that
# the shell's kill() of that pkexec fails with EPERM as it does for the real
# one. The setuid file is deleted at the end.
#
#   inner.sh SRC WORK [PANIC_SRC_DIR]
#
# SRC: the source tree; WORK: an empty directory on a file system WITHOUT
# nosuid (otherwise the authorised-release case is skipped); PANIC_SRC_DIR:
# other panic.cpp/panic.h to test (default: SRC's own).
# Needs: cmake, ninja, a C/C++ compiler, Qt 6 (Core, DBus, Gui, Qml),
# dbus-daemon, python3-gi, setpriv (util-linux).
# Nothing touches a network: the "system" bus is a private dbus-daemon.
set -euo pipefail
SRC=$1; WORK=$2; PANIC_SRC=${3:-$SRC/desktop/horizon-shell/src}
U=${TEST_UID:-65534}
T="$SRC/desktop/horizon-shell/tests/panic"
FAKES="$SRC/desktop/horizon-shell/modules/status/tests/fake-panic-services.py"
[ "$(id -u)" = 0 ] || { echo "inner.sh: run as root" >&2; exit 2; }

cmake -S "$T" -B "$WORK/build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DPANIC_SRC_DIR="$PANIC_SRC" \
    > "$WORK/cmake.log" 2>&1 || { tail -20 "$WORK/cmake.log"; exit 1; }
ninja -C "$WORK/build" > "$WORK/build.log" 2>&1 || { grep -E "error|Error" "$WORK/build.log" | head -20; exit 1; }

R="$WORK/run"
rm -rf "$R"
mkdir -p "$R/root/run" "$R/fakebin" "$R/home"
chmod 755 "$R" "$R/root"
cleanup() { rm -f "$R/root/pkexec"; }
trap cleanup EXIT
install -o 0 -g 0 -m 4755 "$WORK/build/root-pkexec" "$R/root/pkexec"
chown "$U:$U" "$R/root/run" "$R/fakebin" "$R/home"
: > "$R/root/pkexec-root.log"
ROOT_PKEXEC="$R/root/pkexec"
if ! setpriv --reuid="$U" --regid="$U" --clear-groups -- "$ROOT_PKEXEC" --probe 2>/dev/null \
        || ! grep -q 'euid 0' "$R/root/pkexec-root.log"; then
    echo "SKIP: setuid root does not work in $R (nosuid?): the authorised-release case is skipped"
    ROOT_PKEXEC=""
fi

setpriv --reuid="$U" --regid="$U" --clear-groups -- env -i PATH="$R/fakebin:/usr/bin:/bin" HOME="$R/home" \
    LANG=C.UTF-8 R="$R" FAKES="$FAKES" BIN="$WORK/build/panic-test" ROOT_PKEXEC="$ROOT_PKEXEC" /bin/bash -euc '
    cd "$R/home"
    dbus-daemon --session --address="unix:path=$R/home/system-bus" --nofork --nopidfile > "$R/home/bus.log" 2>&1 &
    bus=$!
    sleep 1
    mkfifo "$R/home/fake.ctl"
    DBUS_SYSTEM_BUS_ADDRESS="unix:path=$R/home/system-bus" python3 "$FAKES" --control "$R/home/fake.ctl" \
        --log "$R/home/fake-services.log" --run-dir "$R/root/run" > "$R/home/fake-services.out" 2>&1 &
    fakes=$!
    for _ in $(seq 50); do grep -q ready "$R/home/fake-services.log" 2>/dev/null && break; sleep 0.1; done
    printf "{\"lock_session\": false, \"clear_clipboard\": false}\n" > "$R/home/panic-config.json"
    rc=0
    DBUS_SYSTEM_BUS_ADDRESS="unix:path=$R/home/system-bus" SHADOW_PANIC_CONFIG="$R/home/panic-config.json" \
        TEST_RUN_DIR="$R/root/run" TEST_FAKE_LOG="$R/home/fake-services.log" TEST_FAKE_CTL="$R/home/fake.ctl" \
        TEST_FAKEBIN="$R/fakebin" TEST_WORK="$R/home" TEST_ROOT_PKEXEC="$ROOT_PKEXEC" \
        TEST_ROOT_LOG="$R/root/pkexec-root.log" QT_QPA_PLATFORM=offscreen "$BIN" || rc=$?
    kill "$fakes" "$bus" 2>/dev/null || true
    exit "$rc"
'
