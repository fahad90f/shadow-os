# SHADOW Disk Manager (`shadow-disks`, org.shadow.diskmanager)

Drives, partitions, file systems and disk images, through UDisks2. The
application needs no privilege of its own and runs no tool as root: every
action is a D-Bus call to `org.freedesktop.UDisks2` on the system bus, and
UDisks asks for authorisation through polkit (its own actions
`org.freedesktop.udisks2.*`; the prompt comes from the session's polkit agent).

## Data and actions

| What | Source |
|---|---|
| Device tree | `ObjectManager.GetManagedObjects` once, then `InterfacesAdded/Removed` and `PropertiesChanged` (`udisksclient.cpp`). Drives: `Drive` (vendor, model, serial, revision, removable, ejectable, can power off, rotation rate, connection bus); block devices: `Block`, `Partition`, `PartitionTable`, `Filesystem`, `Encrypted`, `Swapspace`, `Loop`. |
| Bus | `Drive.ConnectionBus`, otherwise the device's sysfs path (USB, SATA, NVMe, VirtIO, SD/MMC, loop). |
| Usage | `statvfs()` of the mount point — only known while mounted. |
| SMART | `Drive.Ata` (status, temperature, power-on time, bad sectors, self-test, `SmartGetAttributes`) or `NVMe.Controller`; refresh = `SmartUpdate`. Absent → the section says so. |
| Capabilities | `Manager.SupportedFilesystems` + `CanFormat/CanResize/CanCheck/CanRepair` per type: the Format/Create dialogs list only types whose mkfs tool is installed; Check/Repair/Resize appear only where UDisks supports them. |
| Jobs | `Job` objects (operation, targets, `Progress`/`ProgressValid`, rate, expected end, `Cancel`). When UDisks gives no progress figure the bar is indeterminate and says so. |
| Mount / unmount | `Filesystem.Mount/Unmount` |
| Eject / power off | `Drive.Eject/PowerOff` (shown when the drive reports the ability) |
| Unlock / lock LUKS | `Encrypted.Unlock(passphrase)/Lock`; the passphrase exists only in the dialog field and the D-Bus call — never stored, logged or put in an error message |
| Format | `Block.Format(type, {label, update-partition-type})` |
| New partition table | `Block.Format("gpt"|"dos")` |
| Create / delete partition | `PartitionTable.CreatePartition[AndFormat]`, `Partition.Delete` |
| Resize | `Partition.Resize` + `Filesystem.Resize` (grow: partition first; shrink: file system first), offline only |
| Label, check, repair | `Filesystem.SetLabel/Check/Repair` |
| Disk images | `Block.OpenForBackup` / `OpenForRestore` return a file descriptor; a worker thread copies it to/from the file (`imageworker.cpp`) with byte-exact progress, rate, cancel. Images are written sparse to `<name>.part` and renamed when complete; a cancelled or failed image is removed. After a restore `Block.Rescan` is called. |

No benchmark is offered (nothing truthful could be shown without a privileged raw-device reader).

### Destructive actions

Format, delete partition, new partition table and restore image

* open `DestructiveDialog.qml`, which names the device, lists every volume
  that will be lost (file system, label, size, usage, mount point, partition
  table) and enables the action only after the device name (e.g. `/dev/sdb1`)
  has been typed;
* are refused — in the back-end, before any D-Bus call, and the buttons are
  disabled with the reason — for the **whole disk** that holds `/`, `/boot`,
  `/boot/efi`, `/efi`, `/usr`, an active swap area, or the live medium
  (`/run/live/medium`, `/lib/live/mount/medium`, `/run/initramfs/live`,
  `/cdrom`, anything under `/run/live/`). Detection (`systemdevices.cpp`):
  `stat()` of those directories, `/proc/self/mountinfo`, `/proc/swaps`, and
  the sysfs topology (partition → disk, device-mapper/RAID members);
* are refused while the target (or anything on it) is mounted, active swap or unlocked.

## Diagnostics

`shadow-disks --dump status|devices|jobs|capabilities|protected` prints the data behind the UI as JSON.

## Run-time dependencies (Debian packages)

`udisks2` (with `libblockdev-*` plug-ins it pulls in), `polkitd` and a polkit
agent in the session, `dbus`; per file system the tool UDisks uses:
`e2fsprogs`, `dosfstools`, `btrfs-progs`, `xfsprogs`, `exfatprogs`,
`ntfs-3g`, `f2fs-tools`, `nilfs-tools`, `udftools`; `cryptsetup-bin` for
LUKS; `smartmontools` is not needed (UDisks reads SMART itself via libatasmart/libnvme).

## Limitations

* Protection is per disk: a spare partition on the system disk cannot be formatted or deleted either (use the installer/recovery environment).
* Resize works on unmounted file systems only and not on encrypted volumes; Btrfs/XFS (online-only resize) are therefore not offered.
* Images are of whole devices (not single partitions); paths are typed, there is no file chooser.
* Creating encrypted volumes, RAID/LVM management, partition type/flag editing and SMART self-tests are not implemented.
* The partition bar gives very small partitions a minimum width (7 %) so they stay clickable; sizes are otherwise proportional.
* MBR extended partitions are listed but logical partitions were not tested.

## What was tested, and how

Everything below ran in the build chroot inside **one** `scripts/dev.sh exec`
invocation per run, with `tests/udisks-rig.sh`: private system bus, `polkitd`,
`systemd-udevd` (one blkid rule for loop devices only), `udisksd` 2.10.1, and
loop devices backed by sparse files in `/tmp`. Safety: in the rig's private
mount namespace every non-loop block device node (`/dev/vda` …) is covered by
an empty regular file; `SHADOW_DISKS_TEST_LOOP_ONLY=1` makes the back-end
refuse modifying actions on non-loop devices; `rig_guard` aborts the script
for any device the rig did not create; loop devices are detached on exit
(verified with `losetup -a` after each run).

```
scripts/dev.sh exec --dir disks -- bash /src/applications/disk-manager/tests/udisks-rig.sh /src/applications/disk-manager/tests/run-tests.sh
scripts/dev.sh exec --dir disks -- env GUI_STEPS=/src/applications/disk-manager/tests/gui-steps.sh bash /src/applications/disk-manager/tests/udisks-rig.sh /src/applications/disk-manager/tests/gui-shots.sh
tests/desktop/smoke.sh --dir disks disk-manager
```

`run-tests.sh` (last run: **80 passed, 0 failed**) drives the same
`UDisksClient` calls as the UI through `shadow-disks-testctl` and checks the
results with independent tools (`blkid`, `lsblk`, `blockdev`, `dumpe2fs`,
`cmp`, `/proc/mounts`): listing and `--dump`; capabilities (here: ext2/3/4,
vfat, btrfs, swap can be created; xfs/ntfs/exfat/f2fs/nilfs2/udf are
correctly not offered); new GPT and MBR table; create partitions with and
without file system in reported free space; mount/unmount and usage; refusal
while mounted; label; check; repair; grow and shrink an ext4 partition with
data intact; format as ext4, vfat, btrfs, swap; UDisks jobs (`format-mkfs`,
`filesystem-check`) appear in the model while running and disappear
afterwards; refusal of table/format/delete/restore for a disk whose
partition is mounted at `/boot` (and `/dev/vda` listed as protected for `/`),
with partition table and file system verified untouched; image create
(byte-identical to the device), refuse overwrite, destroy + restore
(byte-identical again, file content back), cancel (no partial file), refuse
an image larger than the device / a missing file; delete partition; the guards.

`gui-shots.sh` runs the real window against the same UDisks (screenshots
`build/work/shots/disks-dm-*.png`): device list and partition map, Format
dialog, confirm button disabled until the full device name is typed, format
executed from the dialog (verified with `blkid`), free space, protected-disk banner.

Smoke test: pass, no QML warnings (shows "Disk management is unavailable": no system bus in the chroot).

**Not tested / not working here**

* **LUKS unlock, mount of the clear-text device, lock**: the build machine's
  kernel has no device-mapper. A LUKS2 volume made with `cryptsetup luksFormat`
  is recognised as "Encrypted, locked" and `Unlock` returns UDisks' error
  ("Failed to activate device: Operation not supported"); nothing beyond that.
* SMART (ATA and NVMe), eject, power off, removable media, optical drives:
  no hardware; code paths written against the UDisks API documentation only.
* polkit prompts: the tests run as root, which polkit authorises without asking.
* The back-end's `SHADOW_DISKS_TEST_LOOP_ONLY` refusal branch itself (no non-loop device is visible to UDisks in the rig), MBR logical partitions, multi-device stacks (RAID/LVM), read-only media.
