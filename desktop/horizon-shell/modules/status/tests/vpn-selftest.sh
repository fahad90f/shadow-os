#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# vpn-selftest.sh [BIN_DIR]
#
# Runs status-test-vpn (the VPN part of the status module's network back-end)
# against status-test-dbus-fixture on a private message bus that the test is
# told is the system bus. BIN_DIR holds both programs (default: the directory
# of this build's tests, /build/modstatus-tests/bin). Also checks what the
# fixture was asked to do. Exit status 0 when everything holds; 77 (skipped)
# without dbus-daemon.
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bin="${1:-/build/modstatus-tests/bin}"
command -v dbus-daemon >/dev/null || { echo "SKIP: dbus-daemon is not installed"; exit 77; }
work="$(mktemp -d)"
trap 'kill $fixture $bus 2>/dev/null; rm -rf "$work"' EXIT
dbus-daemon --session --address="unix:path=$work/bus" --nofork --nopidfile > "$work/bus.log" 2>&1 &
bus=$!
for _ in $(seq 50); do [ -S "$work/bus" ] && break; sleep 0.1; done
export DBUS_SYSTEM_BUS_ADDRESS="unix:path=$work/bus"
"$bin/status-test-dbus-fixture" "$here/fixture-laptop.json" > "$work/fixture.log" 2>&1 &
fixture=$!
for _ in $(seq 50); do grep -q '^ready' "$work/fixture.log" 2>/dev/null && break; sleep 0.1; done
QT_QPA_PLATFORM=offscreen "$bin/status-test-vpn"
status=$?
S=/org/freedesktop/NetworkManager/Settings
expected="client-call ActivateConnection $S/6
client-call DeactivateConnection /org/freedesktop/NetworkManager/ActiveConnection/100
client-call ActivateConnection $S/5
client-call DeactivateConnection /org/freedesktop/NetworkManager/ActiveConnection/101
client-call ActivateConnection $S/7"
if [ "$(grep '^client-call' "$work/fixture.log")" != "$expected" ]; then
    echo "FAIL: what NetworkManager was asked to do:"; grep '^client-call' "$work/fixture.log"
    status=1
fi
exit "$status"
