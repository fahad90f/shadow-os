# NEXUS OS

NEXUS OS is a Linux distribution for security work and software development,
based on Debian 13 "trixie". It has its own desktop, **NEXUS HORIZON**: a
shell written in Qt 6/QML that runs as a Wayland session on the KWin
compositor (it is not KDE Plasma), a set of system applications built on one
UI library, an installer, a security baseline and on-demand security tool
profiles ("NEXUS Arsenal").

| | |
|---|---|
| Version | 0.1.0 "obsidian", channel `dev` (`distro/base/nexus.conf`) |
| Base | Debian 13 "trixie"; exactly one Debian suite is enabled |
| Kernel in the image | Debian's signed `linux-image-amd64` (6.12 series) |
| Architecture | amd64. arm64 is prepared in the build system but has never been built or booted — it is not supported |
| Desktop | NEXUS HORIZON (Wayland; KWin compositor) |
| Licence | GPL-3.0-or-later (`LICENSE`); third-party components keep their own licences |

> **This is a development version.** Everything that has been verified was
> verified in QEMU with software emulation. **Nothing has been tested on a
> physical machine.** Read [docs/limitations.md](docs/limitations.md) and
> [docs/testing.md](docs/testing.md) before you rely on anything.

## Status

"Verified" below means exactly what [docs/testing.md](docs/testing.md)
records (source: the project's test record, `build/work/briefs/FACTS.md` on
the build machine, and the component READMEs that state how they were
tested). "Implemented" means the code is in the repository and has not been
verified end to end.

| Component | State | Verified how | Not verified |
|---|---|---|---|
| Build system (chroot, packages, repository, ISO) | implemented, used to build both images | minimal and desktop ISO built on an Ubuntu 24.04 amd64 host | any other build host; arm64 |
| Minimal (console) live ISO | verified | QEMU: UEFI, UEFI + Secure Boot (enforcing), legacy BIOS, each from CD and USB — login, sudo, overlay root, clean power-off | real hardware |
| Installer engine (unattended) | verified for five scenarios | QEMU: erase disk (UEFI, UEFI + Secure Boot, BIOS), erase + LUKS2 (UEFI), alongside in free space (UEFI); each followed by booting the installed disk | manual-partition and separate-/home scenarios and the refusal cases have a re-run pending; real hardware |
| Installer planner | verified | 28 unit tests | — |
| Graphical installer | implemented | UI and planner on synthetic hardware in the build chroot (`system/installer/gui/README.md`) | a real installation driven by the GUI |
| Desktop live ISO | partly verified | QEMU, UEFI: SDDM auto-login, KWin + shell running, panel/dock/wallpaper/launcher drawn, System Information started from the launcher | Secure Boot and BIOS boot of the desktop ISO, lock screen, login screen without auto-login |
| NEXUS HORIZON shell | partly verified | nested KWin session: launcher, search, dock, context menu, session menu, global shortcuts | real GPU, multiple screens, HiDPI |
| Shell modules (notifications, tray, status) | implemented | each module's README lists its tests (nested session, fixtures) | the booted image; real hardware services |
| Applications (Monitor, Store, Updater, Network Center, Control Center, Arsenal manager) | implemented | each README lists its tests (build chroot; real daemons on a private bus where possible, otherwise fixtures) | polkit prompts, real hardware, most of them inside the booted image |
| Security baseline (`nexus-hardening`) | implemented | `nexus-firewall` functional test in an isolated network namespace | every other setting as a working whole on an installed system; real hardware |
| NEXUS kernel | configuration only | `kernel/build.sh --configure-only` | a complete build; the image does not contain it |
| Hosted package repository | does not exist | — | — |
| arm64 | not built, not booted | package names resolve; `Architecture: all` packages are published for arm64 | everything else |

## Quick start: from a clone to an ISO

### Build host

The images were built on Ubuntu 24.04 (amd64, 4 vCPU, 15 GB RAM). A Debian
or Ubuntu amd64 host is assumed; nothing else has been tried. You need root
(the scripts create chroots and device nodes), free disk space under `build/`
for two chroots, the image work tree and the outputs (not measured; the
desktop ISO alone is about 1 GB), and network access to `deb.debian.org`.

Commands the scripts call on the host: `mmdebstrap`, `mksquashfs`, `xorriso`,
`mcopy`/`mformat`/`mmd`, `gpg`, `rsync`, `git`, `cpio`, `dpkg`, `unshare`,
`sha256sum`, and Debian's archive keyring. On Debian/Ubuntu:

```sh
sudo apt install mmdebstrap squashfs-tools xorriso mtools gnupg rsync git cpio \
                 debian-archive-keyring
```

### Build

Run from the top of the clone. Each script prints its usage with `--help`.

```sh
# 1. Debian trixie build chroot with the build dependencies.
#    --dev-tools is currently required for a package build as well: see
#    docs/packaging/building.md.
sudo build/chroot/create.sh --dev-tools

# 2. Build every NEXUS package inside the chroot.
sudo build/packages/build.sh

# 3. Publish the packages into a signed local APT repository.
sudo build/packages/repo.sh

# 4. Build the image.
sudo build/iso/build.sh                      # desktop image
sudo build/iso/build.sh --variant minimal    # console image with the installer engine
```

Artefacts:

| Path | Content |
|---|---|
| `build/out/packages/*.deb` | NEXUS packages |
| `build/out/repo/` | signed APT repository (components `core`, `security`, `community`) |
| `build/out/keys/nexus-archive-keyring.gpg` | public half of the repository signing key |
| `build/out/iso/nexus-os-0.1.0-amd64.iso` (+ `.sha256`, `.manifest`) | desktop image |
| `build/out/iso/nexus-os-0.1.0-amd64-minimal.iso` (+ `.sha256`, `.manifest`) | minimal image |
| `build/work/` | chroots, caches, the signing key's GnuPG home (`build/work/signing`), test output |

`build/work/` and `build/out/` are ignored by git. The signing key generated
on first use is a **development key**, not a release key
([docs/packaging/repositories.md](docs/packaging/repositories.md)).

Details: [docs/packaging/building.md](docs/packaging/building.md),
[docs/packaging/iso.md](docs/packaging/iso.md).

## Write the ISO to a USB stick

The image is a hybrid ISO: written raw to a USB stick it boots through UEFI
(with or without Secure Boot) and through legacy BIOS.

1. Check the image:

   ```sh
   cd build/out/iso && sha256sum -c nexus-os-0.1.0-amd64.iso.sha256
   ```

2. Find the device name of the stick. Plug it in, then:

   ```sh
   lsblk -o NAME,SIZE,MODEL,TRAN,MOUNTPOINTS
   ```

   Identify the stick by size and model (`TRAN` is `usb`). You need the whole
   device (`/dev/sdX`), not a partition (`/dev/sdX1`).

3. Write it.

   > **Warning: `dd` overwrites the target device without asking.** Everything
   > on it is lost. If you name the wrong device — for example your system
   > disk — that disk is destroyed. Check the name against `lsblk` again
   > immediately before you press Enter, and unmount any mounted partitions
   > of the stick first.

   ```sh
   sudo dd if=nexus-os-0.1.0-amd64.iso of=/dev/sdX bs=4M conv=fsync status=progress
   ```

4. Boot the computer from the stick (firmware boot menu, usually F12, F10,
   F9 or Esc). The boot menu offers "Live session", "Live session (safe
   graphics)", "Install NEXUS OS" and an "Advanced options" submenu
   ([docs/architecture/boot.md](docs/architecture/boot.md)).

Booting from USB was verified in QEMU only (the image attached as a USB
storage device).

## Before you install on a real machine

No installation on physical hardware has been performed yet. Yours would be
the first test of that kind. Treat it accordingly:

* **Back up your data first**, to a disk that is not connected during the
  installation. "Erase disk" destroys everything on the selected disk; the
  other modes leave existing partitions alone by design, but a power failure
  or a hardware fault during partitioning can still damage a partition table.
* **Try the live session first.** Boot "Live session" and check that the
  display, keyboard, touchpad, Wi-Fi, sound and suspend behave on your
  hardware before installing. If the screen stays black, use "Live session
  (safe graphics)" ([docs/troubleshooting/README.md](docs/troubleshooting/README.md)).
* **Secure Boot.** The image boots through Debian's Microsoft-signed shim,
  Debian-signed GRUB and Debian's signed kernel, so Secure Boot can stay
  enabled and no key has to be enrolled. This was verified in QEMU with OVMF
  and the Microsoft keys; firmware of real machines differs. If the firmware
  refuses the medium, see the troubleshooting page before disabling
  Secure Boot.
* **Firmware settings.** Prefer UEFI mode over legacy/CSM mode. Windows
  "fast startup" and BitLocker must be dealt with in Windows before you
  shrink or share a disk with it. Use a wired or known-good power supply:
  the installer warns on battery power.
* **Proprietary drivers are not included.** NVIDIA cards use nouveau/Mesa
  ([docs/installation/hardware.md](docs/installation/hardware.md)).
* Start with a spare machine or a spare disk if you can.

Installation guide: [docs/installation/installing.md](docs/installation/installing.md).

## Repository layout

| Directory | Content |
|---|---|
| `distro/` | distribution identity (`base/nexus.conf`), package lists, image variants, image hooks, repository component map |
| `build/` | build system: `lib/common.sh`, `chroot/`, `packages/`, `iso/` (`build/work`, `build/out` are generated) |
| `kernel/` | NEXUS kernel pipeline: `build.sh`, configuration fragments, `required.list`, patch series |
| `security/` | hardening files, firewall, audit rules, security status report, polkit policy (package `nexus-hardening`) |
| `system/` | installer engine and graphical installer, boot-loader management (`nexus-boot`), live-session configuration, live-media boot menu |
| `desktop/` | `horizon-shell` (shell, session, modules), `ui` (NexusUI library and style), `themes` (colour schemes, icon themes, login and lock screen) |
| `applications/` | NEXUS applications, one directory each |
| `arsenal/` | security tool profiles (manifests), their tools and docs, the Arsenal manager |
| `assets/` | wallpapers, logos, boot themes and the generators that produce them (`assets/src`) |
| `tests/` | QEMU boot, installation and desktop tests; GUI smoke test; test harness (`tests/lib`) |
| `scripts/` | developer loop (`dev.sh`) and generators (`gen-identity.sh`, `gen-apt-source.sh`, `gen-tokens.py`) |
| `debian/` | the single source package `nexus-os` that builds every NEXUS binary package |
| `cmake/`, `CMakeLists.txt` | CMake project of all compiled and data components |
| `docs/` | this documentation |

## Documentation

| Topic | Document |
|---|---|
| Architecture | [overview](docs/architecture/overview.md), [boot chain](docs/architecture/boot.md) |
| Kernel | [docs/kernel/README.md](docs/kernel/README.md) |
| Building | [packages and chroots](docs/packaging/building.md), [repositories](docs/packaging/repositories.md), [ISO images](docs/packaging/iso.md) |
| Installing | [installation guide](docs/installation/installing.md), [unattended installation](docs/installation/unattended.md), [hardware](docs/installation/hardware.md) |
| Desktop | [using NEXUS HORIZON](docs/desktop/horizon.md), [desktop architecture](docs/desktop/architecture.md), [NexusUI](docs/desktop/nexusui.md), [shell modules](docs/desktop/shell-modules.md) |
| Applications | [application reference](docs/applications/README.md), [developing applications](docs/applications/development.md) |
| Security | [hardening](docs/security/hardening.md), [security toolkit](docs/security/toolkit.md), [updates](docs/security/updates.md) |
| Troubleshooting | [common problems](docs/troubleshooting/README.md), [recovery](docs/troubleshooting/recovery.md) |
| Quality | [testing](docs/testing.md), [limitations](docs/limitations.md) |
| Project | [CONTRIBUTING.md](CONTRIBUTING.md), [SECURITY.md](SECURITY.md) |

## Licence

NEXUS OS is free software, licensed under the GNU General Public License,
version 3 or (at your option) any later version — `SPDX-License-Identifier:
GPL-3.0-or-later`. See [LICENSE](LICENSE). Debian packages and other
third-party software in the image keep their own licences; their copyright
files are kept in the image under `/usr/share/doc/*/copyright`.

The security tools that NEXUS Arsenal can install are for authorised testing,
research, defence and education only: see
[arsenal/docs/legal-use.md](arsenal/docs/legal-use.md).
