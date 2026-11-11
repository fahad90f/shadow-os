# Known limitations

What does not work, is not finished, or has not been verified. This list is
meant to be complete and blunt; if you find something missing, it is a
documentation bug.

Shadow OS 0.1.0 is a development version. Do not use it where data loss or
downtime matters.

## Verification

* **Not tested on physical hardware.** Every boot and installation ran in
  QEMU with software emulation. The first boot on a real machine is the
  first such test. Wi-Fi, GPUs, laptops, suspend, Bluetooth, audio output
  and touchpads are untested.
* **arm64 is not supported.** Configuration for it exists (package lists,
  kernel fragment, boot paths, test matrix), package names resolve and
  `Architecture: all` packages are published for arm64, but no arm64 image
  has been built or booted. `SHADOW_ARCHES` in `distro/base/shadow.conf` lists
  it; that is an intention, not a fact.
* The desktop image has been booted under UEFI only. Its Secure Boot and
  BIOS boots, the graphical installer run, the lock screen and the login
  screen without auto-login are pending.
* Installer scenarios with a pending re-run: manual partition assignment
  (UEFI and BIOS), separate `/home`, and the refusal cases.
* Application back-ends were tested in a build chroot against real daemons
  on a private bus or against fixtures — not in the booted image. polkit
  authentication prompts have not been observed for any application.
* A CI workflow exists (`.github/workflows/build.yml`), but no run of it is
  on record, and as written its package build is expected to stop on missing
  build dependencies (next section). There is no integration test suite
  inside a booted image.
* Images are not verified to be reproducible.

## Distribution and updates

* **No hosted SHADOW package repository exists.** `shadow.sources` is shipped
  disabled. SHADOW packages on an installed system receive **no updates**,
  including security fixes for SHADOW components.
* The SHADOW packages of a release ship as a signed repository on the disk
  (`/usr/share/shadow/repository`, desktop image). It is a snapshot.
* **The archive signing key used so far is a development key** generated on
  the build machine, without a passphrase. There is no release key and no
  release-signing procedure. Treat every existing image as a development
  build.
* Release channels (`dev`, `testing`, `stable`) are a label only.
* No automatic updates and no background update check.
* No rollback: no snapshots, no transactional updates, one version per
  package in the repository.
* No upgrade path to a later Shadow OS or Debian release is defined.
* Images contain whatever Debian package versions the mirror offered at
  build time; the base-layer cache does not notice Debian updates unless
  you build with `--fresh`.
* `build/chroot/build-deps.list` and `debian/control` are out of sync: a
  package build needs the chroot created with `--dev-tools`. The CI workflow
  creates it without.

## Kernel

* The image ships **Debian's signed kernel**, not a SHADOW kernel.
* The SHADOW kernel's configuration pipeline works; a **full build has never
  completed**. No SHADOW kernel package exists, none has been booted, and
  nothing would install one.
* The build-time hardening options of the SHADOW configuration that Debian
  does not already set (1000 Hz and full preemption, `UBSAN_BOUNDS`,
  `RANDOM_KMALLOC_CACHES`, `ZERO_CALL_USED_REGS`, strict IOMMU invalidation,
  always-on BPF JIT) are therefore **not in effect** on a Shadow OS system.
* A SHADOW kernel would be unsigned unless a Secure Boot key is supplied;
  the project has no such key.
* The kernel build keeps the temporary module-signing key in its build
  tree; it is "discarded" only when that tree is deleted.

## Boot

* The boot-loader directory on the ESP is `EFI/debian` (required by
  Debian's signed GRUB). Shadow OS cannot share an EFI System Partition with
  a Debian installation or another distribution that uses that directory.
* The boot-loader refresh trigger has not been exercised by a real GRUB or
  shim update. A failed refresh is reported only by
  `shadow-security-status`; the Updater does not show it.
* The GRUB menu has no password.
* GRUB's recovery-mode entries are kept, but the root account is locked, so
  the rescue shell is expected to be unusable unless you set a root
  password.
* The live medium's "Recovery environment" entry starts a program
  (`shadow-recovery`) that does not exist in the image.
* The Plymouth passphrase prompt on an encrypted installation of the
  desktop image is unverified.

## Installer

* The graphical installer has not performed a real installation.
* No partition editor: manual mode assigns existing partitions only.
* No resizing of existing partitions: "alongside" needs unallocated space
  on a GPT disk.
* No encryption in manual mode. No TPM unlocking, key files or recovery
  keys. `/boot` and the ESP are never encrypted.
* Only ext4 for new filesystems. No btrfs, no RAID, no LVM without
  encryption, no ZFS.
* Partition and swap sizes cannot be set in the graphical installer.
* **Tool profiles selected in the installer are installed without their
  tools**: the engine installs the metapackages with
  `--no-install-recommends`, and the tools are Recommends.
* The installer's own texts are English only; so are the shell and the
  applications (strings are marked for translation, no translations ship).
* The installed system is a copy of the live image: hardware-specific
  package selection does not happen.
* Dual boot with Windows is untested.
* The installer's README says locales that glibc lists without the
  `.UTF-8` suffix (`hi_IN`, `vi_VN`, …) cannot be selected; the engine has
  since gained handling for them, and the combination is untested.

## Desktop

* The compositor is KWin. Shadow Horizon is a shell and session on top of
  it, with dependencies on KWin-specific interfaces, KScreenLocker,
  `plasma-integration` and KDE's portal back-end. A native compositor is
  not implemented.
* **Blur and glass effects need GPU compositing.** With CPU rendering —
  any machine without a GPU render node, including basic virtual machines —
  surfaces are opaque, and animations cost noticeable CPU time.
* **XEmbed (legacy X11) tray icons are not supported.**
* Panel, dock, notifications, quick settings and the tray appear on the
  primary screen only. Multiple screens and HiDPI are untested.
* The lock screen and the SDDM greeter depend on `layer-shell-qt`; a real
  lock on Wayland and an interactive login are unverified.
* Window-management keyboard shortcuts are KWin's defaults and cannot be
  edited in the Control Center (they are listed read-only).
* Idle time-outs written by the Control Center (`power.conf`) are read by
  nothing. Screen locking follows KScreenLocker's own settings.
* Notifications: no sounds, no inline reply, history lost on shell restart.
* Quick settings: Bluetooth, airplane mode, brightness changes and Night
  Light are untested; NetworkManager, UPower and power profiles were tested
  against fixtures only.
* GTK and libadwaita theming is untested.
* No on-screen keyboard; no screen reader has been tried (accessibility
  roles are set in the UI).
* A crash in a shell module takes the whole shell down (it is restarted, up
  to five times a minute).

## Applications

Not in the image, although the desktop refers to them:

| Missing | Effect |
|---|---|
| Terminal (`shadow-terminal`) — has a README, excluded from package builds | **the desktop image contains no terminal emulator.** The terminal shortcuts and the Arsenal manager's "show usage in terminal" report "not installed". Use a text console (Ctrl+Alt+F3) or install a terminal from Debian |
| Screenshot (`shadow-screenshot`) | the Print shortcuts report "not installed" |
| Editor, Archive Manager, Disk Manager | not available; default dock pins for missing applications are not shown |
| Security Center | the status report exists only as the command `shadow-security-status` |
| Recovery tool | see "Boot" |
| Control Center pages: Appearance, Accessibility, Startup, Language, Date & Time, Storage, Users, Privacy, Firewall, Security, Updates | not available. In particular there is no graphical way to manage users, date and time, the firewall, or the appearance beyond the quick-settings dark-mode switch |
| Shell power module | not integrated |

In the image, with limits stated in their READMEs
([applications/README.md](applications/README.md)); the most important:

* **Arsenal manager:** no PackageKit transaction has ever been run through
  it.
* **Files:** mounting real drives through UDisks2, and SFTP, SMB, NFS and
  WebDAV locations, are untested (SFTP/SMB/NFS also need `kio-extras`, which
  is only recommended and not in the image); "Open terminal here" has no
  terminal to open. Whether Files was part of the desktop image that was
  boot-tested is not recorded.
* **Store:** DEB packages only (no Flatpak or Snap); repositories cannot be
  added; screenshot download unverified.
* **Updater:** periodic check not scheduled; no offline updates.
* **Network Center / Control Center:** Wi-Fi scan and join never performed
  against hardware; no enterprise Wi-Fi set-up; OpenVPN and WireGuard
  activation untested.
* **Monitor:** sensors, batteries, GPUs and services tested against
  synthetic data only; no GPU utilisation for Intel, nouveau or virtual
  GPUs.

## Hardware and drivers

* **Proprietary GPU drivers (NVIDIA) are not included**; nouveau and Mesa
  only. Debian's `non-free` component is not enabled.
* No out-of-tree kernel modules; DKMS drivers are your responsibility and,
  under Secure Boot, need your own signing key.
* No printing or scanning support, no virtual-machine guest tools, no
  fingerprint support out of the box.
* Hibernation is untested and, under Secure Boot, disabled by the kernel.
* See [installation/hardware.md](installation/hardware.md).

## Security

* **Tools not in Debian main** (Metasploit, Burp Suite, Ghidra, …) are
  listed as external with links, **not installed**.
* The "security distribution" consists of on-demand profiles of Debian
  packages. No security tool profile is installed by default, whatever the
  variant file's comment says.
* SHADOW ships no AppArmor profiles; SHADOW applications and helpers run
  unconfined.
* The firewall filters inbound traffic by port only; no outbound filtering
  and no per-application rules. There is no graphical firewall management
  in the image.
* Audit rules are not locked.
* The lock screen has a 5-second grace period by default.
* No vulnerability in a SHADOW component can currently be fixed on installed
  systems through an update channel.
* The complete list of deliberate exceptions is in
  [security/hardening.md](security/hardening.md).

## Documentation

* Source comments refer to documents that do not exist under those names:
  `docs/architecture/build-environment.md`, `docs/architecture/image-build.md`,
  `docs/kernel/configuration.md`, `docs/kernel/patches.md`,
  `docs/security/secure-boot.md`, `docs/packaging/licensing.md`,
  `distro/release/channels.md`, `distro/signing/README.md`. The content is
  in [packaging/building.md](packaging/building.md),
  [packaging/iso.md](packaging/iso.md), [kernel/README.md](kernel/README.md),
  [architecture/boot.md](architecture/boot.md) and
  [packaging/repositories.md](packaging/repositories.md), as far as it
  exists.
* `build/iso/build.sh` tells you to run `make packages repo`; there is no
  Makefile. Use `build/packages/build.sh` and `build/packages/repo.sh`.
* Troubleshooting and recovery procedures are derived from the design and
  have not been carried out on broken systems.
