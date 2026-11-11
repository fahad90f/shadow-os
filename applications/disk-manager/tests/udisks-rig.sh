#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Test rig for SHADOW Disk Manager: a private system bus with polkitd, udevd
# and udisksd, and loop devices backed by files in /tmp as the only disks
# that physically exist for the test.
#
# Must run inside ONE "scripts/dev.sh exec" invocation (private mount
# namespace), as root:
#
#   scripts/dev.sh exec --dir disks -- bash /src/applications/disk-manager/tests/udisks-rig.sh <script-or-command> [args...]
#
# Safety measures (the machine's real disks must never be touched):
#   * In this mount namespace only, the device node of every block device
#     that is not a loop device (/dev/vda …) is covered by an empty regular
#     file, so no tool started from the rig can open a real disk.
#   * SHADOW_DISKS_TEST_LOOP_ONLY=1 is exported: the application back-end then
#     refuses every modifying action on anything that is not a loop device.
#   * rig_guard <device> must be called by test scripts before each modifying
#     action; it aborts unless the device is a loop device created by this rig.
#   * every loop device created here is detached on exit.
set -u

[ "$(id -u)" = 0 ] || { echo "rig: must run as root inside the build chroot" >&2; exit 1; }
[ -d /src/applications/disk-manager ] || { echo "rig: not inside the build chroot (no /src)" >&2; exit 1; }

RIG="$(mktemp -d /tmp/shadow-disks-rig.XXXXXX)"
export RIG
RIG_LOOPS=()
export SHADOW_DISKS_TEST_LOOP_ONLY=1

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
    udevadm settle --timeout=5 2>/dev/null
    [ -n "${UDEVD_PID:-}" ] && kill "$UDEVD_PID" 2>/dev/null
    # udev's /dev/block/MAJ:MIN links for our devices live in the shared devtmpfs: remove them.
    if [ -d /dev/block ]; then
        for link in /dev/block/*; do
            [ -L "$link" ] || continue
            for dev in "${RIG_LOOPS[@]}"; do
                case "$(readlink "$link")" in "../$(basename "$dev")"|"../$(basename "$dev")p"[0-9]*) rm -f "$link" ;; esac
            done
        done
        [ "${DEV_BLOCK_EXISTED:-1}" = 0 ] && rmdir /dev/block 2>/dev/null
    fi
    [ -n "${BUS_PID:-}" ] && kill "$BUS_PID" 2>/dev/null
    sleep 0.3
    for dev in "${RIG_LOOPS[@]}"; do
        if [ -n "$(losetup -n -O BACK-FILE "$dev" 2>/dev/null)" ]; then rig_log "WARNING: $dev is still attached"; fi
    done
    rm -rf "$RIG"
}
trap rig_cleanup EXIT

# ---- /dev: real disks are masked ---------------------------------------------
# /dev stays the kernel's devtmpfs (partition nodes must appear the moment
# the kernel creates them), but in this private mount namespace every block
# device node that is not a loop device is covered by an empty regular file.
: > "$RIG/masked"
for node in /dev/*; do
    [ -b "$node" ] || continue
    case "$(basename "$node")" in loop*) continue ;; esac
    mount --bind "$RIG/masked" "$node" || { rig_log "cannot mask $node — aborting"; exit 1; }
    rig_log "masked $node"
done
for node in /dev/*; do
    [ -b "$node" ] || continue
    case "$(basename "$node")" in loop*) ;; *) rig_log "unmasked block device $node — aborting"; exit 1 ;; esac
done
DEV_BLOCK_EXISTED=0; [ -d /dev/block ] && DEV_BLOCK_EXISTED=1
# udev needs a writable sysfs (uevent files) — private to this namespace.
mount -t sysfs sysfs /sys 2>/dev/null || mount -o remount,rw /sys

# ---- udev: only what UDisks needs, and only for loop devices -----------------
# The distribution's rules are hidden (they would create /dev/disk/by-*
# links and change node permissions in the machine's shared /dev); the one
# rule here just probes loop devices with blkid.
mkdir -p "$RIG/no-rules" /run/udev/rules.d
mount --bind "$RIG/no-rules" /usr/lib/udev/rules.d
[ -d /etc/udev/rules.d ] && mount --bind "$RIG/no-rules" /etc/udev/rules.d
cat > /run/udev/rules.d/60-shadow-rig.rules <<'RULES'
SUBSYSTEM=="block", KERNEL=="loop*", IMPORT{builtin}="blkid"
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
    dev="$(losetup -f --show -P "$file")" || return 1
    case "$dev" in /dev/loop[0-9]*) ;; *) rig_log "unexpected loop device '$dev'"; return 1 ;; esac
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
