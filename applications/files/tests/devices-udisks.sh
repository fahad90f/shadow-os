#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Storage devices against a real UDisks2 daemon on a private "system" bus
# inside the build chroot (which has no system bus of its own):
#
#   scripts/dev.sh exec --dir files -- bash /src/applications/files/tests/devices-udisks.sh
#
# Prints `nexus-files --dump devices` without a system bus, with a bus but no
# UDisks2, and with udisksd running; compares the latter with udisksctl.
# Nothing is mounted. The bus and the daemon are stopped again at the end.
W="$(mktemp -d /tmp/files-test-udisks.XXXXXX)"
export HOME="$W/home" XDG_RUNTIME_DIR="$W/run"; mkdir -p "$HOME"; mkdir -m 700 "$XDG_RUNTIME_DIR"
export QT_LOGGING_RULES="*.debug=false;kf.*=false"
cat > "$W/bus.conf" <<CONF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>system</type>
  <listen>unix:path=$W/bus.sock</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow user="*"/><allow own="*"/><allow send_type="method_call"/><allow send_type="signal"/>
    <allow send_type="method_return"/><allow send_type="error"/><allow receive_type="method_call"/>
    <allow receive_type="signal"/><allow receive_type="method_return"/><allow receive_type="error"/>
  </policy>
</busconfig>
CONF
summary() { python3 -c 'import json,sys
d=json.load(sys.stdin)
print("  emptyReason:", repr(d["emptyReason"]))
for v in d["devices"]: print("  volume:", v["device"], v["fsType"], "mounted" if v["mounted"] else "not mounted", v["mountPoint"], v["label"])'; }

echo "== 1. no system bus"
DBUS_SYSTEM_BUS_ADDRESS="unix:path=$W/none.sock" nexus-files --dump devices | summary

dbus-daemon --config-file="$W/bus.conf" --nofork --nopidfile > "$W/bus.log" 2>&1 &
bus=$!
for i in $(seq 50); do [ -S "$W/bus.sock" ] && break; sleep 0.1; done
export DBUS_SYSTEM_BUS_ADDRESS="unix:path=$W/bus.sock"
echo "== 2. system bus without UDisks2"
nexus-files --dump devices | summary

/usr/libexec/udisks2/udisksd > "$W/udisksd.log" 2>&1 &
ud=$!
owned() { dbus-send --system --print-reply --dest=org.freedesktop.DBus / org.freedesktop.DBus.NameHasOwner string:org.freedesktop.UDisks2 2>/dev/null | grep -q "boolean true"; }
for i in $(seq 80); do owned && break; sleep 0.25; done
echo "== 3. udisksd running: $(owned && echo yes || echo NO)"
echo "  udisksctl block devices: $(udisksctl dump 2>/dev/null | grep -c '^/org/freedesktop/UDisks2/block_devices/')"
echo "  udisksctl file systems : $(udisksctl dump 2>/dev/null | grep -c 'org.freedesktop.UDisks2.Filesystem:')"
nexus-files --dump devices | summary

kill "$ud" "$bus" 2>/dev/null; wait 2>/dev/null
tail -3 "$W/udisksd.log" | sed 's/^/  udisksd: /'
rm -rf "$W"
