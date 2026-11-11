# Installing Shadow OS

This guide takes you from the boot menu of the installation medium to a
restarted, installed system.

> **Read this first.** The installer engine has been verified in QEMU for
> whole-disk installations (UEFI, UEFI with Secure Boot, legacy BIOS), for
> whole-disk with LUKS2 encryption, and for installation alongside another
> system in free space. A real installation **driven by the graphical
> installer** has not been run yet, and **nothing has been installed on a
> physical machine**. Back up your data before you start, and prefer a spare
> machine or disk. See [../testing.md](../testing.md).

## Requirements

Checked by the installer (`probe` in the engine):

| Item | Requirement |
|---|---|
| Architecture | amd64. (The engine also accepts arm64, which has never been built or tested) |
| Firmware | 64-bit UEFI, or legacy BIOS on amd64. 32-bit UEFI is refused |
| Memory | at least 2 GiB (refused below); 4 GiB or more recommended for the desktop (warning below) |
| Disk | at least 20 GiB, not the installation medium, not read-only. The root filesystem additionally needs at least 16 GiB or 1.25 × the image's installed size + 4 GiB, whichever is larger |
| Power | on battery: warning; on battery below 30 %: refused |
| Network | not required. Without it the optional tool profiles cannot be installed during installation |

## 1. Boot the medium

Write the ISO to a USB stick as described in the [README](../../README.md)
and boot from it. Menu entries are described in
[../architecture/boot.md](../architecture/boot.md).

* **Live session** starts the desktop from the medium; nothing is written to
  your disks. Use it to try your hardware first.
* **Install Shadow OS** starts the same live session and opens the installer.
* **Live session (safe graphics)** if the screen stays black.

In the live session you are logged in automatically as the user `shadow`
(password `live`; `sudo` works without a password). The system runs from a
read-only image with a writable overlay in memory: changes are lost at
shutdown.

To start the installer from a running live session, use the **Install**
button in the top panel, or find "Install Shadow OS" in the launcher.

## 2. The graphical installer, step by step

The installer (`shadow-installer`, package `shadow-installer-gui`) collects
your choices, asks the installation engine what they mean, shows the
engine's plan word for word and lets the engine carry it out after you
confirm. It runs as the live user; the engine is started through polkit
(no password is asked in the live session). **Nothing is written to a disk
before you confirm the Summary.**

| # | Step | What you do |
|---|---|---|
| 1 | Welcome | Choose the language of the installed system (searchable list). The installer's own texts are English only |
| 2 | Keyboard | Choose layout and variant; try it in the test field. "Use in this session" also switches the live session's layout |
| 3 | System check | Read the result of the hardware check. A failed item blocks the installation; warnings do not |
| 4 | Time zone | Choose region and zone |
| 5 | Disk | Choose the target disk and the mode (below); optionally encryption, a separate `/home`, and the kind of swap |
| 6 | User | Full name, user name, password, host name, optional automatic login |
| 7 | Software | Optionally select SHADOW Arsenal tool profiles. The step is hidden if no profiles are present on the medium |
| 8 | Summary | Review the engine's plan: every step, what is destroyed, and warnings. Tick the acknowledgement. If a step is destructive, type the name of the device that loses data |
| 9 | Installation | Watch the steps as the engine reports them. It cannot be cancelled, and the window refuses to close while the engine runs |
| 10 | Finish | Restart, or stay in the live session. After a failure: the engine's message, a hint, and the log location |

Things to know:

* The installation medium is never offered as a target. Disks the engine
  considers unusable are listed under "Not offered" with the reason.
* Looking for other installed operating systems is a separate button
  ("Look for installed systems"); it mounts filesystems read-only and takes
  time, so it only runs when you ask for it.
* If the disks change between the Summary and the start of the installation
  (a device plugged in or removed, a partition mounted), the engine refuses
  to run the old plan. The installer returns to the Summary and shows the new
  plan, which you must confirm again.
* Sizes of partitions and swap are chosen automatically; the wizard does not
  let you set them. The unattended interface does
  ([unattended.md](unattended.md)).

## 3. Disk modes

### Erase disk

Destroys **everything** on the selected disk and creates a new GPT:

| Partition | Size | Notes |
|---|---|---|
| BIOS boot | 1 MiB | amd64 only; lets the disk also start in legacy mode |
| EFI System Partition | 512 MiB, FAT32 | mounted at `/boot/efi` |
| `/boot` | 1 GiB, ext4 | only with encryption; holds kernels, not encrypted |
| swap | automatic | only if you chose a swap partition (without encryption) |
| root `/` | rest of the disk, ext4 | or part of it with a separate `/home` |
| `/home` | rest, ext4 | only if you chose a separate `/home` |

With **encryption** the last partition is a LUKS2 container holding an LVM
volume group (`shadow`) with the logical volumes `root`, optionally `home`
and optionally `swap`.

With a separate `/home`, root gets 35 % of the available space, but not less
than 40 GiB and not more than 120 GiB; `/home` needs at least 8 GiB.

Refused if the disk is mounted or has active encrypted or logical volumes:
unmount or close them first.

### Alongside another system (free space)

Creates new partitions in the **largest unallocated region** of the selected
disk. Existing partitions are not modified.

* Offered only for disks with a GPT partition table that have unallocated
  space. The installer does not shrink partitions. Make room first with the
  other system's own tool (for Windows: Disk Management), then start the
  installer again.
* On UEFI an existing EFI System Partition on that disk is **reused without
  formatting**; Shadow OS adds its own directory to it.
* Other operating systems found by os-prober are added to the SHADOW boot
  menu.
* If the installation fails, the partitions this attempt created are removed
  again.

### Manual

Assigns **existing** partitions to mount points; there is no partition
editor. Prepare the partitions beforehand (for example with `parted` or
`gdisk` in the live session).

* Mount points: `/`, `/boot`, `/boot/efi`, `/home`, `/var`, `/srv`, `/opt`,
  `/tmp`; plus swap partitions.
* `/` must be formatted (ext4). Installing over an existing system's files
  is not supported.
* Other partitions can be formatted (ext4; FAT32 for `/boot/efi`) or kept.
  Kept filesystems may be ext4, ext3, XFS, btrfs, FAT or F2FS.
* UEFI: you must assign an EFI System Partition to `/boot/efi` (at least
  100 MiB; it does not need formatting). BIOS: you must choose the disk for
  the boot loader; a GPT disk needs a 1 MiB "BIOS boot" partition.
* Encryption is not available in manual mode.

## 4. Encryption

Available with "erase disk" and "alongside".

* LUKS2, cipher `aes-xts-plain64` with a 512-bit key, key derivation
  Argon2id, hash SHA-512.
* The passphrase must have at least 8 characters. **It cannot be
  recovered.** Without it the data is lost.
* `/boot` and the EFI System Partition are not encrypted (the boot loader
  and kernel must be readable before the passphrase is entered). The boot
  chain is protected by Secure Boot, not by encryption.
* You are asked for the passphrase at every boot. On a system installed
  from the minimal image the prompt is on the text console (this is the
  tested case); on the desktop image the boot splash is meant to show it
  (not yet verified).
* TRIM (`discard`) is enabled on the encrypted volume.
* There is no TPM unlocking, no key file and no recovery key.

Parameters and trade-offs: [../security/hardening.md](../security/hardening.md).

## 5. Swap

| Choice | Result |
|---|---|
| swap file (default) | `/swapfile`, half the memory size, at least 2 GiB and at most 8 GiB; reduced automatically on small disks |
| swap partition | size of the memory, at least 2 GiB and at most 32 GiB; with encryption it is a logical volume inside the encrypted container. Used as the resume device |
| none | no swap |

## 6. Dual boot

* Use the "alongside" mode, on a disk with a GPT and free space, in the same
  firmware mode (UEFI or BIOS) as the other system.
* **Windows:** turn off "fast startup", suspend BitLocker before changing
  partitions (or have the recovery key at hand), and shrink the Windows
  partition from within Windows. Dual boot with Windows has not been tested;
  the automated test uses a small Linux "other system".
* **Debian on the same ESP:** Shadow OS installs its loader to `EFI/debian`,
  the directory Debian uses too. The two would overwrite each other's
  loader. Do not share an ESP with a Debian installation
  ([../architecture/boot.md](../architecture/boot.md)).
* The firmware boot entry is labelled "Shadow OS". Entries of other systems
  are not removed.
* With Secure Boot enabled, other systems started from the SHADOW boot menu
  must themselves be signed.

## 7. What gets written where

On the target system the engine:

| Step | Writes |
|---|---|
| Partitioning | the partition table (`sgdisk`), signatures wiped on new partitions |
| Encryption | LUKS2 header, LVM volume group and volumes |
| Formatting | ext4 / FAT32 / swap, labels `shadow-root`, `shadow-home`, `shadow-boot`, `SHADOW-ESP`, `shadow-swap` |
| Copy | the contents of `live/filesystem.squashfs` to the target |
| Configuration | `/etc/fstab` (by UUID), `/etc/crypttab`, `/etc/initramfs-tools/conf.d/resume`, `/etc/hostname`, `/etc/hosts`, `/etc/locale.gen` and `/etc/default/locale`, `/etc/localtime` and `/etc/timezone`, `/etc/default/keyboard`, `/etc/X11/xorg.conf.d/00-keyboard.conf`, `/etc/xdg/kxkbrc`, an empty `/etc/machine-id` (generated at first boot), `/swapfile` |
| User | your account with a home directory, member of `sudo` and the usual desktop groups; the root account is locked; `/etc/sddm.conf.d/20-autologin.conf` if you chose automatic login |
| Clean-up | removes the live-session packages (`live-boot`, `live-config`, `live-tools`, `user-setup`, `shadow-live`, `shadow-installer`, `shadow-installer-gui`) and their leftovers |
| Initramfs | a new initramfs for every installed kernel |
| Boot loader | `/etc/shadow/boot.conf`, `/etc/default/grub.d/60-shadow-installer.cfg`, GRUB on the ESP or in the boot sector (`shadow-grub-install`), the firmware boot entry, `/boot/grub/grub.cfg` |
| Profiles | only if selected with "install now": see the note below |
| Finish | `/var/log/installer/install.log` and `plan.json`; everything is unmounted |

> **Tool profiles during installation.** The engine installs the selected
> `shadow-arsenal-<profile>` metapackages with
> `apt-get install --no-install-recommends`. The metapackages list their
> tools as *Recommends*, so as implemented this installs the metapackage
> without its tools. A failure of this step does not fail the installation.
> Install profiles after the first start instead
> ([../security/toolkit.md](../security/toolkit.md)).

The installed system is the live system minus the live packages: the same
packages, already configured, not a fresh package installation.

## 8. Logs

| Where | What |
|---|---|
| `/var/log/shadow-installer/install-<date>-<time>.log` in the live session | the engine's full log: every command, its output, every event. Root only (mode 0600) |
| `/var/log/installer/install.log`, `/var/log/installer/plan.json` on the installed system | copy of that log and the executed plan. Root only |
| "Copy log" in the installer | what the engine sent to the wizard in this run (events and error output), not the file above |

Passwords and the passphrase are passed to commands on standard input and
are not written to the log; the installation test checks the logs for them.

If the installation fails, note the message and hint, then read the log in
the live session:

```sh
sudo less /var/log/shadow-installer/install-*.log
```

See [../troubleshooting/README.md](../troubleshooting/README.md).

## 9. First start

Remove the medium and restart. On an encrypted system enter the passphrase.
Log in with the account you created. Then:

* connect to a network and install updates
  ([../security/updates.md](../security/updates.md));
* check the firewall state: `sudo shadow-firewall status`;
* read [../limitations.md](../limitations.md).

## Installing without the graphical installer

On the minimal image, or for automation, drive the engine directly:
[unattended.md](unattended.md).
