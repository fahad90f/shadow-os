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
| `/etc/default/grub.d/10-shadow.cfg` | distribution defaults: no saved default, recovery entries kept, `GRUB_GFXMODE=1280x800,1280x720,1024x768,auto` (kept for the kernel, so the splash starts without a mode switch), theme path |

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
`/usr/lib/grub`, `/usr/lib/shim` and `/usr/share/grub/themes/shadow`. When
an update of the GRUB or shim packages, or of `shadow-artwork` (the boot menu
theme), changes files there, dpkg runs the `shadow-boot` postinst with
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
| GRUB menu theme | `assets/boot/grub/shadow/` (`theme.txt`, images, fonts) | `/usr/share/grub/themes/shadow` (package `shadow-artwork`); copied to `/boot/grub/themes/shadow` by `shadow-grub-install`; copied onto the ISO by `assemble.sh` |
| Plymouth splash | `assets/boot/plymouth/shadow/` (`shadow.plymouth`, `shadow.script`, about 175 images) | `/usr/share/plymouth/themes/shadow` (package `shadow-artwork`) |

The binary parts of both are regenerated by `assets/src/boot/gen-boot-theme.sh
<dir>`, which calls `gen-plymouth-theme.py` (splash images) and
`gen-grub-theme.sh` (boot menu; with `--images-only` where `grub-mkfont` is
missing, as in the build chroot). The boot menu theme is described in
[boot-menu.md](boot-menu.md).

* If the theme file is missing, GRUB shows its plain text menu; the live
  menu falls back to a cyan/black colour scheme.
* The Plymouth theme is selected by the image hook
  `distro/base/hooks/60-plymouth.sh` when Plymouth and the theme are both
  installed (desktop image); the initramfs is regenerated afterwards.
* The minimal image has no Plymouth; on an encrypted installation made from
  it the passphrase is asked for on the text console.

### The splash

The theme uses Plymouth's `script` module. One script draws boot, the prompt
for the disk-encryption passphrase, messages, offline updates, shutdown and
restart. Escape switches to the text log as with any Plymouth theme.

What is on the screen:

* **The core.** The Shadow symbol lights up in the middle (its cyan node, the
  S, then the hexagon from the top corner down both sides), followed once by
  a short "lock" (a cyan and a violet copy closing in, three refreshes) and
  one scan beam.
* **The instrument** around it: a plate with six index marks, a ring of 60
  segments (30 on small screens), a slowly turning bezel of ticks and dashes,
  and four cyan brackets.
* **The network.** Traces at multiples of 30 degrees grow out of the six
  corners of the hexagon (82 segments, 16 nodes) and are then powered from the
  core outwards, cyan near the core, blue, then violet. This takes 2.8 seconds.
* **The text column** below: the wordmark, one line for the daemon's status
  string or the update percentage, the prompt with its entry field, and up to
  three message lines.

The script keeps decoration and data apart, and shows nothing as data that
the daemon did not report:

| On screen | Where it comes from |
|---|---|
| power-up of the network, comet on the hexagon, turning bezel, breathing glow | decoration; a fixed clock, not boot progress |
| lit part of the segment ring | Plymouth's boot-progress estimate, never ahead of it; after the first `plymouth system-update --progress=N` that percentage instead |
| one pulse running out along a trace, and the line under the wordmark | one per `plymouth update --status=…` (the units systemd reports); the string is shown as it is |
| "NN %" under the wordmark | only after a client has sent `system-update --progress`; the Shadow updater does not send one |
| brackets closing from the wide to the narrow frame, one more beam | the root file system was reported mounted (`plymouth update-root-fs --new-root-dir`) |
| message lines | `plymouth display-message`; `hide-message` removes the line with the same text |
| prompt text, number of cells in the field | `plymouth ask-for-password`; one cell per typed character, the text itself is never drawn |

Behaviour worth knowing:

* **Passphrase.** The prompt, the field and the cells are complete in the
  refresh in which the request arrives, also in the middle of the entrance
  animation (it is cut short). The brackets then move from the instrument to
  the field and return after Enter. The theme is not told whether the
  passphrase was right and shows nothing about it. A question
  (`plymouth ask-question`) uses the same field and shows the typed answer.
* **Messages** are placed from the bottom of the common screen area upwards
  when there is no room, so the line "Installing updates — do not turn off
  the computer" is visible at 640x480 too. Up to three lines are shown,
  newest last, each starting at the same place. A line that is too wide is
  broken once at a space (in a smaller size if that makes it fit); what still
  does not fit is cut and marked with a small arrow. A passphrase prompt
  text that is too wide is treated the same way.
* **Shutdown and restart** start from the powered picture and take the power
  back to the core. At shutdown a short arc then goes round the ring
  counter-clockwise; at restart the six spokes stay lit and two arcs go round
  clockwise. There are no words.
* **Offline updates.** `shadow-offline-update` switches the splash to its
  updates picture (`plymouth change-mode --updates`: network unpowered, one
  arc on the ring) before it shows its message, and to the restart picture
  (`change-mode --reboot`) before it restarts. The order matters: a mode
  change restarts the script and drops a message shown before it.
* **Screen sizes.** Three image sets: small (below 1000x700), medium
  (1024x768 to 1440x900) and large (1600x900 and above); on screens of at
  least 3000x1700 the large set is drawn at twice the size. With several
  screens the picture is laid out for the smallest and centred on each.
* **Options** in the `[script-env-vars]` group of `shadow.plymouth`:
  `motion=calm` gives a still picture in which only the ring, the texts and a
  slow glow change (for people who must not be shown motion; the default is
  `full`); `status=0` hides the status line. The splash does not follow the
  desktop's Reduce motion setting. Choosing `calm` means editing
  `/usr/share/plymouth/themes/shadow/shadow.plymouth` by hand; the next
  upgrade of `shadow-artwork` puts the packaged file back, and the boot and
  passphrase screens see the change only after `update-initramfs -u`
  (shutdown and restart read the file directly).
* **Slow screens.** The script counts how often it is refreshed. When two
  measuring windows of about two seconds in a row stay below 26 refreshes a
  second it first stops the bezel; after two more it reduces the comet and
  the glow. It never steps back up.

Files and tools:

* `shadow.script` is written by hand. Its header lists the constructs that
  must not be used (each one emptied the screen or killed the daemon while it
  was developed). A syntax error means an empty screen at boot, at the
  passphrase prompt and at shutdown, and Plymouth reports it only on its own
  error output.
* `assets/src/boot/gen-plymouth-theme.py <dir>` writes all images from
  `assets/logos/shadow-symbol.svg` (python3, `rsvg-convert`, the Inter font).
  The images are committed.
* `tests/build/test_plymouth_theme.py` checks that the script and the image
  files match, that the directory stays below 800 kB (everything in it is
  copied into every initramfs; it is about 700 kB) and that the forbidden
  constructs are absent. It does not run the script.
* `debian/shadow-artwork.triggers` asks for a new initramfs when the package
  is installed or upgraded; without it boot would keep showing the old theme
  while shutdown shows the new one.

What was verified and what was not: the theme was run in the real `plymouthd`
(24.004.60) with its X11 renderer, driven by the real `plymouth` client, for
boot, passphrase, question, messages, updates, shutdown and restart at
640x480, 1024x768, 1280x720, 1280x800, 1920x1080 and 3840x2160, with two
screens of different size, and with the fonts an initramfs holds (Noto Sans
and Noto Mono only). There it used about 11 % of one processor core at
1920x1080 (the previous theme: 21 %) and about 40 to 45 % at 3840x2160 with a
device scale of 2 (previously 94 %). At that size it drew 28.3 to 29.0 frames
a second on a quiet machine, close to the 28 it aims for; with other load on
the machine it fell below 28. Not verified: a real boot with the
frame-buffer or DRM renderer, the real cryptsetup prompt, the Escape toggle
on a console, the hand-over to the login screen (Plymouth quits without
keeping the splash, so a short gap is expected), and the initramfs trigger.
The encrypted installation test (minimal image) enters the passphrase on the
console.
