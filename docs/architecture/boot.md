# Boot architecture

How Shadow OS starts: from the installation medium and from an installed
disk, under UEFI (with and without Secure Boot) and under legacy BIOS.

Sources: `system/boot/shadow-boot/`, `system/boot/grub/live-grub.cfg.in`,
`build/iso/stage-rootfs.sh`, `build/iso/assemble.sh`,
`distro/base/package-lists/20-boot.list`, `debian/shadow-boot.*`.

What has been verified (QEMU only, see [../testing.md](../testing.md)): the
minimal ISO boots under UEFI, UEFI with Secure Boot enforcing and legacy
BIOS, from CD and USB; systems installed by the engine boot from disk under
all three. Nothing has been booted on physical hardware.

## UEFI boot chain

```
UEFI firmware
  │  verifies with the Microsoft UEFI CA (present on retail PCs)
  ▼
shim            shimx64.efi   — Debian's shim, signed by Microsoft   (package shim-signed)
  │  verifies with the Debian key embedded in shim (or an enrolled MOK)
  ▼
GRUB            grubx64.efi   — Debian's GRUB image, signed by Debian (package grub-efi-amd64-signed)
  │  verifies the kernel through shim
  ▼
Linux kernel    vmlinuz       — Debian's kernel, signed by Debian     (package linux-image-amd64)
  │  under Secure Boot: kernel lockdown, only signed modules
  ▼
initramfs → root filesystem → systemd
```

Shadow OS signs nothing in this chain. Every binary is taken unmodified from
the Debian packages installed in the image, which is why Secure Boot works
without enrolling a key and why SHADOW does not need to keep a Secure Boot
signing key. `mmx64.efi` (MOK manager) is shipped alongside so that you can
enrol your own keys, for example for DKMS modules.

Without Secure Boot the same binaries are used; shim simply does not enforce
anything.

A SHADOW-built kernel (not shipped, see [../kernel/README.md](../kernel/README.md))
would break this chain unless it is signed with a key that the user enrols.

## Why the boot-loader id is `debian`

Debian's signed GRUB image is a fixed binary. The directory it reads its
configuration from is compiled in: `EFI/debian` on the EFI System Partition,
wherever the binary itself is stored. Renaming the directory would require a
GRUB image built and signed by SHADOW.

`shadow-grub-install` therefore installs the loader with
`--bootloader-id=debian`. Only the label of the firmware boot entry says
"Shadow OS". On an installed system you will find:

```
/boot/efi/EFI/debian/shimx64.efi, grubx64.efi, mmx64.efi, grub.cfg
/boot/efi/EFI/BOOT/…          only if the removable-media path is maintained
```

Consequence: a Shadow OS installation and a Debian installation that share
one ESP use the same directory and will overwrite each other's loader. This
is a known limitation ([../limitations.md](../limitations.md)).

## `shadow-boot` and `shadow-grub-install`

Debian's `grub-pc` / `grub-efi-*` front-end packages install GRUB to a disk
from their maintainer scripts and ask debconf questions. Shadow OS does not
install them (`shadow-boot` conflicts with them). The image contains only the
`*-bin` and `*-signed` packages, which ship files under `/usr/lib` and never
write to a disk, plus the package `shadow-boot`:

| File | Purpose |
|---|---|
| `/usr/sbin/shadow-grub-install` | (re-)installs the boot loader the way this system was installed |
| `/etc/default/grub` | GRUB defaults: `GRUB_TIMEOUT=5`, `GRUB_DISTRIBUTOR="Shadow OS"`, `quiet splash` |
| `/etc/default/grub.d/10-shadow.cfg` | distribution defaults: no saved default, recovery entries kept, `GRUB_GFXMODE=auto`, theme path |

Further files in `/etc/default/grub.d/`: `20-shadow-hardening.cfg` (package
`shadow-hardening`, kernel command line) and `60-shadow-installer.cfg` (written
by the installer: extra kernel parameters, `GRUB_DISABLE_OS_PROBER`). Put
your own settings in a new file there, for example `90-local.cfg`, then run
`sudo update-grub`.

### `/etc/shadow-os/boot.conf`

The installer records how the system boots; `shadow-grub-install` reads it.

| Key | Meaning |
|---|---|
| `FIRMWARE=uefi\|bios` | how the installed system is started |
| `ESP_MOUNT=/boot/efi` | mount point of the EFI System Partition |
| `ESP_REMOVABLE=yes\|no` | also maintain the fallback path `EFI/BOOT` |
| `ESP_NVRAM=yes\|no` | keep a firmware boot entry labelled "Shadow OS" |
| `BIOS_DISK=/dev/disk/by-id/…` | legacy boot sector to maintain (required for `bios`) |
| `BIOS_DISK_OPTIONAL=…` | additional legacy boot sector on a UEFI system (best effort) |

If the file does not exist — the live session, a build chroot — the script
does nothing and exits successfully.

### What `shadow-grub-install` does

```
sudo shadow-grub-install [--verbose]
```

`--verbose` is the only option.

1. Copies the boot-menu theme from `/usr/share/grub/themes/shadow` to
   `/boot/grub/themes/shadow`. `/boot` is never encrypted on a Shadow OS
   installation, so GRUB can read the theme before any volume is unlocked.
2. `FIRMWARE=uefi`: requires the ESP to be mounted, then runs
   `grub-install --target=x86_64-efi --efi-directory=<ESP> --bootloader-id=debian --no-nvram --recheck`
   with `--uefi-secure-boot` when the signed GRUB and shim are installed
   (otherwise `--no-uefi-secure-boot` and a warning), and
   `--force-extra-removable` when `ESP_REMOVABLE=yes`. It fails if
   `EFI/debian/grub.cfg` was not written.
3. With `ESP_NVRAM=yes` and when booted through UEFI: deletes existing
   firmware entries labelled exactly "Shadow OS" and creates a new one that
   points at `\EFI\debian\shimx64.efi` (or `grubx64.efi` if there is no
   shim). Entries of other operating systems are not touched.
4. With `BIOS_DISK_OPTIONAL` set: also runs
   `grub-install --target=i386-pc` on that disk; failure is only a warning.
5. `FIRMWARE=bios`: runs `grub-install --target=i386-pc --recheck <BIOS_DISK>`;
   failure is an error. If `ESP_REMOVABLE=yes` and an ESP is mounted, it also
   installs the UEFI loader to the removable path, best effort.

It does not run `update-grub`. The installer runs `update-grub` after it;
kernel packages run it through their own hooks.

### dpkg triggers

`debian/shadow-boot.triggers` registers interest (`interest-noawait`) in
`/usr/lib/grub` and `/usr/lib/shim`. When an update of the GRUB or shim
packages changes files there, dpkg runs the `shadow-boot` postinst with
`triggered`, which calls `shadow-grub-install`. The same happens when
`shadow-boot` itself is upgraded.

If the refresh fails, the package run is **not** failed (that would leave
APT half-configured). Instead the postinst writes the time to
`/var/lib/shadow/boot-loader-refresh-failed` and prints a warning.
`shadow-security-status` reports the marker in its `secureboot` section. Fix
it with `sudo shadow-grub-install --verbose`; a successful run of the trigger
removes the marker.

Without this refresh the machine would keep booting the old copies on the
ESP after a GRUB or shim security update — including copies that a later
Secure Boot revocation (SBAT/dbx) would refuse.

The installation test runs `shadow-grub-install --verbose` on every installed
system as the "update path" check. The trigger itself (a real GRUB or shim
package update) has not been exercised.

## Legacy BIOS

Supported on amd64 through `grub-pc-bin`.

* **Live medium:** the ISO carries a GRUB El Torito image and a hybrid MBR
  (`boot_hybrid.img`), so it boots from optical media and from USB.
* **Installed system:** "erase disk" always creates a 1 MiB BIOS boot
  partition on amd64 and GRUB is written to the disk's boot sector. Because
  the installer also creates an ESP and (by default) installs the loader to
  the removable path, a disk installed in BIOS mode can still start if the
  firmware is later switched to UEFI, and a disk installed in UEFI mode gets
  a best-effort legacy boot sector (`BIOS_DISK_OPTIONAL`).
* Secure Boot does not exist in BIOS mode.

## Live media layout

`build/iso/assemble.sh` writes a hybrid ISO with xorriso:

```
/.disk/info                  media identification; Debian's signed "gcd" GRUB image
                             finds the boot medium by this file
/EFI/boot/bootx64.efi        shim (Microsoft-signed)
/EFI/boot/grubx64.efi        GRUB (Debian-signed, removable-media "gcd" variant)
/EFI/boot/mmx64.efi          MOK manager
/boot/grub/grub.cfg          boot menu, shared by UEFI and BIOS
/boot/grub/fonts/unicode.pf2
/boot/grub/themes/shadow/     boot menu theme
/boot/grub/i386-pc/          BIOS core image (eltorito.img) and modules
/live/vmlinuz, initrd.img    kernel and initramfs taken from the image
/live/filesystem.squashfs    the root filesystem
/live/filesystem.size, kernel-version
```

* The image carries a real GPT. Partition 1 is the ISO9660 filesystem;
  partition 2 is an appended EFI System Partition image (FAT, at least
  4 MiB) that holds the same three EFI binaries.
* UEFI firmware boots from the ESP (USB) or through the El Torito EFI entry
  (optical). BIOS boots through GRUB's hybrid MBR (USB) or El Torito
  (optical).
* The boot-loader binaries are copied from the image's own root filesystem,
  so the medium uses the same GRUB version the installed system will.
* The root filesystem is mounted by `live-boot` (`boot=live`) as squashfs
  with a writable overlay in RAM.

### Boot menu

Generated from `system/boot/grub/live-grub.cfg.in`. Default entry: Live
session; timeout 8 seconds.

| Entry | Kernel parameters | Purpose |
|---|---|---|
| Shadow OS - Live session | `boot=live components quiet splash` | the live desktop (console on the minimal image) |
| Shadow OS - Live session (safe graphics) | … `nomodeset` | no kernel mode setting: for black screens caused by a graphics driver |
| Install Shadow OS | … `shadow.autostart=installer` | live session that opens the installer |
| Advanced options ▸ Live session - verbose boot (no splash) | `boot=live components` | kernel and service messages instead of the splash |
| Advanced options ▸ Live session - copy system to RAM | … `toram` | the medium can be removed after boot |
| Advanced options ▸ Live session - serial console (ttyS0, 115200 8N1) | `boot=live components noeject console=tty0 console=ttyS0,115200n8` | headless machines and automated tests |
| Advanced options ▸ Recovery environment | `boot=live components shadow.autostart=recovery` | live session that asks the shell to start `shadow-recovery` (that program is not part of the image yet, see [../troubleshooting/recovery.md](../troubleshooting/recovery.md)) |
| Advanced options ▸ UEFI firmware settings | `fwsetup` | UEFI only |
| Advanced options ▸ Boot from the first local disk | exits GRUB (UEFI) or chain-loads `(hd0)` (BIOS) | skip the medium |

`shadow.autostart=…` is read by `system/live/2000-shadow-session`, which writes
the request to `/run/shadow/autostart`; the shell reads it once at start.

**Automated-test hook.** The menu reads the SMBIOS system serial number. If
it is `SHADOW-AUTOTEST` the serial-console entry is booted after one second;
if it is `SHADOW-AUTOTEST-INSTALLED` the local-disk entry is. The QEMU harness
(`tests/lib/qemu.py`) sets these serials. A real machine does not carry
them, so the menu behaves normally there.

## Installed-system boot menu

Generated by `update-grub` from `/etc/default/grub` and
`/etc/default/grub.d/*.cfg`. Recovery-mode entries are kept
(`GRUB_DISABLE_RECOVERY=false`), the default entry is always the newest
kernel (`GRUB_SAVEDEFAULT=false`), and other operating systems are added by
os-prober unless the installer disabled it (it does so for "erase disk").

## GRUB theme and Plymouth

| Item | Source | Installed as |
|---|---|---|
| GRUB menu theme | `assets/boot/grub/shadow/` (`theme.txt`, background, logo, three fonts, selection box) | `/usr/share/grub/themes/shadow` (package `shadow-artwork`); copied to `/boot/grub/themes/shadow` by `shadow-grub-install`; copied onto the ISO by `assemble.sh` |
| Plymouth splash | `assets/boot/plymouth/shadow/` (`shadow.plymouth`, `shadow.script`, images) | `/usr/share/plymouth/themes/shadow` (package `shadow-artwork`) |

Both are regenerated from the logo, wallpaper and fonts by
`assets/src/boot/gen-boot-theme.sh`.

* If the theme file is missing, GRUB shows its plain text menu; the live
  menu falls back to a cyan/black colour scheme.
* The Plymouth theme uses the `script` module: logo, progress bar, boot
  messages and the prompt for the disk-encryption passphrase. Escape switches
  to the text log. It is selected by the image hook
  `distro/base/hooks/60-plymouth.sh` when Plymouth and the theme are both
  installed (desktop image); the initramfs is regenerated afterwards.
* The minimal image has no Plymouth; on an encrypted installation made from
  it the passphrase is asked for on the text console.

The encrypted installation test (minimal image) enters the passphrase on the
console. The Plymouth passphrase prompt of the desktop image has not been
verified.
