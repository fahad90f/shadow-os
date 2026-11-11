# SPDX-License-Identifier: GPL-3.0-or-later
#
# Screenshots of the Disk Manager window showing real UDisks data for loop
# devices. Sourced by udisks-rig.sh:
#
#   scripts/dev.sh exec --dir disks -- bash /src/applications/disk-manager/tests/udisks-rig.sh /src/applications/disk-manager/tests/gui-shots.sh
#
# The window runs on a private Xvfb; "click X Y" sends real mouse input.
CTL=nexus-disks-testctl
MIB=1048576
rig_loop 2048; A="$RIG_DEV"
rig_loop 64;   B="$RIG_DEV"
rig_guard "$A"; $CTL table "$A" gpt
rig_guard "$A"; $CTL create "$A" $MIB $((600*MIB)) ext4 Projects
rig_guard "$A"; $CTL create "$A" $((601*MIB)) $((400*MIB)) btrfs Backups
rig_guard "$A"; $CTL create "$A" $((1001*MIB)) $((256*MIB)) swap
$CTL mount "${A}p1"
mp="$(grep "^${A}p1 " /proc/mounts | awk '{print $2}')"
dd if=/dev/urandom of="$mp/data.bin" bs=1M count=180 status=none; sync
rig_guard "$B"; $CTL format "$B" ext4 Stick
udevadm settle

export HOME="$RIG/home"; mkdir -p "$HOME/.config/nexus" "$HOME/Documents"
export XDG_RUNTIME_DIR="$RIG/run"; mkdir -m 700 -p "$XDG_RUNTIME_DIR"
export QT_QPA_PLATFORM=xcb LIBGL_ALWAYS_SOFTWARE=1 QT_QUICK_CONTROLS_STYLE=org.nexus.style QT_LOGGING_RULES="qt.qpa.*=false"
export DISPLAY=":$(( 700 + RANDOM % 200 ))"
Xvfb "$DISPLAY" -screen 0 1280x900x24 -nolisten tcp >"$RIG/xvfb.log" 2>&1 &
XVFB=$!
sleep 1.5
snap() { import -window root "/shots/$1"; echo "shot $1"; }
click() { xdotool mousemove "$1" "$2" click 1; sleep "${3:-1.2}"; }

NEXUS_WINDOW_SIZE=1280x900 dbus-run-session -- nexus-disks >"$RIG/app.log" 2>&1 &
APP=$!
sleep 6
xdotool search --name "Disk Manager" windowmove 0 0 2>/dev/null
sleep 1
snap disks-dm-1-main.png
if [ -n "${GUI_STEPS:-}" ]; then . "$GUI_STEPS"; fi
kill $APP 2>/dev/null; wait $APP 2>/dev/null
grep -v "^$" "$RIG/app.log" | head -20
kill $XVFB 2>/dev/null
