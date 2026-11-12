# Recovering a system that does not boot

Three levels, from least to most effort: another entry in the boot menu,
the recovery-mode entries, and repair from the installation medium.

> None of the procedures on this page has been carried out on a broken
> Shadow OS system. They follow from how the installer lays the system out
> and from how `shadow-grub-install` works; the commands themselves are the
> ones the installer runs. Read a procedure to the end before starting, and
> if the data matters, copy it off the disk first (step 3 of the chroot
> procedure gives you access to it).

There is no graphical recovery tool yet. The boot menu of the medium has an
entry "Recovery environment"; it starts a live session that asks the shell
to open `shadow-recovery`, a program that is not part of the image. Use the
normal live session and the procedure below instead.

## 1. Boot menu options

Hold **Shift** (BIOS) or press **Esc** (UEFI) during start if the menu does
not appear; it is shown for 5 seconds by default.

| Situation | Try |
|---|---|
| A new kernel does not boot | **Advanced options for Shadow OS** → the previous kernel version |
| Black screen with any kernel | press **e** on the entry, append `nomodeset` to the `linux` line, **Ctrl+X** ([README.md](README.md)) |
| You want to see where the boot stops | press **e**, remove `quiet splash` from the `linux` line, **Ctrl+X** |
| The boot stops at an `(initramfs)` prompt | the root filesystem was not found or not unlocked; see "Common repairs" below |

Changes made with **e** apply to that one boot only.

## 2. Recovery-mode entries

`update-grub` creates a "(recovery mode)" entry for every kernel under
*Advanced options*; Shadow OS keeps them (`GRUB_DISABLE_RECOVERY=false` in
`/etc/default/grub.d/10-shadow.cfg`). Recovery mode boots with `single`
into systemd's rescue target with only the root filesystem and basic
services.

**Limitation you must know about:** the rescue shell is opened by
`sulogin`, which asks for the **root password**. Shadow OS locks the root
account — the installer runs `passwd --lock root`, and administration goes
through `sudo`. With a locked root account, Debian's `sulogin` is expected
to refuse to open a shell. This has not been checked on Shadow OS, but you
should assume that recovery mode gets you no shell unless you gave root a
password beforehand:

```sh
sudo passwd root        # on a working system, if you want recovery mode to be usable
```

Setting a root password weakens the "no root login" default; whether that
trade is worth it is your decision. Without it, use the live medium.

## 3. Repair from the installation medium

This works for every kind of damage that leaves the disk readable: broken
boot loader, missing firmware boot entry, interrupted update, broken
initramfs, forgotten user password.

### Step 1: boot the medium

Boot the Shadow OS medium — **in the same firmware mode as the installed
system** (UEFI or legacy BIOS). The firmware boot entry can only be
rewritten when the live session itself was booted through UEFI. Choose
"Live session" (or "Live session (safe graphics)"). The minimal image works
too and has the same tools.

Open a terminal (desktop image: there is no terminal emulator in the image
yet, so switch to a text console with **Ctrl+Alt+F3** and log in as
`shadow` / `live`) and become root:

```sh
sudo -i
```

### Step 2: identify the partitions

```sh
lsblk -o NAME,SIZE,FSTYPE,LABEL,PARTLABEL,MOUNTPOINTS
```

The installer labels what it creates:

| Label (`LABEL`) | What |
|---|---|
| `SHADOW-ESP` (vfat) | EFI System Partition → `/boot/efi` |
| `shadow-boot` (ext4) | `/boot` — only on encrypted installations |
| `shadow-root` (ext4) | root filesystem `/` |
| `shadow-home` (ext4) | `/home`, if separate |
| `shadow-swap` | swap partition, if any |
| `shadow-crypt` (crypto_LUKS) | the encrypted container, on encrypted installations |

With "alongside" installations the ESP may be an existing one with a
different label; with manual installations the labels are whatever the
partitions had. In the examples below replace `/dev/sdXn` with your
devices. **Check twice: the live medium itself also appears in the list.**

### Step 3: mount the installed system

Unencrypted:

```sh
mount /dev/sdX3 /mnt                 # shadow-root
mount /dev/sdX2 /mnt/boot/efi        # SHADOW-ESP (UEFI installations)
mount /dev/sdX4 /mnt/home            # only if /home is separate
```

Encrypted:

```sh
cryptsetup open /dev/sdX4 shadow_crypt      # the shadow-crypt partition; asks for the passphrase
vgchange -ay shadow
mount /dev/shadow/root /mnt
mount /dev/sdX3 /mnt/boot                  # shadow-boot
mount /dev/sdX2 /mnt/boot/efi              # SHADOW-ESP
mount /dev/shadow/home /mnt/home            # only if it exists: ls /dev/shadow
```

The mapping name must be the one in the first column of the installed
system's `/etc/crypttab` (`cat /mnt/etc/crypttab`), otherwise the initramfs
rebuilt in step 5 will not find the volume. The installer uses
`shadow_crypt` and the volume group `shadow`, or `shadow_crypt1` / `shadow1`
if those names were already taken when it ran. If the name differs,
unmount, `vgchange -an`, `cryptsetup close shadow_crypt` and open it again
under the right name.

At this point your files are readable under `/mnt`. **If you only need
your data, copy it to another disk now.**

### Step 4: enter the system

These are the mounts the installer itself uses:

```sh
mount --rbind /dev /mnt/dev
mount --make-rslave /mnt/dev
mount -t proc proc /mnt/proc
mount -t sysfs sysfs /mnt/sys
mount --bind /run /mnt/run
[ -d /sys/firmware/efi/efivars ] && mount -t efivarfs efivarfs /mnt/sys/firmware/efi/efivars
chroot /mnt /bin/bash
```

You are now root inside the installed system. For network access inside it
(needed for `apt`), copy the live session's resolver configuration before
entering: `cp -L /etc/resolv.conf /mnt/etc/resolv.conf` — but first note
whether `/mnt/etc/resolv.conf` is a symbolic link (`ls -l`), and restore it
afterwards.

### Step 5: repair

Run only what applies.

**Finish an interrupted update:**

```sh
dpkg --configure -a
apt -f install
```

**Rebuild the initramfs** (after `(initramfs)` prompts, a changed
`/etc/crypttab` or `/etc/fstab`, or a kernel without an initramfs):

```sh
update-initramfs -u -k all
```

**Re-install the boot loader and regenerate the menu:**

```sh
cat /etc/shadow-os/boot.conf          # how this system boots; see below
shadow-grub-install --verbose
update-grub
```

`shadow-grub-install` re-installs GRUB exactly the way the system was
installed: the signed shim and GRUB to `EFI/debian` on the ESP (and to
`EFI/BOOT` if `ESP_REMOVABLE=yes`), the "Shadow OS" firmware entry if
`ESP_NVRAM=yes` and the live session was booted through UEFI, and the
legacy boot sector on `BIOS_DISK`. It needs the ESP mounted at the
`ESP_MOUNT` path (`/boot/efi`) — that is why step 3 mounts it. Do **not**
run `grub-install` by hand with other options: the boot-loader id must be
`debian` ([../architecture/boot.md](../architecture/boot.md)), and the next
GRUB update would re-install from `boot.conf` anyway.

Running `shadow-grub-install` in the live session itself, outside the
chroot, does nothing: the live system has no `/etc/shadow-os/boot.conf`.

**Reset a forgotten user password:**

```sh
passwd <user name>
```

(This does not help with a forgotten disk-encryption passphrase. That
cannot be recovered.)

**Remove a package that breaks the boot or the desktop,** or hold one:

```sh
apt remove <package>
apt-mark hold <package>
```

### Step 6: leave and restart

```sh
exit                                   # leave the chroot
umount -R /mnt
vgchange -an shadow                     # encrypted installations only
cryptsetup close shadow_crypt           # encrypted installations only
systemctl reboot
```

Remove the medium when the machine restarts.

## Common repairs

### The firmware no longer lists "Shadow OS"

Firmware updates, a CMOS reset or another operating system's installer can
delete boot entries. Do the chroot procedure in UEFI mode and run
`shadow-grub-install --verbose`; it creates the entry again. If the firmware
refuses to store entries, set the removable path: in
`/etc/shadow-os/boot.conf` change `ESP_REMOVABLE=no` to `ESP_REMOVABLE=yes`
and run `shadow-grub-install --verbose` again — but only if no other
operating system uses `EFI/BOOT` on that ESP, because it will be
overwritten.

### `/etc/shadow-os/boot.conf` is missing or wrong

Write it by hand. For a UEFI installation with its own ESP:

```
FIRMWARE=uefi
ESP_MOUNT=/boot/efi
ESP_REMOVABLE=yes
ESP_NVRAM=yes
```

For a legacy BIOS installation (use the disk's stable name from
`ls -l /dev/disk/by-id/`, the whole disk, not a partition):

```
FIRMWARE=bios
BIOS_DISK=/dev/disk/by-id/<id of the disk>
```

All keys are described in [../architecture/boot.md](../architecture/boot.md).

### Another operating system took over the boot

Windows and other installers sometimes put themselves first in the
firmware's boot order. Select "Shadow OS" in the firmware's boot menu and
move it to the top of the order in the firmware setup, or from the running
SHADOW system: `efibootmgr` lists the entries, and
`sudo efibootmgr --bootorder XXXX,YYYY` sets the order. If the entry is
gone, see above. If another Debian-based system overwrote `EFI/debian` on a
shared ESP, re-run `shadow-grub-install` from the chroot; the two systems
will keep overwriting each other, so give them separate ESPs.

### The system starts but hangs before the login screen

Boot once without `quiet splash` (section 1) to see the failing unit. From
the chroot you can read the previous boots' journal:

```sh
journalctl -D /mnt/var/log/journal -b -1 -p err      # from the live session, outside the chroot
```

A filesystem named in `/etc/fstab` that no longer exists (a removed disk)
stops the boot; comment its line out in `/mnt/etc/fstab`.

### The root filesystem needs checking

From the live session, with the filesystem **not mounted**:

```sh
e2fsck -f /dev/sdX3            # unencrypted root
e2fsck -f /dev/shadow/root      # encrypted: after cryptsetup open and vgchange -ay
```

### The disk is failing

`smartctl -a /dev/sdX` (or `nvme smart-log /dev/nvme0`) from the live
session. If it reports errors, stop repairing and copy your data off
first. `ddrescue` is not in the image; it is in Debian as `gddrescue`.

## Reinstalling while keeping `/home`

If `/home` is a separate partition, the installer's **manual** mode can
format `/` and keep `/home`: assign the old root partition to `/` with
formatting, the ESP to `/boot/efi` without formatting and the home
partition to `/home` without formatting
([../installation/installing.md](../installation/installing.md)). This is
not possible for encrypted installations, because manual mode does not
support encryption, and it is the scenario whose automated test has a
re-run pending. Back up `/home` first.
