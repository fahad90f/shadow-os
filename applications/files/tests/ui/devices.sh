# SPDX-License-Identifier: GPL-3.0-or-later
# The Devices section driven through Solid's fake hardware backend
# (ui/fakehw.xml): list, mount on click, unmount, eject, context menu. This
# exercises the application's device handling, not UDisks2 itself — see
# tests/devices-udisks.sh for the real daemon.
T=/tmp/files-test-devices; rm -rf "$T"; mkdir -p "$T/usb" "$T/data" "$T/disc" "$T/work"
echo on-the-stick > "$T/usb/on-the-stick.txt"; echo data > "$T/data/data.txt"; echo disc > "$T/disc/disc.txt"
export SOLID_FAKEHW=/src/applications/files/tests/ui/fakehw.xml
echo "dump: $(nexus-files --dump devices | python3 -c 'import json,sys
for v in json.load(sys.stdin)["devices"]: print(v["label"], v["device"], v["fsType"], "mounted" if v["mounted"] else "unmounted", "removable" if v["removable"] else "fixed", "optical" if v["optical"] else "", end="; ")')"
app_start "$T/work"
snap 1-list
click 70 280; sleep 2; snap 2-stick-mounted            # STICK: mount, then open its mount point
click 218 280; sleep 2; snap 3-stick-removed           # its eject button: unmount
click 218 238; sleep 2; snap 4-data-unmounted          # Data: unmount
click 70 196 3; sleep 0.8; snap 5-disc-menu; key Escape
click 218 196; sleep 2; snap 6-disc-ejected            # optical disc: eject
click 70 238 3; sleep 0.8; snap 7-data-menu; key Escape
app_stop
