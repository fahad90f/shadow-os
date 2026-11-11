# Building ISO images

`build/iso/build.sh` produces a hybrid live/install ISO. Prerequisite: the
NEXUS packages are built and published into `build/out/repo`
([building.md](building.md), [repositories.md](repositories.md)).

```
sudo build/iso/build.sh [--arch amd64|arm64] [--variant NAME] [--fresh]
                        [--comp zstd|xz] [--no-nexus-packages]
```

| Option | Effect |
|---|---|
| `--arch` | target architecture (default: host). Only amd64 has been built |
| `--variant` | `desktop` (default) or `minimal`: a file `distro/base/variants/<NAME>.conf` |
| `--fresh` | ignore the cached Debian base layer and bootstrap again |
| `--comp` | squashfs compressor: `zstd` (default; level 19, 1 MiB blocks) or `xz` (smaller, slower to boot) |
| `--no-nexus-packages` | do not install NEXUS packages from the local repository (bring-up and debugging of the base image) |

Environment: the variables of `build/lib/common.sh`, plus `NEXUS_KEEP_DOCS=1`
to keep `/usr/share/doc`, `info` and `lintian` files in the image.

Output in `build/out/iso/`:

| File | Content |
|---|---|
| `nexus-os-<version>-<arch>.iso` | the desktop image; other variants are named `…-<arch>-<variant>.iso` |
| `….iso.sha256` | SHA-256 checksum, `sha256sum -c` format |
| `….manifest` | every package in the image: name, version, architecture |

The first desktop image built was 1.03 GB.

## Variants

| Variant | Package lists | NEXUS packages | Purpose |
|---|---|---|---|
| `desktop` | `10-core 15-firmware 20-boot 30-live 40-desktop` | `nexus-base-files nexus-archive-keyring nexus-boot nexus-installer nexus-live nexus-hardening nexus-artwork nexus-themes nexus-ui nexus-horizon nexus-sysinfo nexus-monitor nexus-installer-gui nexus-store nexus-updater nexus-control-center nexus-network-center nexus-arsenal nexus-arsenal-data nexus-files` | the release image |
| `minimal` | `10-core 20-boot 30-live` | `nexus-base-files nexus-archive-keyring nexus-boot nexus-installer nexus-live` | console-only image with the installer engine, for build-system bring-up and fast automated boot and installer tests. Not a release artefact |

Note that `minimal` contains neither `nexus-hardening` nor the firmware
list.

A variant file is shell syntax and defines `VARIANT_LABEL`, `VARIANT_LISTS`
and `VARIANT_NEXUS_PACKAGES`. To add a variant, add a file; no script needs
to change.

## Package lists

`distro/base/package-lists/<name>.list`: one Debian package per line, `#`
starts a comment, and a trailing `[arch …]` restricts a line to those
architectures.

| List | Content |
|---|---|
| `10-core` | systemd, D-Bus, Debian's signed kernel, initramfs-tools, sudo, polkit, locales and console, base utilities, hardware inspection, storage and filesystem tools (ext4, FAT, btrfs, XFS, NTFS, exFAT, LUKS, LVM), NetworkManager and network utilities |
| `15-firmware` | firmware and CPU microcode from Debian's `non-free-firmware` component ([../installation/hardware.md](../installation/hardware.md)) |
| `20-boot` | GRUB common files, `efibootmgr`, `os-prober`, `mokutil`, signed shim and GRUB for UEFI, `grub-pc-bin` for BIOS |
| `30-live` | live-boot, live-config, live-tools, user-setup |
| `40-desktop` | Plymouth, SDDM, portals, KWin and Xwayland, Mesa, PipeWire, BlueZ, UPower, power-profiles-daemon, UDisks2, AccountsService, PackageKit, AppStream, fwupd, fonts, Firefox ESR, tools the NEXUS applications call |

Packages are installed **without Recommends and Suggests** in both the base
layer and the NEXUS layer, so anything needed must be listed explicitly or
be a hard dependency.

## Pipeline

```
1. base layer    mmdebstrap of Debian trixie + the variant's package lists       cached
2. NEXUS layer   overlayfs on top of the base: NEXUS packages from the local
                 signed repository, rootfs overlay, hooks
3. squashfs      live/filesystem.squashfs
4. boot files    signed shim + GRUB (UEFI), GRUB El Torito image (BIOS, amd64)
5. ISO           hybrid ISO9660 via xorriso, checksum, manifest
```

Stages 2–4 run in `build/iso/stage-rootfs.sh` inside a private mount
namespace; stage 5 is `build/iso/assemble.sh`.

### 1. Base layer

`mmdebstrap --variant=minbase` with the expanded, sorted package set, into
`build/work/image-<arch>/base-<key>`. The key is a hash of the architecture,
suite, documentation setting, build mirrors and the package set, so the
layer is reused until one of them changes; a stale layer is deleted. Unless
`NEXUS_KEEP_DOCS=1`, dpkg is told to exclude `/usr/share/doc/*` except the
`copyright` files, `/usr/share/info/*` and `/usr/share/lintian/*`.

The cache does **not** notice new package versions in Debian. Use `--fresh`
to pick up Debian updates.

### 2. NEXUS layer

The base is the read-only lower directory of an overlayfs; all changes go to
an upper directory that is recreated on every build.

* `policy-rc.d` prevents any daemon from starting.
* The local repository is bind-mounted read-only at `/run/nexus-repo` and
  trusted through a temporary key; `apt-get install --no-install-recommends`
  installs the variant's NEXUS packages; the temporary source and key are
  removed.
* If the image contains `horizon-shell`, the repository is copied to
  `/usr/share/nexus/repository` and `nexus-local.sources` is written
  ([repositories.md](repositories.md)).
* `distro/base/rootfs/` is copied over the image as root-owned files
  (currently empty).
* The hooks run in order.

### Hooks

`distro/base/hooks/NN-name.sh` are run with `bash -eu` inside the image, in
numeric order, with the `NEXUS_*` identity variables, `NEXUS_ARCH` and
`NEXUS_VARIANT` in the environment. A failing hook fails the build.

| Hook | What it does |
|---|---|
| `20-locale.sh` | generates `en_US.UTF-8`, sets it as default, time zone UTC (the installer sets the user's choices on the target) |
| `50-live.sh` | locks the root password; removes `/etc/hostname` (the bootstrap copies the build host's name; live-config sets the live host name, the installer the final one) |
| `60-plymouth.sh` | selects the `nexus` Plymouth theme when Plymouth and the theme are installed |
| `80-apt-sources.sh` | writes the shipped APT configuration (`debian.sources`, HTTPS) and fails if Debian's archive keyring is missing |
| `85-initramfs.sh` | regenerates every initramfs (`update-initramfs -u -k all`) |

### 3. Clean-up and squashfs

Before compression the script removes APT lists and caches, logs, debconf
and dpkg backups, `/tmp` contents, shell history and SSH host keys; empties
`/etc/machine-id` (a unique id is generated at first boot); and aborts if
the build mirror URL is still referenced under `/etc/apt`.

The newest kernel and its initramfs are copied to `live/vmlinuz` and
`live/initrd.img`. `/etc/.updated` and `/var/.updated` are written so that
systemd does not repeat its "first boot after update" jobs on every boot of
the read-only live system.

`mksquashfs` then packs the merged tree, excluding `boot/initrd.img-*` and
the contents of `proc`, `sys`, `run`, `tmp` and `dev`. The uncompressed size
is recorded in `live/filesystem.size`; the installer uses it for its space
checks.

### 4 and 5. Boot files and ISO

See [../architecture/boot.md](../architecture/boot.md) for the layout and
the boot menu. The boot-loader binaries are taken from the image itself.
The volume id is `NEXUS_<VERSION>_<ARCH>` in upper case.

## Reproducibility

Measures that are in the scripts:

| Measure | Where |
|---|---|
| `SOURCE_DATE_EPOCH` defaults to the time of the last git commit and is passed into chroots | `build/lib/common.sh` |
| Files newer than `SOURCE_DATE_EPOCH` are set to it before the squashfs is made | `stage-rootfs.sh` |
| ISO modification date and the build stamp in `.disk/info` come from `SOURCE_DATE_EPOCH` | `assemble.sh` |
| FAT volume serial of the ESP image derived from `SOURCE_DATE_EPOCH` | `assemble.sh` |
| Package set sorted and de-duplicated; base layer keyed by a hash of its inputs | `build.sh` |
| Source exported from git-tracked files only | `build/packages/build.sh` |
| Kernel build with fixed `KBUILD_BUILD_USER`/`KBUILD_BUILD_HOST` | `kernel/build.sh` |
| Empty machine-id, no SSH host keys, no logs or caches in the image | `stage-rootfs.sh` |
| Wallpapers generated with a fixed random seed | `assets/src/wallpapers/gen-wallpapers.py` |

What is **not** done:

* **Bit-for-bit reproducibility has not been tested**; no two builds have
  been compared.
* Debian packages are taken from the live mirror at build time, not from a
  snapshot, so two builds on different days can contain different package
  versions. The manifest records what a given image contains.
* `mksquashfs` is not given a fixed timestamp or a sorted file order
  explicitly, and the initramfs is generated by Debian's tools inside the
  image.
* The development signing key differs per build machine, so signatures and
  the shipped keyring differ.

## Checking an image

```sh
cd build/out/iso && sha256sum -c nexus-os-0.1.0-amd64.iso.sha256
sudo tests/boot/run-matrix.sh build/out/iso/nexus-os-0.1.0-amd64-minimal.iso
```

More in [../testing.md](../testing.md).
