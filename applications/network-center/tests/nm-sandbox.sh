#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Developer test aid, not installed. Runs a command against a REAL NetworkManager
# without touching the machine's network: a private network + mount namespace
# with only lo and a veth pair (nctest0 <-> ncpeer0, 10.77.0.1/24 on the peer),
# a private system bus, and tmpfs over every place NetworkManager writes to.
#
#   scripts/dev.sh exec --dir netcenter -- unshare -n -m --propagation unchanged \
#       /src/applications/network-center/tests/nm-sandbox.sh <command...>
set -u
mount -t tmpfs tmpfs /run 2>/dev/null
mkdir -p /run/dbus /run/NetworkManager /tmp/nc-nm
mount -t tmpfs tmpfs /etc/NetworkManager/system-connections || exit 90
mount -t tmpfs tmpfs /var/lib/NetworkManager || exit 91
mkdir -p /tmp/nc-nm; : > /tmp/nc-nm/resolv.conf
mount --bind /tmp/nc-nm/resolv.conf /etc/resolv.conf || exit 92
ip link set lo up
ip link add nctest0 type veth peer name ncpeer0
ip link set ncpeer0 up
ip addr add 10.77.0.1/24 dev ncpeer0
dbus-daemon --system --fork --print-pid > /tmp/nc-nm/dbus.pid 2>/tmp/nc-nm/dbus.err
sleep 0.5
NetworkManager --no-daemon --log-level=INFO > /tmp/nc-nm/nm.log 2>&1 &
NMPID=$!
for i in $(seq 1 40); do nmcli -t general status >/dev/null 2>&1 && break; sleep 0.5; done
"$@"; rc=$?
kill $NMPID 2>/dev/null; wait $NMPID 2>/dev/null
kill "$(cat /tmp/nc-nm/dbus.pid)" 2>/dev/null
exit $rc
