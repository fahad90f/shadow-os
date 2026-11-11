#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Test rig for NEXUS Disk Manager: a private system bus with polkitd, udevd
# and udisksd, and loop devices backed by files in /tmp as the only disks
# that physically exist for the test.
#
# Must run inside ONE "scripts/dev.sh exec" invocation (private mount
# namespace), as root:
#
#   scripts/dev.sh exec --dir disks -- bash /src/applications/disk-manager/tests/udisks-rig.sh <script-or-command> [args...]
#
# Safety measures (the machine's real disks must never be touched):
#   * /dev is replaced, in this mount namespace only, by a tmpfs that holds
#     nothing but the basic character devices and the loop devices this rig
#     creates. The node of the real disk (/dev/vda …) does not exist here, so
#     no tool started from the rig can open it.
#   * NEXUS_DISKS_TEST_LOOP_ONLY=1 is exported: the application back-end then
#     refuses every modifying action on anything that is not a loop device.
#   * rig_guard <device> must be called by test scripts before each modifying
#     action; it aborts unless the device is a loop device created by this rig.
#   * every loop device created here is detached on exit.
set -u

[ "$(id -u)" = 0 ] || { echo "rig: must run as root inside the build chroot" >&2; exit 1; }
[ -d /src/applications/disk-manager ] || { echo "rig: not inside the build chroot (no /src)" >&2; exit 1; }

RIG="$(mktemp -d /tmp/nexus-disks-rig.XXXXXX)"
export RIG
RIG_LOOPS=()
export NEXUS_DISKS_TEST_LOOP_ONLY=1

rig_log() { echo "rig: $*" >&2; }

rig_cleanup() {
    set +e
    rig_log "cleanup"
    # Unmount everything that sits on our loop devices, then detach them.
    for dev in "${RIG_LOOPS[@]}"; do
        base="$(basename "$dev")"
        grep -E "^/dev/(${base}|${base}p[0-9]+|dm-[0-9]+) " /proc/mounts | awk '{print $2}' | sort -r | while read -r mp; do umount -l "$mp"; done
    done
    for dm in /sys/class/block/dm-*; do
        [ -e "$dm" ] || continue
        for dev in "${RIG_LOOPS[@]}"; do
            base="$(basename "$dev")"
            if ls "$dm/slaves" 2>/dev/null | grep -qE "^${base}(p[0-9]+)?$"; then
                name="$(cat "$dm/dm/name")"
                umount -l "/dev/mapper/$name" 2>/dev/null
                cryptsetup close "$name" 2>/dev/null || dmsetup remove "$name" 2>/dev/null
            fi
        done
    done
    [ -n "${UDISKS_PID:-}" ] && kill "$UDISKS_PID" 2>/dev/null
    [ -n "${POLKIT_PID:-}" ] && kill "$POLKIT_PID" 2>/dev/null
    for dev in "${RIG_LOOPS[@]}"; do
        losetup -d "$dev" 2>/dev/null && rig_log "detached $dev"
    done
    [ -n "${UDEVD_PID:-}" ] && kill "$UDEVD_PID" 2>/dev/null
    [ -n "${BUS_PID:-}" ] && kill "$BUS_PID" 2>/dev/null
    sleep 0.3
    for dev in "${RIG_LOOPS[@]}"; do
        if [ -n "$(losetup -n -O BACK-FILE "$dev" 2>/dev/null)" ]; then rig_log "WARNING: $dev is still attached"; fi
    done
    rm -rf "$RIG"
}
trap rig_cleanup EXIT

# ---- private /dev -----------------------------------------------------------
mount -t tmpfs -o mode=755,nosuid tmpfs /dev || exit 1
mknod -m 666 /dev/null c 1 3; mknod -m 666 /dev/zero c 1 5; mknod -m 666 /dev/full c 1 7
mknod -m 666 /dev/random c 1 8; mknod -m 666 /dev/urandom c 1 9; mknod -m 666 /dev/tty c 5 0
mknod -m 660 /dev/loop-control c 10 237
[ -e /sys/class/misc/device-mapper/dev ] && { mkdir -p /dev/mapper; mknod -m 600 /dev/mapper/control c $(tr ':' ' ' < /sys/class/misc/device-mapper/dev); }
mkdir -p /dev/shm /dev/pts; mount -t tmpfs tmpfs /dev/shm; mount -t devpts devpts /dev/pts 2>/dev/null
ln -s /proc/self/fd /dev/fd
# udev needs a writable sysfs (uevent files) — private to this namespace.
mount -t sysfs sysfs /sys 2>/dev/null || mount -o remount,rw /sys

# ---- udev: creates/removes the nodes of loop and device-mapper devices only --
mkdir -p /run/udev/rules.d
cat > /run/udev/rules.d/00-nexus-rig.rules <<'RULES'
SUBSYSTEM=="block", KERNEL=="loop*|dm-*", ACTION=="add|change", RUN+="/bin/sh -c 'test -b /dev/%k || mknod -m 660 /dev/%k b %M %m'"
SUBSYSTEM=="block", KERNEL=="loop*|dm-*", ACTION=="remove", RUN+="/bin/rm -f /dev/%k"
RULES
/usr/lib/systemd/systemd-udevd --daemon >"$RIG/udevd.log" 2>&1
UDEVD_PID="$(pgrep -n -f 'systemd-udevd --daemon')"
rig_log "udevd pid ${UDEVD_PID:-?}"

# ---- private system bus, polkitd, udisksd ----------------------------------
cat > "$RIG/system.conf" <<CONF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN" "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>system</type>
  <listen>unix:path=$RIG/system_bus_socket</listen>
  <auth>EXTERNAL</auth>
  <policy context="default">
    <allow user="*"/><allow own="*"/>
    <allow send_type="method_call"/><allow send_type="signal"/>
    <allow send_type="method_return"/><allow send_type="error"/>
    <allow receive_type="method_call"/><allow receive_type="signal"/>
    <allow receive_type="method_return"/><allow receive_type="error"/>
  </policy>
</busconfig>
CONF
chmod 755 "$RIG"
BUS_PID="$(dbus-daemon --config-file="$RIG/system.conf" --fork --print-pid)"
export DBUS_SYSTEM_BUS_ADDRESS="unix:path=$RIG/system_bus_socket"
mkdir -p /run/dbus; ln -sf "$RIG/system_bus_socket" /run/dbus/system_bus_socket

if id polkitd >/dev/null 2>&1; then
    setpriv --reuid polkitd --regid polkitd --init-groups /usr/lib/polkit-1/polkitd --no-debug >"$RIG/polkitd.log" 2>&1 &
else
    /usr/lib/polkit-1/polkitd --no-debug >"$RIG/polkitd.log" 2>&1 &
fi
POLKIT_PID=$!
mkdir -p /run/udisks2 /var/lib/udisks2 /run/media /run/mount /run/cryptsetup
/usr/libexec/udisks2/udisksd --no-debug >"$RIG/udisksd.log" 2>&1 &
UDISKS_PID=$!
rig_has_name() { dbus-send --system --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus.NameHasOwner "string:$1" 2>/dev/null | grep -q "boolean true"; }
for _ in $(seq 50); do
    rig_has_name org.freedesktop.UDisks2 && break
    sleep 0.2
done
rig_has_name org.freedesktop.UDisks2 || { rig_log "udisksd did not appear on the bus"; cat "$RIG/udisksd.log" >&2; exit 1; }
rig_log "udisksd is on the private system bus"
rig_has_name org.freedesktop.PolicyKit1 && rig_log "polkitd running" || rig_log "polkitd NOT on the bus (see $RIG/polkitd.log)"

# rig_loop <size-MiB> — create a loop device backed by a sparse file. The
# device name is returned in $RIG_DEV (not on stdout: the function must run in
# this shell, not in a command substitution, so that the device is recorded
# for the guard and for detaching).
rig_loop() {
    local size="$1" file dev n
    file="$(mktemp "$RIG/disk.XXXXXX.img")"
    truncate -s "${size}M" "$file"
    [ "$BASHPID" = "$$" ] || { rig_log "rig_loop must not be called in a subshell"; return 1; }
    dev="$(losetup -f)" || return 1
    dev="${dev%% *}"            # "/dev/loopN (lost)" while the node does not exist yet
    n="${dev#/dev/loop}"
    case "$n" in ''|*[!0-9]*) rig_log "unexpected free loop device '$dev'"; return 1 ;; esac
    [ -b "$dev" ] || mknod -m 660 "$dev" b 7 "$n"
    losetup -P "$dev" "$file" || return 1
    RIG_LOOPS+=("$dev")
    udevadm settle --timeout=10 2>/dev/null
    RIG_DEV="$dev"
    rig_log "created $dev (${size} MiB, $file)"
}

# rig_guard <device> — abort unless <device> is (a partition of) a loop device of this rig.
rig_guard() {
    local dev="$1" l
    for l in "${RIG_LOOPS[@]}"; do
        case "$dev" in
            "$l"|"$l"p[0-9]*) return 0 ;;
        esac
    done
    echo "rig: REFUSING to operate on '$dev': not a loop device created by this rig" >&2
    exit 97
}

if [ $# -ge 1 ] && [ -f "$1" ]; then
    script="$1"; shift
    . "$script"
else
    "$@"
fi
