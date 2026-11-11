# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sourced by engine-test.sh and gui-test.sh: builds a fake installed NEXUS OS
# on LOOP DEVICES backed by files in a fresh temporary directory, and removes
# all of it again when the script ends.
#
# Safety: the engine is only ever started with NEXUS_RECOVERY_TEST_DEVICES set
# to loop devices that this file attached and whose backing files lie in its
# own work directory (checked again before every use). In that mode the
# engine lists no other block device, so nothing else can become a target.
# State and log go to the work directory, not to /run and /var/log.
#
# Provides: $WORK, $ENGINE, $ROOT_DEV $ESP_DEV $LUKS_DEV $OTHER_DEV $FAT_DEV,
# $ROOT_UUID, $ESP_FS, $LUKS_OK, and the functions note, guard, engine.
ENGINE="${ENGINE:-/src/system/recovery/engine/nexus-recovery-engine}"
WORK="$(mktemp -d /tmp/nexus-recovery-test.XXXXXX)"
export NEXUS_RECOVERY_STATE_DIR="$WORK/state" NEXUS_RECOVERY_LOG_DIR="$WORK/log"
export PYTHONDONTWRITEBYTECODE=1

note() { echo "$*"; }

# ---- guards ------------------------------------------------------------------------
attach() {      # attach <image> → loop device, remembered and verified
    local device
    device="$(losetup --find --show "$1")" || { echo "losetup failed" >&2; exit 1; }
    guard "$device"
    echo "$device" >> "$WORK/loops"     # (attach runs in a subshell: a file, not a variable)
    echo "$device"
}
guard() {       # die unless $1 is a loop device backed by a file of ours
    case "$1" in /dev/loop[0-9]*) ;; *) echo "GUARD: $1 is not a loop device" >&2; exit 99 ;; esac
    local backing
    backing="$(losetup -n -O BACK-FILE "$1" 2>/dev/null | sed 's/ *$//')"
    case "$backing" in "$WORK"/*) ;; *) echo "GUARD: $1 is not backed by a file in $WORK ($backing)" >&2; exit 99 ;; esac
}
engine() {      # the only way this script starts the engine
    local device
    [ -n "${NEXUS_RECOVERY_TEST_DEVICES:-}" ] || { echo "GUARD: no test devices set" >&2; exit 99; }
    for device in ${NEXUS_RECOVERY_TEST_DEVICES//,/ }; do guard "$device"; done
    python3 "$ENGINE" "$@"
}
result() {      # result <json-lines> <path.to.field>  — from the last ("result") line
    tail -n 1 <<<"$1" | python3 -c 'import json,sys
value = json.loads(sys.stdin.read())
try:
    for key in sys.argv[1].split("."):
        value = value[int(key)] if isinstance(value, list) else value[key]
except (KeyError, IndexError, TypeError):
    value = "<missing>"
print(value if isinstance(value, str) else json.dumps(value))' "$2"
}
cleanup() {
    local device where
    # innermost first; only mounts below our work directory
    for where in $(awk -v w="$WORK" 'index($2, w) == 1 { print $2 }' /proc/self/mounts | sort -r); do
        umount -R "$where" 2>/dev/null || umount -l "$where" 2>/dev/null
    done
    for device in $(cat "$WORK/loops" 2>/dev/null); do
        case "$(losetup -n -O BACK-FILE "$device" 2>/dev/null)" in "$WORK"/*) losetup -d "$device" ;; esac
    done
    if grep -q " $WORK" /proc/self/mounts; then
        echo "WARNING: something is still mounted below $WORK; it is left in place" >&2
    else
        rm -rf --one-file-system "$WORK"
    fi
}
trap cleanup EXIT

# ---- the fake installed system -----------------------------------------------------
copy_program() {    # copy a program and the libraries it needs into the fake root
    local program="$1" root="$2" lib
    mkdir -p "$root$(dirname "$program")"
    cp -L "$program" "$root$program"
    for lib in $(ldd "$program" 2>/dev/null | grep -o '/[^ ]*'); do
        mkdir -p "$root$(dirname "$lib")"
        [ -e "$root$lib" ] || cp -L "$lib" "$root$lib"
    done
}
stub() {            # a stand-in for a tool the chroot cannot really run: logs its arguments
    cat > "$1" <<'STUB'
#!/bin/sh
name="$(basename "$0")"
echo "$name $*" >> /var/log/stub-calls.log
echo "[$name] called with: $*"
[ -e "/etc/stub-fail-$name" ] && { echo "[$name] failing as the test asked"; exit 1; }
exit 0
STUB
    chmod +x "$1"
}

truncate -s 96M "$WORK/root.img"; truncate -s 40M "$WORK/esp.img"; truncate -s 24M "$WORK/luks.img"
truncate -s 48M "$WORK/other.img"; truncate -s 40M "$WORK/fat.img"
ROOT_DEV="$(attach "$WORK/root.img")"; ESP_DEV="$(attach "$WORK/esp.img")"
LUKS_DEV="$(attach "$WORK/luks.img")"; OTHER_DEV="$(attach "$WORK/other.img")"; FAT_DEV="$(attach "$WORK/fat.img")"
export NEXUS_RECOVERY_TEST_DEVICES="$ROOT_DEV,$ESP_DEV,$LUKS_DEV,$OTHER_DEV,$FAT_DEV"
note "loop devices: root=$ROOT_DEV esp=$ESP_DEV luks=$LUKS_DEV other=$OTHER_DEV fat=$FAT_DEV (work: $WORK)"

guard "$ROOT_DEV"; mkfs.ext4 -q -F -L nexus-root "$ROOT_DEV"
# The ESP is FAT. A kernel without vfat (the build host's) cannot mount one, so
# there the "ESP" of the fake system is ext4 and FAT is only checked (fsck.vfat
# needs no kernel support) on a second, data file system.
guard "$ESP_DEV"
if grep -qw vfat /proc/filesystems; then ESP_FS=vfat; mkfs.vfat -F 32 -n ESP "$ESP_DEV" > /dev/null
else ESP_FS=ext4; mkfs.ext4 -q -F -L ESP "$ESP_DEV"; note "this kernel has no vfat: the fake ESP is ext4"; fi
guard "$FAT_DEV"; mkfs.vfat -F 32 -n FATDATA "$FAT_DEV" > /dev/null
guard "$OTHER_DEV"; mkfs.ext4 -q -F -L data "$OTHER_DEV"       # a data file system, not a system
ROOT_UUID="$(blkid -p -s UUID -o value "$ROOT_DEV")"; ESP_UUID="$(blkid -p -s UUID -o value "$ESP_DEV")"
FAT_UUID="$(blkid -p -s UUID -o value "$FAT_DEV")"
LUKS_OK=no
guard "$LUKS_DEV"
if echo -n "correct horse" | cryptsetup luksFormat --type luks2 --batch-mode --pbkdf pbkdf2 --pbkdf-force-iterations 1000 --key-file - "$LUKS_DEV" 2>/dev/null; then LUKS_OK=yes; fi

mkdir "$WORK/build"; mount "$ROOT_DEV" "$WORK/build"
R="$WORK/build"
mkdir -p "$R"/{etc/nexus,etc/pam.d,usr/lib,usr/bin,usr/sbin,boot/efi,dev,proc,sys,run,root,tmp,var/log/installer,var/log/apt,home/alice}
ln -s usr/bin "$R/bin"; ln -s usr/sbin "$R/sbin"; ln -s usr/lib "$R/lib"; ln -s usr/lib64 "$R/lib64"; mkdir -p "$R/usr/lib64"
printf 'PRETTY_NAME="NEXUS OS 1.0 (test)"\nNAME="NEXUS OS"\nVERSION_ID="1.0"\nID=nexus\nID_LIKE=debian\n' > "$R/usr/lib/os-release"
ln -s ../usr/lib/os-release "$R/etc/os-release"            # as on Debian: a relative link
echo "recovery-test-host" > "$R/etc/hostname.real"
ln -s /etc/hostname.real "$R/etc/hostname"                 # an absolute link: must resolve INSIDE the target
printf 'UUID=%s / ext4 errors=remount-ro 0 1\nUUID=%s /boot/efi %s defaults 0 2\nUUID=00000000-0000-0000-0000-00000000dead /var xfs defaults 0 2\nUUID=%s /data vfat defaults 0 2\n/swapfile none swap sw 0 0\n' \
    "$ROOT_UUID" "$ESP_UUID" "$ESP_FS" "$FAT_UUID" > "$R/etc/fstab"
printf 'FIRMWARE=uefi\nESP_MOUNT=/boot/efi\nESP_REMOVABLE=yes\nESP_NVRAM=no\n' > "$R/etc/nexus/boot.conf"
printf 'root:x:0:0:root:/root:/bin/bash\ndaemon:x:1:1:daemon:/usr/sbin:/usr/sbin/nologin\nalice:x:1000:1000:Alice Example,,,:/home/alice:/bin/bash\nsvc:x:1001:1001::/nonexistent:/usr/sbin/nologin\n' > "$R/etc/passwd"
printf 'root:!:19000:0:99999:7:::\ndaemon:*:19000:0:99999:7:::\nalice:$6$oldsalt$oldhasholdhasholdhash:19000:0:99999:7:::\nsvc:!:19000::::::\n' > "$R/etc/shadow"
chmod 600 "$R/etc/shadow"
printf 'root:x:0:\nalice:x:1000:\n' > "$R/etc/group"
printf 'passwd: files\ngroup: files\nshadow: files\n' > "$R/etc/nsswitch.conf"
printf 'password required pam_unix.so sha512\n' > "$R/etc/pam.d/chpasswd"
printf 'password required pam_unix.so sha512\n' > "$R/etc/pam.d/other"
cp /etc/login.defs "$R/etc/login.defs" 2>/dev/null
echo "2026-01-01 installer: test log line" > "$R/var/log/installer/install-20260101.log"
echo "Start-Date: 2026-01-02  10:00:00" > "$R/var/log/apt/history.log"
# Real programs the tests run in the chroot: a shell and chpasswd (with PAM).
for program in /bin/sh /bin/bash /usr/sbin/chpasswd /usr/bin/cat /usr/bin/basename; do copy_program "$(readlink -f "$program")" "$R"; done
[ -e "$R/usr/bin/sh" ] || ln -s "$(basename "$(readlink -f /bin/sh)")" "$R/usr/bin/sh"
PAMDIR="$(dirname "$(find /usr/lib -name pam_unix.so | head -n1)")"
mkdir -p "$R$PAMDIR"; cp -L "$PAMDIR/pam_unix.so" "$R$PAMDIR/"
for lib in $(ldd "$PAMDIR/pam_unix.so" | grep -o '/[^ ]*') $(find /usr/lib -name 'libnss_files*' | head -n2); do
    mkdir -p "$R$(dirname "$lib")"; [ -e "$R$lib" ] || cp -L "$lib" "$R$lib"
done
# Stand-ins (they only record how they were called) for what cannot run here.
for tool in update-initramfs nexus-grub-install update-grub; do stub "$R/usr/sbin/$tool"; done
for tool in dpkg apt-get; do stub "$R/usr/bin/$tool"; done
umount "$WORK/build"

mkdir "$WORK/esp"; mount "$ESP_DEV" "$WORK/esp"; mkdir -p "$WORK/esp/EFI/debian"; echo "esp" > "$WORK/esp/EFI/debian/grub.cfg"; umount "$WORK/esp"
mkdir "$WORK/other"; mount "$OTHER_DEV" "$WORK/other"; echo data > "$WORK/other/file"; umount "$WORK/other"
sync

