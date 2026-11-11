#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Test of nexus-recovery-engine against a fake installed system on LOOP
# DEVICES backed by files in a temporary directory. Run as root inside the
# build chroot, in ONE invocation:
#
#   scripts/dev.sh exec --dir recovery -- bash /src/system/recovery/tests/engine-test.sh
#
# Safety: the engine is only ever started with NEXUS_RECOVERY_TEST_DEVICES set
# to loop devices that this script attached and whose backing files lie in its
# own work directory (checked again before every call). In that mode the
# engine lists no other block device, so nothing else can become a target.
# State and log go to the work directory, not to /run and /var/log.
#
# What it cannot test here is stated at the end of its output.
set -u
ENGINE="${ENGINE:-/src/system/recovery/engine/nexus-recovery-engine}"
WORK="$(mktemp -d /tmp/nexus-recovery-test.XXXXXX)"
pass=0; fail=0
export NEXUS_RECOVERY_STATE_DIR="$WORK/state" NEXUS_RECOVERY_LOG_DIR="$WORK/log"
export PYTHONDONTWRITEBYTECODE=1

note() { echo "$*"; }
ok() { note "ok    $1"; pass=$((pass + 1)); }
bad() { note "FAIL  $1"; fail=$((fail + 1)); }
check() { if [ "$2" = "$3" ]; then ok "$1 ($2)"; else bad "$1: got '$2', expected '$3'"; fi; }
contains() { case "$2" in *"$3"*) ok "$1" ;; *) bad "$1: '$2' does not contain '$3'" ;; esac; }

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

# ---- guards of the engine itself -----------------------------------------------------
note "-- guards"
out="$(NEXUS_RECOVERY_TEST_DEVICES=/dev/vda python3 "$ENGINE" scan 2>&1)"
check "a non-loop device in NEXUS_RECOVERY_TEST_DEVICES is refused" "$(result "$out" error.code)" usage
out="$(engine mount --target '../../etc' 2>&1)"; check "a path as target id is refused" "$(result "$out" error.code)" usage
out="$(engine mount --target 0000-not-there 2>&1)"; check "an unknown target is refused" "$(result "$out" error.code)" not-found
check "the running root is refused for fsck (unit)" "$(cd "$(dirname "$ENGINE")" && python3 -c '
from nexus_recovery import target
from nexus_recovery.util import RecoveryError
try:
    target.unmounted_devices_of({"running": True, "mounted_by_recovery": None})
    print("accepted")
except RecoveryError as e:
    print(e.code)')" running
check "symbolic links are resolved inside the target (unit)" "$(cd "$(dirname "$ENGINE")" && python3 -c '
import os, sys, tempfile
from nexus_recovery import scan
d = tempfile.mkdtemp(dir=sys.argv[1])
os.makedirs(d + "/etc"); open(d + "/etc/inside", "w").write("inside")
os.symlink("/etc/inside", d + "/etc/abs"); os.symlink("../../../../../etc/hostname", d + "/etc/up"); os.symlink("loop", d + "/etc/loop")
print(scan.read_in_root(d, "etc/abs"), scan.resolve_in_root(d, "etc/up"), scan.resolve_in_root(d, "etc/loop"))' "$WORK")" "inside None None"

# ---- scan --------------------------------------------------------------------------
note "-- scan"
before="$(md5sum < "$WORK/root.img")"
scan="$(engine scan)"
echo "$scan" > "$WORK/scan.json"
check "scan: ok" "$(result "$scan" ok)" true
check "scan: one installation (the data file system is not one)" "$(tail -n1 <<<"$scan" | python3 -c 'import json,sys; print(len(json.load(sys.stdin)["installations"]))')" 1
check "scan: id is the root UUID" "$(result "$scan" installations.0.id)" "$ROOT_UUID"
check "scan: name from os-release (through the relative link)" "$(result "$scan" installations.0.name)" "NEXUS OS 1.0 (test)"
check "scan: NEXUS OS" "$(result "$scan" installations.0.is_nexus)" true
check "scan: host name (through the absolute link, inside the target)" "$(result "$scan" installations.0.hostname)" recovery-test-host
check "scan: not the running system" "$(result "$scan" installations.0.running)" false
check "scan: root device" "$(result "$scan" installations.0.root.device)" "$ROOT_DEV"
check "scan: /boot/efi resolved from fstab" "$(result "$scan" installations.0.filesystems.0.device)" "$ESP_DEV"
contains "scan: a missing fstab file system is reported" "$(result "$scan" installations.0.problems)" "/var"
check "scan: login accounts (root and alice, not the service accounts)" "$(tail -n1 <<<"$scan" | python3 -c 'import json,sys; print(",".join(u["name"] for u in json.load(sys.stdin)["installations"][0]["users"]))')" "root,alice"
check "scan: boot.conf read" "$(result "$scan" installations.0.boot.FIRMWARE)" uefi
check "scan: restricted to the test devices" "$(result "$scan" restricted_to)" "[\"$ROOT_DEV\", \"$ESP_DEV\", \"$LUKS_DEV\", \"$OTHER_DEV\", \"$FAT_DEV\"]"
check "scan: wrote nothing to the root image" "$(md5sum < "$WORK/root.img")" "$before"
check "scan: left nothing mounted" "$(grep -c " $WORK" /proc/self/mounts)" 0
if [ "$LUKS_OK" = yes ]; then
    check "scan: locked LUKS volume listed" "$(result "$scan" locked_volumes.0.device)" "$LUKS_DEV"
    out="$(echo "correct horse" | engine unlock --device "$LUKS_DEV")"
    note "      unlock in this kernel: ok=$(result "$out" ok) code=$(result "$out" error.code) message=$(result "$out" error.message)"
    out="$(engine unlock --device "$LUKS_DEV" < /dev/null)"; check "unlock: no passphrase on stdin is refused" "$(result "$out" error.code)" usage
    out="$(echo x | engine unlock --device "$ROOT_DEV")"; check "unlock: not a LUKS volume is refused" "$(result "$out" error.code)" usage
else
    note "      (cryptsetup luksFormat did not work here: LUKS detection not exercised)"
fi

# ---- fsck (unmounted) ----------------------------------------------------------------
note "-- fsck"
out="$(engine fsck --target "$ROOT_UUID")"
check "fsck: clean root" "$(result "$out" filesystems.0.status)" clean
check "fsck: clean ESP ($ESP_FS)" "$(result "$out" filesystems.1.status)" clean
check "fsck: clean FAT file system (fsck.vfat -n)" "$(result "$out" filesystems.2.fstype)/$(result "$out" filesystems.2.status)" vfat/clean
check "fsck: ok" "$(result "$out" ok)" true
contains "fsck: the check is e2fsck -f -n" "$out" "e2fsck -f -n $ROOT_DEV"
# Damage the root file system on purpose: a wrong link count.
guard "$ROOT_DEV"; debugfs -w -R "sif /etc/hostname.real links_count 7" "$ROOT_DEV" > /dev/null 2>&1; sync
before="$(md5sum < "$WORK/root.img")"
out="$(engine fsck --target "$ROOT_UUID" --device "$ROOT_DEV")"; code=$?
check "fsck: damage found" "$(result "$out" filesystems.0.status)" errors
check "fsck: not ok, exit status 1" "$(result "$out" ok)/$code" "false/1"
check "fsck: without --repair nothing is written" "$(md5sum < "$WORK/root.img")" "$before"
out="$(engine fsck --target "$ROOT_UUID" --device "$ROOT_DEV" --repair)"
check "fsck --repair: repaired" "$(result "$out" filesystems.0.status)" repaired
contains "fsck --repair: check first, then e2fsck -f -y" "$(grep -o '"command": "[^"]*"' <<<"$out" | tr '\n' ' ')" "e2fsck -f -n $ROOT_DEV\" \"command\": \"e2fsck -f -y $ROOT_DEV"
out="$(engine fsck --target "$ROOT_UUID")"; check "fsck: clean again" "$(result "$out" ok)" true
out="$(engine fsck --target "$ROOT_UUID" --device "$OTHER_DEV")"; check "fsck: a device that is not part of the target is refused" "$(result "$out" error.code)" usage

# ---- mount ---------------------------------------------------------------------------
note "-- mount"
out="$(engine repair-boot --target "$ROOT_UUID" --dry-run)"; check "repair before mount is refused" "$(result "$out" error.code)" not-mounted
out="$(engine mount --target "$ROOT_UUID")"
MP="$(result "$out" mountpoint)"
check "mount: ok" "$(result "$out" ok)" true
check "mount: private mount point" "$MP" "$WORK/state/mnt-$ROOT_UUID"
check "mount: what was mounted (/data is left alone)" "$(result "$out" mounts)" '["/", "/boot/efi", "/dev", "/proc", "/sys", "/run"]'
contains "mount: the missing /var is a warning, not an error" "$(result "$out" warnings)" "/var"
check "mount: root is the target's" "$(cat "$MP/etc/hostname.real")" recovery-test-host
check "mount: ESP inside" "$(cat "$MP/boot/efi/EFI/debian/grub.cfg")" esp
check "mount: /proc inside" "$([ -e "$MP/proc/self/stat" ] && echo yes)" yes
check "mount: /dev inside" "$([ -c "$MP/dev/null" ] && echo yes)" yes
out="$(engine mount --target "$ROOT_UUID")"; check "mount twice is refused" "$(result "$out" error.code)" already-mounted
out="$(engine fsck --target "$ROOT_UUID")"; check "fsck of a mounted installation is refused" "$(result "$out" error.code)" mounted
check "scan sees it as mounted by recovery" "$(result "$(engine scan)" installations.0.mounted_by_recovery)" "$MP"

# ---- repairs -------------------------------------------------------------------------
note "-- repair-boot / repair-packages"
out="$(engine repair-boot --target "$ROOT_UUID" --dry-run)"
check "repair-boot --dry-run: the command sequence" "$(result "$out" steps | python3 -c 'import json,sys; print(" ; ".join(s["command"] for s in json.load(sys.stdin)))')" \
    "chroot $MP update-initramfs -u -k all ; chroot $MP nexus-grub-install --verbose ; chroot $MP update-grub"
check "repair-boot --dry-run: nothing was run" "$([ -e "$MP/var/log/stub-calls.log" ] && echo ran || echo not-run)" not-run
out="$(engine repair-packages --target "$ROOT_UUID" --dry-run)"
check "repair-packages --dry-run: the command sequence" "$(result "$out" steps | python3 -c 'import json,sys; print(" ; ".join(s["command"] for s in json.load(sys.stdin)))')" \
    "chroot $MP dpkg --audit ; chroot $MP dpkg --configure -a ; chroot $MP apt-get -f install -y"
# With the stand-ins: order, chroot, live output, failure handling. NOT the real tools.
out="$(engine repair-boot --target "$ROOT_UUID")"
check "repair-boot (stand-in tools): ok" "$(result "$out" ok)" true
check "repair-boot (stand-in tools): called in order, inside the target" "$(tr '\n' ';' < "$MP/var/log/stub-calls.log")" "update-initramfs -u -k all;nexus-grub-install --verbose;update-grub ;"
contains "repair-boot: output is passed on line by line" "$out" '{"event": "output", "line": "[nexus-grub-install] called with: --verbose"}'
: > "$MP/var/log/stub-calls.log"; touch "$MP/etc/stub-fail-nexus-grub-install"
out="$(engine repair-boot --target "$ROOT_UUID")"; code=$?
check "repair-boot: a failing step stops the sequence" "$(result "$out" failed_step)/$(result "$out" ok)/$code" "grub-install/false/1"
check "repair-boot: the step after the failure did not run" "$(grep -c update-grub "$MP/var/log/stub-calls.log")" 0
rm "$MP/etc/stub-fail-nexus-grub-install"
out="$(engine repair-packages --target "$ROOT_UUID")"; check "repair-packages (stand-in tools): ok" "$(result "$out" ok)" true
mv "$MP/etc/nexus/boot.conf" "$MP/etc/nexus/boot.conf.off"
# (is_nexus stays true through os-release)
out="$(engine repair-boot --target "$ROOT_UUID" --dry-run)"; check "repair-boot without boot.conf is refused" "$(result "$out" error.code)" no-boot-conf
mv "$MP/etc/nexus/boot.conf.off" "$MP/etc/nexus/boot.conf"
umount "$MP/boot/efi"
out="$(engine repair-boot --target "$ROOT_UUID" --dry-run)"; check "repair-boot with the ESP not mounted is refused" "$(result "$out" error.code)" not-mounted
guard "$ESP_DEV"; mount -t "$ESP_FS" "$ESP_DEV" "$MP/boot/efi"

# ---- password ------------------------------------------------------------------------
note "-- reset-password"
old="$(grep '^alice:' "$MP/etc/shadow")"
out="$(printf 'N3w-Pass:word with spaces\n' | engine reset-password --target "$ROOT_UUID" --user alice)"
check "reset-password: ok" "$(result "$out" ok)" true
new="$(grep '^alice:' "$MP/etc/shadow" | cut -d: -f2)"
check "reset-password: the hash changed" "$([ "$old" != "alice:$new" ] && [ -n "$new" ] && echo changed)" changed
check "reset-password: the new password verifies against the hash" "$(perl -e 'print crypt($ARGV[0], $ARGV[1]) eq $ARGV[1] ? "match" : "no match"' 'N3w-Pass:word with spaces' "$new")" match
check "reset-password: other accounts untouched" "$(grep -c '^root:!:' "$MP/etc/shadow")" 1
check "reset-password: the password is not in the log" "$(grep -c 'N3w-Pass' "$WORK"/log/*.log)" 0
contains "reset-password: the command is in the log, input redacted" "$(cat "$WORK"/log/*.log)" "chpasswd <stdin: redacted>"
out="$(echo pw | engine reset-password --target "$ROOT_UUID" --user mallory)"; check "reset-password: unknown user refused" "$(result "$out" error.code)" not-found
out="$(echo pw | engine reset-password --target "$ROOT_UUID" --user svc)"; check "reset-password: service account refused" "$(result "$out" error.code)" not-found
out="$(echo pw | engine reset-password --target "$ROOT_UUID" --user 'alice;id')"; check "reset-password: odd user name refused" "$(result "$out" error.code)" usage
out="$(engine reset-password --target "$ROOT_UUID" --user alice < /dev/null)"; check "reset-password: empty stdin refused" "$(result "$out" error.code)" usage

# ---- logs ----------------------------------------------------------------------------
note "-- logs"
out="$(engine logs --target "$ROOT_UUID" --output "$WORK/logs.tar.gz")"
check "logs: ok" "$(result "$out" ok)" true
members="$(tar -tzf "$WORK/logs.tar.gz" | tr '\n' ' ')"
for member in target/etc/fstab target/etc/nexus/boot.conf target/var/log/installer/install-20260101.log target/var/log/apt/history.log recovery/installation.json recovery/lsblk.txt recovery/not-included.txt; do
    contains "logs: contains $member" "$members" "$member"
done
check "logs: the target's os-release, not this system's" "$(tar -xzOf "$WORK/logs.tar.gz" target/etc/os-release | grep -c 'NEXUS OS 1.0 (test)')" 1
check "logs: no shadow file" "$(grep -c shadow <<<"$members")" 0
check "logs: private file" "$(stat -c %a "$WORK/logs.tar.gz")" 600
out="$(engine logs --target "$ROOT_UUID" --output "$WORK/logs.tar.gz")"; check "logs: an existing file is not overwritten" "$(result "$out" error.code)" exists
out="$(engine logs --target "$ROOT_UUID" --output relative.tar.gz)"; check "logs: a relative path is refused" "$(result "$out" error.code)" usage
# As pkexec would run it for an unprivileged user (here: nobody).
mkdir "$WORK/pub" "$WORK/priv"; chmod 755 "$WORK"; chown nobody "$WORK/pub"; chmod 700 "$WORK/priv"
out="$(PKEXEC_UID="$(id -u nobody)" engine logs --target "$ROOT_UUID" --output "$WORK/pub/l.tar.gz")"
check "logs: written with the caller's rights, owned by the caller" "$(result "$out" ok)/$(stat -c %U "$WORK/pub/l.tar.gz" 2>/dev/null)" true/nobody
out="$(PKEXEC_UID="$(id -u nobody)" engine logs --target "$ROOT_UUID" --output "$WORK/priv/l.tar.gz")"
check "logs: not where the caller may not write" "$(result "$out" error.code)/$([ -e "$WORK/priv/l.tar.gz" ] && echo created || echo absent)" output/absent

# ---- shell ---------------------------------------------------------------------------
note "-- shell"
out="$(engine shell --target "$ROOT_UUID")"; check "shell: the command" "$(result "$out" command)" "chroot $MP /bin/bash -l"
check "shell --exec: a shell in the target" "$(echo 'cat /etc/hostname.real' | engine shell --target "$ROOT_UUID" --exec 2>/dev/null | tail -n1)" recovery-test-host

# ---- umount --------------------------------------------------------------------------
note "-- umount"
( cd "$MP/home/alice" && exec sleep 300 ) & sleeper=$!
sleep 0.5
out="$(engine umount --target "$ROOT_UUID")"
check "umount: busy is reported" "$(result "$out" error.code)" busy
contains "umount: … with the process that is in the way" "$(result "$out" error.message)" "sleep [$sleeper]"
kill "$sleeper"; wait "$sleeper" 2>/dev/null
out="$(engine umount --target "$ROOT_UUID")"
check "umount: ok" "$(result "$out" ok)" true
check "umount: nothing left mounted" "$(grep -c " $WORK/state" /proc/self/mounts)" 0
check "umount: mount point and state removed" "$(ls "$WORK/state" | grep -c -E '^mnt-|\.json$')/$(ls "$WORK/state/targets" | wc -l)" 0/0
check "umount: /dev of this system is intact" "$([ -c /dev/null ] && [ -e /dev/loop-control ] && echo yes)" yes
out="$(engine umount --target "$ROOT_UUID")"; check "umount twice is refused" "$(result "$out" error.code)" not-mounted
out="$(engine mount --target "$ROOT_UUID" --read-only)"; check "mount --read-only" "$(result "$out" read_only)" true
out="$(engine repair-packages --target "$ROOT_UUID" --dry-run)"; check "repair on a read-only mount is refused" "$(result "$out" error.code)" read-only
out="$(engine umount --target "$ROOT_UUID")"; check "umount after read-only" "$(result "$out" ok)" true
out="$(engine fsck --target "$ROOT_UUID")"; check "file systems clean after all of this" "$(result "$out" ok)" true

mkdir -p /shots/nested-logs/recovery 2>/dev/null && cp "$WORK"/log/*.log /shots/nested-logs/recovery/engine-test-recovery.log 2>/dev/null
note "== $pass ok, $fail failed"
note "not tested here: LUKS unlock/lock and LVM activation (kernel without device-mapper: see the unlock line above),"
note "  the real update-initramfs / nexus-grub-install / update-grub / dpkg / apt-get (stand-ins were called),"
note "  journal export (the fake system has no journal), efivars, and the running system as target."
[ "$fail" -eq 0 ]
