# SPDX-License-Identifier: GPL-3.0-or-later
#
# Back-end tests of SHADOW Disk Manager against loop devices. Sourced by
# udisks-rig.sh inside one "scripts/dev.sh exec" invocation:
#
#   scripts/dev.sh exec --dir disks -- bash /src/applications/disk-manager/tests/udisks-rig.sh /src/applications/disk-manager/tests/run-tests.sh
#
# Every action goes through shadow-disks-testctl, i.e. through the same
# UDisksClient calls the user interface makes; results are checked with
# independent tools (blkid, lsblk, blockdev, cmp, /proc/mounts).
# rig_guard is called before every modifying action.
CTL=shadow-disks-testctl
pass=0; fail=0
ok() { echo "PASS  $1"; pass=$((pass+1)); }
bad() { echo "FAIL  $1"; fail=$((fail+1)); }
# act <description> <expected: ok|refused> <testctl arguments…>
act() {
    local what="$1" expect="$2" out rc; shift 2
    out="$($CTL "$@" 2>&1)"; rc=$?
    echo "      $out"
    if { [ "$expect" = ok ] && [ $rc -eq 0 ]; } || { [ "$expect" = refused ] && [ $rc -eq 1 ]; }; then ok "$what"; else bad "$what (exit $rc)"; fi
    udevadm settle --timeout=10 2>/dev/null
    LAST="$out"
}
is() { if [ "$2" = "$3" ]; then ok "$1 ($2)"; else bad "$1 (got '$2', expected '$3')"; fi; }
dump() { shadow-disks --dump "$1" 2>/dev/null; }
jq_py() { python3 -c "import json,sys; d=json.load(sys.stdin); $1"; }
MIB=1048576

rig_loop 512; A="$RIG_DEV"; A_FILE="$(losetup -n -O BACK-FILE "$A")"
rig_loop 48;  B="$RIG_DEV"; B_FILE="$(losetup -n -O BACK-FILE "$B")"
sleep 1

echo "--- listing"
dump status
dump devices | jq_py "
for x in d: print('     ', x['device'], x['name'], x['size'], 'loop' if x['isLoop'] else '', x['bus'], '| protected:', x['protectedReason'] or '-', '| backing', x['backingFile'])"
n="$(dump devices | jq_py "print(sum(1 for x in d if x['device'] in ('$A','$B') and x['isLoop']))")"
is "both loop devices are listed" "$n" 2
echo "      formatTypes: $(dump capabilities | jq_py "print(' '.join(d['formatTypes']))")"
dump capabilities | jq_py "
for k,v in sorted(d['fileSystems'].items()): print('     ', k, 'format' if v.get('format') else 'no-format(%s)' % v.get('formatTool'), 'resize-mode=%s' % v.get('resizeMode'), 'check' if v.get('check') else '-', 'repair' if v.get('repair') else '-')"
echo "      protected disks: $(dump protected | tr -d '\n')"

echo "--- partition table and partitions"
rig_guard "$A"; act "new GPT partition table on $A" ok table "$A" gpt
is "blkid sees a GPT" "$(blkid -o value -s PTTYPE "$A")" gpt
rig_guard "$A"; act "create 200 MiB ext4 partition 'DATA'" ok create "$A" $MIB $((200*MIB)) ext4 DATA
is "p1 is ext4" "$(blkid -o value -s TYPE "${A}p1")" ext4
is "p1 label" "$(blkid -o value -s LABEL "${A}p1")" DATA
free_offset="$(dump devices | jq_py "
dev=[x for x in d if x['device']=='$A'][0]
print([v for v in dev['volumes'] if v['free']][0]['offset'])")"
echo "      free space reported from offset $free_offset"
rig_guard "$A"; act "create a partition without file system in the remaining space" ok create "$A" "$free_offset" 0
lsblk -o NAME,SIZE,FSTYPE,LABEL "$A" | sed 's/^/      /'
is "two partitions exist" "$(lsblk -ln -o NAME "$A" | grep -c "^$(basename "$A")p[0-9]")" 2
dump devices | jq_py "
dev=[x for x in d if x['device']=='$A'][0]
print('      model: table', dev['tableType'], [(v['device'] or 'FREE', v['size'], v.get('fsType','')) for v in dev['volumes']])"

echo "--- mount, usage, in-use protection"
act "mount p1" ok mount "${A}p1"
mp="$(grep "^${A}p1 " /proc/mounts | awk '{print $2}')"
[ -n "$mp" ] && ok "p1 is mounted at $mp (/proc/mounts)" || bad "p1 not in /proc/mounts"
dd if=/dev/urandom of="$mp/payload.bin" bs=1M count=20 status=none; sync
dump devices | jq_py "
dev=[x for x in d if x['device']=='$A'][0]; v=dev['volumes'][0]
print('      usage: used', v['fsUsed'], 'of', v['fsTotal'], 'mounted at', v['mountPoints'])
sys.exit(0 if v['usageKnown'] and v['fsUsed'] > 20*1048576 else 1)" && ok "usage of the mounted file system is reported" || bad "usage missing"
rig_guard "${A}p1"; act "format refused while mounted" refused format "${A}p1" ext4 X
rig_guard "$A"; act "new partition table refused while a partition is mounted" refused table "$A" gpt
is "p1 still ext4 with its label" "$(blkid -o value -s LABEL "${A}p1")" DATA
act "unmount p1" ok unmount "${A}p1"
grep -q "^${A}p1 " /proc/mounts && bad "p1 still mounted" || ok "p1 no longer in /proc/mounts"

echo "--- label, check, repair"
rig_guard "${A}p1"; act "change label" ok label "${A}p1" SHADOWDATA
is "new label" "$(blkid -c /dev/null -o value -s LABEL "${A}p1")" SHADOWDATA
act "check p1" ok check "${A}p1"
rig_guard "${A}p1"; act "repair p1" ok repair "${A}p1"

echo "--- resize"
rig_guard "${A}p2"; act "delete p2" ok delete "${A}p2"
is "one partition left" "$(lsblk -ln -o NAME "$A" | grep -c "^$(basename "$A")p[0-9]")" 1
rig_guard "${A}p1"; act "grow p1 to 320 MiB" ok resize "${A}p1" $((320*MIB))
is "partition size after growing" "$(blockdev --getsize64 "${A}p1")" $((320*MIB))
fsblocks="$(dumpe2fs -h "${A}p1" 2>/dev/null | awk -F: '/^Block count/{gsub(/ /,"",$2); print $2}')"
fsbs="$(dumpe2fs -h "${A}p1" 2>/dev/null | awk -F: '/^Block size/{gsub(/ /,"",$2); print $2}')"
is "file system grew with it" "$((fsblocks*fsbs))" $((320*MIB))
rig_guard "${A}p1"; act "shrink p1 to 120 MiB" ok resize "${A}p1" $((120*MIB))
is "partition size after shrinking" "$(blockdev --getsize64 "${A}p1")" $((120*MIB))
fsblocks="$(dumpe2fs -h "${A}p1" 2>/dev/null | awk -F: '/^Block count/{gsub(/ /,"",$2); print $2}')"
[ $((fsblocks*fsbs)) -le $((120*MIB)) ] && ok "file system fits the smaller partition ($((fsblocks*fsbs)) bytes)" || bad "file system larger than partition"
act "mount p1 after resizing" ok mount "${A}p1"
mp="$(grep "^${A}p1 " /proc/mounts | awk '{print $2}')"
[ "$(stat -c %s "$mp/payload.bin" 2>/dev/null)" = $((20*MIB)) ] && ok "data survived both resizes" || bad "payload missing after resize"
act "unmount p1" ok unmount "${A}p1"

echo "--- format"
for fs in $(dump capabilities | jq_py "print(' '.join(d['formatTypes']))"); do
    [ "$fs" = ext2 ] || [ "$fs" = ext3 ] && continue
    rig_guard "${A}p1"; act "format p1 as $fs" ok format "${A}p1" "$fs" T$fs
    want="$fs"; [ "$fs" = swap ] && want=swap
    is "blkid type after formatting as $fs" "$(blkid -c /dev/null -o value -s TYPE "${A}p1")" "$want"
done
rig_guard "${A}p1"; act "format with a type UDisks cannot create here is refused" refused format "${A}p1" xfs X
rig_guard "${A}p1"; act "format p1 as ext4 again" ok format "${A}p1" ext4 SYSTEM

echo "--- UDisks jobs while operations run"
$CTL watch-jobs "$A" 12 >"$RIG/jobs.out" 2>&1 &
watcher=$!
sleep 2
rig_guard "${A}p1"; act "format p1 (watched)" ok format "${A}p1" ext4 JOBTEST
act "check p1 (watched)" ok check "${A}p1"
wait $watcher
sort "$RIG/jobs.out" | uniq -c | sed 's/^/      /'
grep -q "^job format-mkfs" "$RIG/jobs.out" && ok "the model reported the UDisks format job while it ran" || bad "no format job seen"
grep -q "^no jobs" "$RIG/jobs.out" && ok "the job list emptied when the work was done" || bad "job list never emptied"

echo "--- destructive actions on a system disk are refused"
if mount "${A}p1" /boot; then
echo "      ${A}p1 mounted at /boot in this private namespace"
echo "      protected disks: $(dump protected | tr -d '\n')"
dump protected | grep -q "\"$A\"" && ok "$A is recognised as holding /boot" || bad "$A not protected"
out="$($CTL refusal "$A")"; echo "      $out"
rig_guard "$A"; act "new partition table on the /boot disk refused" refused table "$A" gpt
case "$LAST" in *"boot files"*) ok "the refusal names the reason" ;; *) bad "refusal without reason" ;; esac
rig_guard "${A}p1"; act "format of the /boot partition refused" refused format "${A}p1" ext4 X
rig_guard "${A}p1"; act "delete of the /boot partition refused" refused delete "${A}p1"
truncate -s 1M "$RIG/small.img"
rig_guard "$A"; act "restore onto the /boot disk refused" refused restore "$A" "$RIG/small.img"
is "partition table untouched" "$(blkid -c /dev/null -o value -s PTTYPE "$A")" gpt
is "file system untouched" "$(blkid -c /dev/null -o value -s LABEL "${A}p1")" JOBTEST
umount /boot
out="$($CTL refusal "$A")"; [ "$out" = '{"refusal":""}' ] && ok "no refusal once /boot is unmounted" || bad "still refused: $out"
else
bad "could not mount ${A}p1 at /boot — protection tests skipped"
fi

echo "--- disk images"
rig_guard "$B"; act "format $B as a whole (no partition table)" ok format "$B" ext4 IMAGEME
act "mount $B" ok mount "$B"
mpb="$(grep "^$B " /proc/mounts | awk '{print $2}')"
echo "shadow image test $(date +%s)" > "$mpb/marker.txt"; dd if=/dev/urandom of="$mpb/blob" bs=1M count=8 status=none
marker="$(cat "$mpb/marker.txt")"
act "unmount $B" ok unmount "$B"
act "create image of $B" ok image "$B" "$RIG/b.img"
cmp -s "$RIG/b.img" "$B_FILE" && ok "image is byte-identical to the device ($(stat -c %s "$RIG/b.img") bytes, $(du -k "$RIG/b.img" | cut -f1) KiB allocated)" || bad "image differs from the device"
act "image onto an existing file refused" refused image "$B" "$RIG/b.img"
rig_guard "$B"; act "destroy $B: new MBR partition table" ok table "$B" dos
is "ext4 signature is gone" "$(blkid -c /dev/null -o value -s TYPE "$B")" ""
rig_guard "$B"; act "restore image to $B" ok restore "$B" "$RIG/b.img"
sync; cmp -s "$RIG/b.img" "$B_FILE" && ok "device is byte-identical to the image again" || bad "device differs from the image"
act "mount restored $B" ok mount "$B"
mpb="$(grep "^$B " /proc/mounts | awk '{print $2}')"
is "content restored" "$(cat "$mpb/marker.txt" 2>/dev/null)" "$marker"
act "unmount $B" ok unmount "$B"
act "create image of $A, cancelled after 64 MiB" refused image-cancel "$A" "$RIG/a.img" $((64*MIB))
[ ! -e "$RIG/a.img" ] && [ ! -e "$RIG/a.img.part" ] && ok "no partial image left behind" || bad "partial image left: $(ls "$RIG" | grep a.img)"
act "create full image of $A" ok image "$A" "$RIG/a.img"
rig_guard "$B"; act "restore of an image larger than the device refused" refused restore "$B" "$RIG/a.img"
rig_guard "$B"; act "restore from a missing file refused" refused restore "$B" "$RIG/missing.img"

echo "--- encrypted volume (LUKS)"
rig_guard "${A}p1"
if printf 'correct horse' | cryptsetup luksFormat --batch-mode --type luks2 --pbkdf pbkdf2 --pbkdf-force-iterations 1000 "${A}p1" - 2>"$RIG/luks.err"; then
    udevadm trigger --action=change --sysname-match="$(basename "$A")p1"; udevadm settle; sleep 1
    dump devices | jq_py "
v=[x for x in d if x['device']=='$A'][0]['volumes'][0]
print('      model:', v['device'], 'encrypted' if v['encrypted'] else '', 'canUnlock' if v['canUnlock'] else '', v['fsType'])
sys.exit(0 if v['encrypted'] and v['canUnlock'] and not v['unlocked'] else 1)" && ok "locked LUKS volume is recognised" || bad "LUKS volume not recognised"
    if [ -e /sys/class/misc/device-mapper ]; then
    out="$(printf 'wrong passphrase\n' | $CTL unlock "${A}p1" 2>&1)"; rc=$?; echo "      $out"
    [ $rc -eq 1 ] && ok "wrong passphrase is rejected" || bad "wrong passphrase accepted?"
    out="$(printf 'correct horse\n' | $CTL unlock "${A}p1" 2>&1)"; rc=$?; echo "      $out"
    [ $rc -eq 0 ] && ok "unlock with the right passphrase" || bad "unlock failed"
    udevadm settle; sleep 1
    clear="$(dump devices | jq_py "print([x for x in d if x['device']=='$A'][0]['volumes'][0]['cleartextDevice'])")"
    echo "      cleartext device: $clear"
    if [ -n "$clear" ]; then
        rig_guard "${A}p1"; act "format the unlocked volume as ext4" ok format "$clear" ext4 SECRET
        act "mount the unlocked volume" ok mount "$clear"
        grep -q " /media/.*SECRET" /proc/mounts && ok "encrypted file system mounted ($(grep SECRET /proc/mounts | awk '{print $1, $2}'))" || bad "not mounted"
        act "lock refused while mounted" refused lock "${A}p1"
        act "unmount the unlocked volume" ok unmount "$clear"
        act "lock" ok lock "${A}p1"
        [ -e "$clear" ] && bad "cleartext device still present" || ok "cleartext device is gone after locking"
    else
        bad "no cleartext device in the model"
    fi
    else
        out="$(printf 'correct horse\n' | $CTL unlock "${A}p1" 2>&1)"; rc=$?; echo "      $out"
        [ $rc -eq 1 ] && ok "unlock reports UDisks' error (this kernel has no device-mapper, so no volume can be opened)" || bad "unlock claimed success without device-mapper"
        echo "NOTE  no device-mapper in this kernel: unlocking, mounting and locking a LUKS volume could NOT be tested"
    fi
    grep -c "correct horse\|wrong passphrase" "$RIG/udisksd.log" >/dev/null 2>&1 && bad "passphrase appears in the udisksd log" || ok "passphrase not in the udisksd log"
else
    echo "NOTE  cryptsetup luksFormat failed here: $(tr '\n' ' ' < "$RIG/luks.err") — LUKS unlock/lock not tested"
fi

echo "--- delete, guard"
rig_guard "${A}p1"; act "delete p1" ok delete "${A}p1"
is "no partitions left" "$(lsblk -ln -o NAME "$A" | grep -c "^$(basename "$A")p[0-9]")" 0
( rig_guard /dev/vda ) 2>/dev/null; is "rig_guard refuses a non-loop device (exit code)" $? 97
reachable=""
for real in $(lsblk -dn -o NAME 2>/dev/null | grep -v '^loop'); do [ -b "/dev/$real" ] && reachable="$reachable $real"; done
[ -z "$reachable" ] && ok "no real disk ($(lsblk -dn -o NAME | grep -v '^loop' | tr '\n' ' ')) is reachable as a block device inside the rig" || bad "real disk nodes reachable:$reachable"
out="$(SHADOW_DISKS_TEST_LOOP_ONLY=0 $CTL mount "$B" 2>&1)"; is "test driver refuses to run without the loop-only guard" $? 2

echo "disk-manager tests: $pass passed, $fail failed"
