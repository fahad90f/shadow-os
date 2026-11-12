# Testing

What is tested, how to run each test, what each one verifies, the results
on record, and what only real hardware can verify.

## The short version

* Everything was built and tested on one cloud virtual machine: Ubuntu
  24.04, x86_64, 4 vCPU, 15 GB RAM, **no KVM**.
* Every boot test ran in QEMU with pure software emulation (TCG): OVMF
  UEFI, OVMF with Secure Boot enforcing and the Microsoft keys enrolled, and
  SeaBIOS.
* **Nothing has been tested on a physical machine.** Wi-Fi, GPUs, laptops,
  suspend, Bluetooth, audio output and touchpads are untested.
* **arm64:** package sets resolve and `Architecture: all` packages are
  published for arm64, but no arm64 image has been built or booted.

A pass under emulation says the software logic works. It says little about
timing and nothing about real firmware or devices.

## Results on record

Source: the project's test record (`build/work/briefs/FACTS.md` on the build
machine). "Pending" means the test exists but has no valid result yet.

### Live boot — minimal ISO

| Firmware | Medium | Result |
|---|---|---|
| UEFI | CD | PASS |
| UEFI | USB | PASS |
| UEFI + Secure Boot (enforcing) | CD | PASS |
| UEFI + Secure Boot (enforcing) | USB | PASS |
| Legacy BIOS | CD | PASS |
| Legacy BIOS | USB | PASS |

Checked in each: login, sudo, overlay root, clean power-off.

### Installer engine — unattended, on the minimal ISO

Each scenario installs, then boots the installed disk and checks identity,
locale, user, sudo, locked root, fstab by UUID, swap, boot entry,
boot-loader re-installation and that the logs are free of secrets.

| Scenario | Firmware | Result |
|---|---|---|
| erase disk | UEFI | PASS |
| erase disk + LUKS2 encryption | UEFI | PASS |
| erase disk | UEFI + Secure Boot (enforcing) | PASS |
| erase disk | Legacy BIOS | PASS |
| alongside an existing system, in free space | UEFI | PASS |
| manual partition assignment keeping `/home` | UEFI | **pending re-run.** The installation was correct; the test's last check had a bug (it expected an English message on a German installation), which is fixed |
| manual | Legacy BIOS | **pending re-run** |
| erase disk with separate `/home` | UEFI | **pending re-run** |
| refusal cases | UEFI | **pending re-run** |

Planner unit tests: **28 pass.**

### Desktop ISO (first build, 1.03 GB)

| Check | Result |
|---|---|
| Boots via UEFI to SDDM auto-login into the "Shadow Horizon" Wayland session | PASS |
| KWin and `horizon-shell` run; panel, dock, wallpaper and launcher are drawn | PASS |
| System Information started from the launcher by keyboard; shows Shadow OS 0.1.0 on kernel 6.12 | PASS |
| Graphical installer run | pending |
| Lock screen | pending |
| Login screen without auto-login | pending |
| Secure Boot boot of the desktop ISO | pending |
| Legacy BIOS boot of the desktop ISO | pending |

Found and fixed during these runs:

* software OpenGL (llvmpipe) crashed the compositor under emulation → the
  session now uses CPU rendering when no GPU render node exists;
* a duplicate shell after a compositor restart → a single supervisor;
* audit rules failed to load when a watched path was missing → fixed;
* the Meta-key shortcut was lost → repaired at start.

### Other

| Component | Result |
|---|---|
| `shadow-firewall` | functional test in an isolated network namespace passes |
| Shell in a nested KWin session (not a VM) | launcher, search, dock, context menu, session menu and global shortcuts verified with injected input and screenshots |
| SHADOW kernel | `kernel/build.sh --configure-only` verified; a full build was started and **not completed** |
| Applications and shell modules | each README states its own tests; most back-ends that need NetworkManager, BlueZ, UPower, logind or PackageKit were tested against the real daemon on a private bus where possible, otherwise against fixtures, and say which |

Per-component details: [applications/README.md](applications/README.md) and
the READMEs it links.

## Test suites

All paths are relative to the repository. The QEMU tests use only the
Python standard library (`tests/lib/qemu.py`), except that the desktop test
needs Pillow.

Host packages for the QEMU tests on Debian/Ubuntu: `qemu-system-x86`,
`qemu-utils` (`qemu-img`), `ovmf` (including the `…_4M.ms.fd` Secure Boot
images), `python3`, `python3-pil`, and optionally `imagemagick` (converts
screenshots to PNG). KVM is used automatically when `/dev/kvm` is usable;
otherwise TCG. Each test prints which one it used.

The tests write to `build/work/tests/` and `build/work/shots/`. If those
directories belong to root (they do after a build with `sudo`), run the
tests with `sudo` or change the owner.

### 1. Live boot: `tests/boot/`

```sh
tests/boot/test_live_boot.py --iso build/out/iso/shadow-os-0.1.0-amd64-minimal.iso \
    [--arch amd64|arm64] [--firmware uefi|uefi-secureboot|bios] [--media cdrom|usb] \
    [--user shadow] [--password live] [--memory 4096] [--timeout 900] [--workdir DIR]

tests/boot/run-matrix.sh <iso> [amd64|arm64] [results-dir]
```

`run-matrix.sh` runs the six firmware × medium combinations for amd64 and
prints a PASS/FAIL table (`summary.txt` in the results directory, default
`build/work/tests/boot-matrix-amd64`). `SHADOW_TEST_TIMEOUT` sets the
per-step timeout.

Verified from inside the guest, over the serial console:

* firmware → shim → GRUB → kernel → live-boot → systemd reaches a login
  prompt;
* the live user can log in and has passwordless sudo;
* the root filesystem is the overlay from the medium and the kernel command
  line has `boot=live`;
* the system booted through the expected firmware type;
* the Secure Boot state matches the firmware mode under test, and under
  Secure Boot the kernel lockdown is active;
* systemd reports `running` with no failed units;
* the guest powers off cleanly.

The test boots the serial-console menu entry, selected automatically
through an SMBIOS serial number that only the harness sets
([architecture/boot.md](architecture/boot.md)).

### 2. Planner unit tests: `tests/installation/test_plan.py`

```sh
python3 tests/installation/test_plan.py
```

Runs anywhere, without root and without disks; it also runs whenever the
architecture-independent packages are built (`debian/rules`). The planner is a pure function of an answer file and a probe
report, and it decides what is erased. The tests pin its rules: answer
validation, whole-disk layouts (UEFI and BIOS, encryption, separate
`/home`, swap variants, small disks), free-space installation, manual
assignment, and the refusal of unsafe requests.

### 3. Installation end to end: `tests/installation/`

```sh
tests/installation/test_install.py --iso <iso> --workdir DIR \
    [--arch amd64|arm64] [--firmware uefi|uefi-secureboot|bios] [--scenario NAME] \
    [--memory 4096] [--timeout 900] [--install-timeout 3600] [--verify-only]

tests/installation/run-all.sh <iso> [arch] [results-dir] [scenario:firmware ...]
```

| Scenario | What it does |
|---|---|
| `erase` | whole-disk installation, no encryption |
| `erase-luks` | whole-disk with LUKS2 + LVM; the passphrase is entered at boot |
| `erase-home` | whole-disk with a separate `/home` and a swap partition |
| `alongside` | installs into free space next to an existing "other system"; its partition, files and EFI loader must be bit-identical afterwards (UEFI only) |
| `manual` | manual assignment of existing partitions; `/home` is kept |
| `refuse` | the engine must refuse unsafe requests: the installation medium as target, a wrong plan hash, a non-existent disk, encryption without a passphrase, a reserved user name, a mounted target disk |

Default matrix of `run-all.sh` for amd64: `erase:uefi erase-luks:uefi
erase:uefi-secureboot erase:bios alongside:uefi manual:uefi manual:bios
erase-home:uefi refuse:uefi`. Results go to
`build/work/tests/install-matrix-amd64/` (`summary.txt`, one log per
scenario; disk images of passing runs are deleted).

What "verified" means here: the engine's own JSON events report success;
the virtual machine then boots **from the installed disk alone** through
the same firmware; and these checks run inside the installed system:

* booted from disk, not the live medium; `os-release` identifies Shadow OS;
* host name, locale, time zone and keyboard as answered (a German
  configuration is used on purpose); the locale was generated;
* the user exists with its groups and `sudo` works with its password; the
  root account is locked;
* the live-session packages are gone;
* the root filesystem is ext4 and mounted from `fstab` by UUID;
* encrypted: root is on LUKS2 + LVM, and LUKS reports version 2 with
  Argon2id;
* swap is a file, a partition or absent, as answered;
* UEFI: a firmware boot entry "Shadow OS" exists and the Secure Boot state
  is as configured; BIOS: booted through legacy BIOS;
* `/etc/shadow-os/boot.conf` exists and `shadow-grub-install --verbose` succeeds
  (the update path);
* the installation log is on the installed system and contains neither the
  password nor the passphrase;
* systemd reports `running`.

A second, unrelated disk is attached in every scenario and must stay
untouched.

### 4. Desktop boot: `tests/desktop/test_desktop_boot.py`

```sh
tests/desktop/test_desktop_boot.py --iso build/out/iso/shadow-os-0.1.0-amd64.iso \
    [--firmware uefi|uefi-secureboot|bios] [--workdir DIR] [--shots DIR] \
    [--memory 4096] [--timeout 900] [--session-timeout 1200] [--settle 45] [--keys]
```

Verified from the serial console while the session runs on the emulated
display:

* the display manager is active and logged the live user into a Wayland
  session without a password prompt;
* `kwin_wayland` and `horizon-shell` are running and the shell did not
  restart;
* `org.kde.KWin` and `org.kde.kglobalaccel` are on the session bus;
* no failed systemd units, system or user;
* a screenshot is not (almost) a single colour, i.e. something was drawn.

`--keys` also opens the launcher with the Meta key. Screenshots
(`desktop-1-first.png`, `desktop-2-settled.png`, `desktop-3-launcher.png`)
are saved for visual review in `build/work/shots/`.

### 5. Interactive VM: `tests/lib/vmshell.py`

Booting under emulation takes minutes, so debugging uses a VM that stays up
between commands:

```sh
tests/lib/vmshell.py start --iso build/out/iso/shadow-os-0.1.0-amd64.iso [--disk FILE] \
    [--firmware uefi] [--memory 4096] [--cpu MODEL] [--no-login] [--foreground]
tests/lib/vmshell.py run 'systemctl --failed'      # as the live user, over the serial console
tests/lib/vmshell.py sudo 'journalctl -b -p err'
tests/lib/vmshell.py shot name.png                 # → build/work/shots/name.png
tests/lib/vmshell.py key meta_l
tests/lib/vmshell.py type 'text'
tests/lib/vmshell.py mouse X Y [left|right|middle]
tests/lib/vmshell.py tail
tests/lib/vmshell.py stop
```

State lives in `build/work/tests/vmshell/`. The interactive desktop checks
on record were done this way.

### 6. GUI smoke test: `tests/desktop/smoke.sh`

```sh
sudo tests/desktop/smoke.sh [--dir BUILD_DIR_NAME] [--shots] [component...]
```

Launches every SHADOW application headlessly inside the build chroot and
fails if it cannot load its UI or the QML engine reports any warning or
error. Applications opt in with a `smoke-tests.list` next to their sources
(one `<binary> [args...]` per line). `--dir` names a build tree made with
`scripts/dev.sh build`; a component is the directory name that holds the
list. `--shots` saves a screenshot of each window.

This test runs without system services. It proves that the UI loads and
that its "unavailable" states are sound — not that the back-ends work.

### 7. Component tests

Kept next to the component and described in its README: for example
`applications/monitor/tests/`, `applications/network-center/tests/`,
`applications/store/pkcommon/tools/overlay-tests/`,
`desktop/horizon-shell/modules/*/tests/`, `desktop/themes/tests/`,
`system/installer/gui/testing/`. They run in the build chroot through
`scripts/dev.sh` ([packaging/building.md](packaging/building.md)).

Static checks without a chroot:

```sh
python3 arsenal/tools/validate.py
python3 arsenal/tools/gen-control.py --check
python3 arsenal/tools/gen-profiles-doc.py --check
python3 scripts/gen-tokens.py check
python3 desktop/themes/tools/generate.py --check
```

### Continuous integration

`.github/workflows/build.yml` runs on pushes to `main`, version tags, pull
requests and manual dispatch on an `ubuntu-24.04` runner: host set-up,
static checks (`gen-tokens.py check`, `test_plan.py`, `validate.py`), build
chroot, packages, repository, the desktop ISO, checksum verification, the
desktop boot test under UEFI, and the live-boot test of the desktop ISO
under UEFI Secure Boot and BIOS. It uploads the ISO as a workflow artifact
and, for `v*` tags, creates a release. A release signing key can be
supplied through the secret `SHADOW_SIGNING_KEY`; otherwise a development key
is generated during the run.

**No run of this workflow is on record**, so none of its steps counts as
verified. As written, it creates the build chroot without `--dev-tools`,
while `debian/control` needs packages from that list
([packaging/building.md](packaging/building.md)); the package build step is
therefore expected to fail until the two lists are synchronised.

### Not present

Source comments mention `tests/integration/` (checking each application's `--dump` output
against the system's own tools inside a booted VM) and `tests/packages/`;
neither directory exists. Application back-ends have therefore **not** been
verified inside the booted image.

## Reproducing the results

```sh
# build (see README)
sudo build/chroot/create.sh --dev-tools
sudo build/packages/build.sh
sudo build/packages/repo.sh
sudo build/iso/build.sh --variant minimal
sudo build/iso/build.sh

# test
python3 tests/installation/test_plan.py
sudo tests/boot/run-matrix.sh build/out/iso/shadow-os-0.1.0-amd64-minimal.iso
sudo tests/installation/run-all.sh build/out/iso/shadow-os-0.1.0-amd64-minimal.iso
sudo tests/desktop/test_desktop_boot.py --iso build/out/iso/shadow-os-0.1.0-amd64.iso --keys
```

Expect long run times without KVM: every boot is emulated instruction by
instruction, and an installation scenario copies and boots a whole system.
The timeouts in the scripts are sized for that. With KVM the same tests
are much faster.

Images are not bit-for-bit reproducible and contain whatever package
versions Debian's mirror offered on the build day
([packaging/iso.md](packaging/iso.md)); your results apply to your image.

## What only real hardware can verify

None of the following can be established in QEMU, and none of it has been:

| Area | Open questions |
|---|---|
| Firmware | real UEFI implementations and their quirks: boot-entry handling, removable-path fallback, firmware that ignores or rewrites entries; Secure Boot with vendors' key sets (including machines that disable the Microsoft third-party CA by default); real legacy BIOS/CSM |
| Graphics | Intel, AMD and NVIDIA (nouveau) GPUs: KWin on real DRM/KMS, GPU compositing, blur and "glass" surfaces, performance, multi-GPU laptops, the `nomodeset` path |
| Displays | multiple monitors, hot-plug, HiDPI and fractional scaling, rotation, VRR/HDR, laptop panel backlight |
| Input | touchpads (gestures, tap-to-click), touchscreens, special keys (volume, brightness, media, microphone mute), keyboard layouts on real keyboards |
| Networking | Wi-Fi: firmware loading, scanning, WPA2/WPA3, hidden and enterprise networks, MAC randomisation against real access points; Ethernet link changes; captive portals; VPNs |
| Bluetooth | adapters, pairing, audio devices, rfkill |
| Audio | real sound cards, SOF firmware, headphone jack detection, microphones, HDMI audio |
| Power | suspend and resume, hibernation, lid switch, battery reporting, power profiles, lock-on-resume |
| Storage | NVMe and SATA on real controllers, USB sticks from different vendors, SD cards, real disk sizes and sector sizes (4K native), TRIM on LUKS |
| Dual boot | Windows (fast startup, BitLocker), other distributions' boot loaders, shared ESPs |
| Peripherals | printers and scanners (not supported), webcams, fingerprint readers, smart cards, docks |
| Timing | boot time, installation time, real-time behaviour of audio and animations — emulation distorts all of these |
| Security tools | monitor mode and injection with real Wi-Fi chipsets, SDR hardware, hardware debuggers |
| Firmware updates | `fwupd` against real devices |
| arm64 | everything |

If you test on hardware, please report what you tried, the exact machine
(`shadow-sysinfo --dump report`), and what happened — including successes
([../CONTRIBUTING.md](../CONTRIBUTING.md)).
