# SHADOW Recovery

Repairs a Shadow OS installation that no longer starts or updates. It runs

* from the **Shadow OS medium**: the boot-menu entry "Recovery environment"
  starts the live session with `shadow.autostart=recovery`, and the shell then
  opens SHADOW Recovery — or
* on the **installed system itself** (application menu → SHADOW Recovery), for
  what can be repaired while it runs.

Two programs, one package (`shadow-recovery`, CMake component `recovery`):

| Program | What it is |
|---|---|
| `shadow-recovery` (`org.shadow.recovery`) | the window; runs as the logged-in user |
| `shadow-recovery-engine` (`/usr/sbin`, code in `/usr/lib/shadow-recovery/shadow_recovery`) | does the work as root; Python 3, standard library only; usable by hand on a console |

## For users

1. **Choose the installation.** Recovery lists every installed system it
   finds on the local disks, with device, size and host name. An encrypted
   disk shows up as "Encrypted volume": *Unlock…* asks for its passphrase,
   after which the system on it appears. The list is made by reading only.
2. **Start what you need.** Each action says what it does and what it
   changes; none starts by itself.

   | Action | Use it when | What it runs | What it changes |
   |---|---|---|---|
   | Check the file systems | after a crash or power loss; before anything else if the disk may be damaged | `e2fsck -f -n`, `fsck.vfat -n` on root, `/boot`, the EFI partition (unmounted) | nothing. If damage is found, *Repair…* (after a confirmation) runs `e2fsck -f -y` / `fsck.vfat -a` |
   | Repair the boot loader | the computer shows a GRUB prompt, "no bootable device", or stops in the initramfs | `update-initramfs -u -k all`, `shadow-grub-install --verbose`, `update-grub` inside the installation | files in `/boot`, the loader on the EFI partition or boot sector, the firmware's "Shadow OS" entry |
   | Repair the package system | an update was interrupted; the Updater reports a broken package system | `dpkg --audit`, `dpkg --configure -a`, `apt-get -f install -y` inside the installation | installed packages and their configuration |
   | Set a new password | a password was forgotten | `chpasswd` (and `faillock --reset`) inside the installation | that account's password — data encrypted with the old one (a wallet) stays locked; the disk-encryption passphrase is *not* changed |
   | Save logs to… | you want help: attach the archive to a report | collects the installer's log, the journal of the last three starts, APT/dpkg logs, `fstab`, `crypttab`, boot configuration, and Recovery's own log | nothing on the installation; writes one new `.tar.gz` (never overwrites) |
   | Open a terminal in the installed system | you want to repair by hand | a root shell whose `/` is the installation (`chroot`) | whatever you type |

   The **Output** box shows every command and its output as it happens; the
   same goes to `/var/log/shadow-recovery/recovery-<date>.log` (on the live
   medium that is in memory — use *Save logs to…* to keep it).
3. **Restart.** *Restart…* unmounts the installation, closes the encrypted
   volumes Recovery opened and restarts. Remove the medium when the screen
   goes dark.

On an installed system an administrator has to authenticate; in the live
session no password is asked for (the live user has none).

Without a graphical session: `sudo shadow-recovery-engine scan --pretty`, then
the sub-commands below with the `id` it printed.

## The engine

```
shadow-recovery-engine [--pretty] <command>
  scan                                  find installed systems (changes nothing)
  unlock --device DEV                   open a LUKS volume; passphrase on stdin
  lock [--device DEV]                   close the LUKS volumes the engine opened
  activate-lvm --volume-group VG        vgchange -ay
  mount --target ID [--read-only]       assemble the installation under /run/shadow-recovery/mnt-ID
  umount --target ID                    undo exactly that
  fsck --target ID [--device DEV] [--repair]
  repair-boot --target ID [--dry-run]
  repair-packages --target ID [--dry-run]
  reset-password --target ID --user NAME        new password on stdin
  logs --target ID --output FILE.tar.gz
  shell --target ID [--exec]            print (or start) the chroot shell
```

Output: one JSON object per line — `step` (title and exact command), `output`
(a line of the command's output), `step-done`, and last `result` with
`"ok": true|false` (and `error: {code, message, hint}`). Exit status 0, 1
(failed) or 2 (usage). `ID` is the root file system's UUID from `scan`;
`running` means the system the engine runs on.

How it keeps to its limits (`engine/shadow_recovery/`):

* **scan** (`scan.py`): `lsblk -J` (completed with `blkid -p` where udev has
  no data). Every ext2/3/4, btrfs, xfs or f2fs file system that is not the
  live medium is looked at: if it is mounted, there; otherwise it is mounted
  read-only *without journal/log replay* (`ro,noload`, `ro,norecovery`,
  `ro,rescue=nologreplay`) on a private directory, read and unmounted. An
  installation is a file system with `/etc/os-release` and `/usr`; it counts
  as Shadow OS with `ID=shadow` or `/etc/shadow/boot.conf`. Reported per
  installation: root device, `fstab` file systems resolved to devices,
  `crypttab`, `boot.conf`, login accounts, what is missing. Symbolic links
  inside an installation are resolved inside it, never into the running system.
* **A target is named by id and looked up again** by every command; devices
  are never taken from the command line except for `unlock`/`fsck --device`,
  which must be a LUKS volume / a file system of that target.
* **mount** (`target.py`): root, then `/boot`, `/boot/efi`, `/efi`, `/usr`,
  `/var` from the installation's `fstab` (nothing else — `/home` is left
  alone), `/dev` (rbind), `/proc`, `/sys`, efivars, `/run`. Every mount is
  recorded in `/run/shadow-recovery/targets/ID.json`; **umount** unmounts
  exactly that list and names the processes in the way when it is busy. Only
  Shadow OS installations are mounted; the running system is not.
* **fsck** refuses a mounted file system (the running root above all), checks
  with `-n` first and repairs only with `--repair`, and only what the check
  found damaged.
* **repair-boot** refuses when `/boot` or the EFI partition of the target is
  not mounted, when there is no `boot.conf`, and — for an encrypted system —
  when the LUKS volume is open under another name than the installation's
  `crypttab` uses (an initramfs built then would not unlock the disk).
  `unlock` therefore re-opens the volume under the `crypttab` name.
* **Secrets** are read from stdin, passed to `cryptsetup --key-file -` /
  `chpasswd` on their stdin, and never logged (`<stdin: redacted>`).
* **logs** creates the archive with the *calling user's* rights
  (`PKEXEC_UID`), mode 0600, `O_EXCL|O_NOFOLLOW`; `/etc/shadow` is never included.
* **Log**: every command that changes something appends its commands and
  their output to `/var/log/shadow-recovery/recovery-<date>.log` (0600).
* No shell anywhere: argument vectors only.
* It does not import the installer engine (`shadow-installer` exists on the
  live medium only).

Test overrides (for a root shell; pkexec does not pass environment on):
`SHADOW_RECOVERY_TEST_DEVICES=/dev/loopN,…` confines the engine to those loop
devices; `SHADOW_RECOVERY_STATE_DIR`, `SHADOW_RECOVERY_LOG_DIR`.

## The window

`recovery.{h,cpp}` drive the engine: one run at a time, started as
`pkexec /usr/sbin/shadow-recovery-engine <arguments>` (directly when already
root), secrets written to its stdin and wiped. Polkit:
`polkit/org.shadow.recovery.policy` — action `org.shadow.recovery.run`, bound
to exactly that program, `auth_admin_keep`;
`polkit/49-shadow-recovery-live.rules` grants it without a password to the
live user while `/run/shadow/live-session` exists. Actions that need the
installation mounted run the engine's `mount` first (shown in the output);
the file-system check runs `umount` first. Confirmations: file-system repair
(`NxConfirmDialog`, destructive), new password (account + typed twice, in a
dialog that says it cannot be undone), restart. Restart is logind `Reboot`
(offered only when `CanReboot` allows it). The terminal is
`shadow-terminal -e pkexec shadow-recovery-engine shell --target ID --exec`;
without SHADOW Terminal the row shows the console command instead.

`shadow-recovery --dump status|scan`; `--engine PATH` / `$SHADOW_RECOVERY_ENGINE`
(development); `--scan-file FILE` (test mode: shows a recorded scan result,
runs no engine, all actions disabled — used by the smoke test with
`tests/scan-sample.json`, a recording of the engine test below).

## Run-time dependencies (Debian packages)

`python3`, `util-linux` (lsblk, blkid, mount, findmnt), `e2fsprogs`,
`dosfstools`, `cryptsetup-bin`, `lvm2`, `coreutils`, `pkexec`, `polkitd` and an authentication agent,
`systemd` (journalctl for the log archive, logind for the restart), the
SHADOW UI library (`shadow-ui`). Recommended: `shadow-terminal`, `efibootmgr`.
Inside the installation being repaired: `initramfs-tools`, `shadow-boot`,
`grub2-common`, `dpkg`, `apt`, `passwd` — what a Shadow OS installation has.

## Tested (build chroot, as root)

* `tests/desktop/smoke.sh --dir recovery recovery` — 2 passed (no engine
  installed → "cannot work here"; test mode with the recorded scan), no QML warnings.
* `scripts/dev.sh exec --dir recovery -- bash /src/system/recovery/tests/engine-test.sh`
  — **98 checks, 0 failed**. `tests/fake-system.sh` builds a fake installed
  system on loop devices backed by files in a temporary directory (ext4 root
  with os-release, fstab, boot.conf, accounts, a real `chpasswd` with PAM and
  a shell; a second file system for `/boot/efi`; a LUKS2 volume; a FAT and a
  plain data file system) and guards every call: the engine only ever sees
  those loop devices. Covered: scan (and that it writes nothing — image
  checksum unchanged — and leaves nothing mounted; links resolved inside the
  target; missing `fstab` device reported; LUKS volume listed; btrfs root),
  fsck clean / deliberately damaged (found, not touched without `--repair`,
  repaired with it, check-before-repair order) / FAT / refused when mounted
  or foreign device, mount (what is mounted, warnings, twice, read-only),
  repair-boot and repair-packages `--dry-run` command sequences, the same
  with stand-in tools (order, chroot, live output, a failing step stops the
  rest, refusals without `boot.conf` / unmounted ESP), reset-password (hash
  changes and verifies, other accounts untouched, not in the log, refusals),
  logs (contents, no shadow, never overwrites, written with the caller's
  rights), shell, umount (busy with the process named, clean afterwards,
  the host's `/dev` intact), the engine's own guards.
* `scripts/dev.sh exec --dir recovery -- bash /src/system/recovery/tests/gui-test.sh`
  — **14 checks, 0 failed**: the window against the real engine and the same
  fake system, operated with xdotool: scan, Save logs (dialog), Set password
  (dialog; verified in the fake system's shadow file), Repair the boot loader
  (stand-in tools), Check (unmounts first; only `-n` was run), Mount/Unmount,
  Unlock (see below). Screenshots `build/work/shots/recovery-gui-1…8-*.png`,
  `recovery-1-noengine.png`, `recovery-2-testmode.png`.
* `cmake --install --component shadow-recovery` installs the files listed above.

## Not tested — to be done in a VM

* **`repair-boot` with the real tools** (`update-initramfs`,
  `shadow-grub-install`, `update-grub`): only its command sequence (dry run)
  and its control flow with stand-ins were run. Same for **`repair-packages`**
  (`dpkg`, `apt-get`), including the network inside the chroot.
* **LUKS unlock/lock, the re-open under the crypttab name, LVM activation**:
  the build host's kernel has no device-mapper (`cryptsetup open` fails; the
  engine and the window report that honestly). `luksFormat` and the
  detection of a locked volume worked.
* A FAT EFI partition being *mounted* (that kernel has no vfat; the fake ESP
  was ext4, FAT was only checked with `fsck.vfat`), efivars, xfs/f2fs roots.
* The running system as target (`--target running`), an unrestricted scan of
  real disks and the live-medium exclusion — the engine was never run
  outside the loop-device restriction here, on purpose.
* pkexec/polkit (policy file, live rule, authentication dialog): the window
  was run as root, which starts the engine directly. Restart through logind,
  the terminal action (no `shadow-terminal` build was on PATH), the journal
  export (the fake system has no journal).

## Limitations

* Only Shadow OS installations are repaired; other systems are listed but not
  selectable.
* `repair-packages` needs working name resolution inside the installation to
  download packages; Recovery does not touch the installation's network
  configuration.
* Root file systems on RAID, bcachefs or ZFS, and `/boot` on an encrypted
  volume, are not handled. btrfs/xfs/f2fs are found and mounted but not
  checked (`fsck` reports them as unsupported).
* The password action does not check password quality and cannot unlock data
  encrypted with the old password.
* On the live medium the engine's log lives in memory; save the logs before
  restarting if you need them.

## Needed outside this directory

* `debian/control`: a package `shadow-recovery` (Architecture: any; Depends:
  `${shlibs:Depends}, ${misc:Depends}, shadow-ui (= ${binary:Version}),
  python3, util-linux, e2fsprogs, dosfstools, cryptsetup-bin, lvm2, pkexec,
  polkitd, systemd`; Recommends: `shadow-terminal, efibootmgr`). `debian/rules`
  picks it up through the CMake component of the same name; a
  `python3 -m py_compile system/recovery/engine/shadow_recovery/*.py` check
  like the installer's would fit there.
* The package belongs in the live image *and* on installed systems.
