# Building NEXUS packages

How the NEXUS binary packages are built, and the developer loop for working
on one component. Image building is in [iso.md](iso.md); publishing in
[repositories.md](repositories.md).

Everything on this page was used to produce the packages and images on the
build machine (Ubuntu 24.04, amd64). No other host has been tried.

## Build host

`sudo scripts/host-setup.sh [--no-test-tools]` installs what a Debian or
Ubuntu host needs: the tools that create chroots and assemble images, and
(unless `--no-test-tools`) QEMU, OVMF and the Python imaging library for the
tests. The package list is in the script.

## Principles

* Packages are compiled **inside a pristine Debian trixie chroot** of the
  target architecture, never against the build host's libraries.
* The chroot is entered through a private mount namespace
  (`in_chroot` in `build/lib/common.sh`): `/proc`, `/sys`, `/dev` and bind
  mounts vanish when the command exits, so there is no clean-up step that
  can be forgotten and no mount left behind for a later `rm -rf` to follow.
* All build scripts must run as **root** (`need_root`).
* Build-time APT mirrors are plain HTTP on purpose (APT authenticates
  content with the signed Release files; a pristine chroot does not have a
  TLS-intercepting proxy's CA). The shipped system uses HTTPS.

## Configuration

`build/lib/common.sh` is sourced by every build script. It reads
`distro/base/nexus.conf` and accepts these environment variables:

| Variable | Default | Meaning |
|---|---|---|
| `NEXUS_WORK` | `build/work` | chroots, caches, image staging, signing home |
| `NEXUS_OUT` | `build/out` | packages, repository, ISO images, kernel configuration |
| `NEXUS_JOBS` | `nproc` | parallel jobs |
| `NEXUS_BUILD_MIRROR` | `http://deb.debian.org/debian` | Debian mirror used while building |
| `NEXUS_BUILD_SECURITY_MIRROR` | `http://deb.debian.org/debian-security` | security mirror used while building |
| `NEXUS_REPO_URL` | empty | URL of a published NEXUS repository ([repositories.md](repositories.md)) |
| `NEXUS_SIGNING_HOME` | `$NEXUS_WORK/signing` | GnuPG home of the repository signing key |
| `SOURCE_DATE_EPOCH` | time of the last git commit | timestamp for reproducible output |

`sudo` does not pass your environment on by default. Use
`sudo env NEXUS_JOBS=2 build/packages/build.sh`, or `sudo -E`.

## 1. The build chroot

```
sudo build/chroot/create.sh [--arch amd64|arm64] [--recreate] [--dev-tools]
```

| Option | Effect |
|---|---|
| `--arch` | target architecture; default: the host's. A foreign architecture needs `qemu-user-static` registered with binfmt_misc and `arch-test` (untested) |
| `--recreate` | delete the existing chroot first |
| `--dev-tools` | additionally install `build/chroot/dev-tools.list` |

It bootstraps Debian trixie with `mmdebstrap` (variant `buildd`) into
`build/work/chroot-build-<arch>`, authenticated against the Debian archive
keyring found on the host, then installs `build/chroot/build-deps.list`. An
existing chroot is reused and only updated; an interrupted bootstrap is
detected (no stamp file) and redone.

`dev-tools.list` holds the run-time tools and services the applications
talk to and what the nested-session tests need (KWin, xdotool,
NetworkManager, PackageKit, PipeWire, …). No daemon is started in the chroot
(`policy-rc.d` refuses).

> **`--dev-tools` is currently needed for a package build too.**
> `debian/control` lists seven development packages in `Build-Depends-Arch`
> (`libkf6networkmanagerqt-dev`, `libkf6dbusaddons-dev`,
> `libkf6iconthemes-dev`, `libkf6notifications-dev`, `libkf6i18n-dev`,
> `libkf6crash-dev`, `libkf6statusnotifieritem-dev`) that are in
> `dev-tools.list`, not in `build-deps.list`. Without them
> `dpkg-buildpackage` refuses to build the compiled packages. The two lists
> are meant to be kept in sync; they are not at present.

## 2. Building the packages

```
sudo build/packages/build.sh [--arch amd64|arm64] [--indep | --arch-only]
```

| Mode | `dpkg-buildpackage` | Builds |
|---|---|---|
| (default) | `-b` | all packages for the architecture plus the architecture-independent ones |
| `--indep` | `-A` | only `Architecture: all` packages; no compiler run, seconds |
| `--arch-only` | `-B` | only compiled packages |

What it does:

1. Checks that the version in `debian/changelog` equals `NEXUS_VERSION` in
   `distro/base/nexus.conf`.
2. Exports the source into the chroot at `/build/pkg/nexus-os-<version>`:
   **tracked files with their working-tree content.** Untracked files are
   left out so unfinished work cannot break a build; set
   `NEXUS_EXPORT_UNTRACKED=1` to include untracked, non-ignored files.
   Paths listed in `build/packages/wip.exclude` (work in progress that is
   committed but not yet integrated) are left out as well.
3. Makes sure a repository signing key exists
   (`build/packages/signing-key.sh`) and hands its public half to the build;
   it ends up in `nexus-archive-keyring`.
4. Runs `dpkg-buildpackage -us -uc --no-sign` in the chroot with
   `DEB_BUILD_OPTIONS=parallel=$NEXUS_JOBS` and `NEXUS_REPO_URL`.
5. Copies the resulting `.deb` files to `build/out/packages/`.

### What `debian/rules` does

* `dh` with the `cmake+ninja` build system; `DEB_BUILD_MAINT_OPTIONS =
  hardening=+all`; build type `RelWithDebInfo`.
* **Compiled packages:** one `cmake --install --component <package>` per
  package returned by `dh_listpackages -a`. A component that installs no
  file is an error. The `nexus-arsenal-*` metapackages are skipped (they are
  empty by design).
* **Architecture-independent packages:**
  * `scripts/gen-identity.sh debian/generated` renders `os-release`,
    `issue`, `issue.net` and `nexus-release` from `nexus.conf`;
  * `scripts/gen-apt-source.sh debian/generated` prepares the archive keyring
    and `nexus.sources`;
  * the installer engine is byte-compiled as a syntax check and the planner
    unit tests run (`python3 tests/installation/test_plan.py`) — a failing
    planner test fails the package build;
  * `nexus-artwork` and `nexus-themes` are installed from the CMake project
    configured with `-DNEXUS_DATA_ONLY=ON` (no compiler needed);
  * the rest is assembled by the `debian/*.install` files.
* GUI smoke tests do not run during the package build; they are run with
  `tests/desktop/smoke.sh`.

### Packages

From `debian/control` (source package `nexus-os`):

| Package | Arch | Content |
|---|---|---|
| `nexus-base-files` | all | `os-release`, `issue`, `issue.net` (diverting Debian's), `/usr/share/nexus/nexus-release` |
| `nexus-archive-keyring` | all | repository public key and `nexus.sources` |
| `nexus-boot` | all | `nexus-grub-install`, `/etc/default/grub`, `10-nexus.cfg`, dpkg trigger |
| `nexus-installer` | all | installer engine |
| `nexus-installer-gui` | any | graphical installer |
| `nexus-live` | all | live-session configuration |
| `nexus-hardening` | all | security baseline |
| `nexus-artwork` | all | wallpapers, logos, GRUB and Plymouth themes |
| `nexus-themes` | all | colour schemes, icon themes, SDDM theme, lock screen, `nexus-apply-appearance` |
| `nexus-ui` | any | NexusUI library, style and icons |
| `nexus-horizon` | any | shell and session |
| `nexus-sysinfo`, `nexus-monitor`, `nexus-files`, `nexus-store`, `nexus-updater`, `nexus-control-center`, `nexus-network-center` | any | applications |
| `nexus-arsenal` | any | Arsenal manager |
| `nexus-arsenal-data` | all | profile manifests |
| `nexus-arsenal-core`, `-web`, `-network`, `-redteam`, `-blueteam`, `-forensics`, `-developer`, `-complete` | all | profile metapackages |
| `nexus-arsenal-reversing` | any | profile metapackage (one tool is amd64-only) |

Components that exist in the tree but have no package yet are listed in
`build/packages/wip.exclude`; see [../limitations.md](../limitations.md).

## 3. The developer loop (`scripts/dev.sh`)

For day-to-day work you do not build packages. `scripts/dev.sh` configures
and builds inside the same chroot, with the repository mounted read-only at
`/src` and build trees under `/build/<NAME>` in the chroot.

```
scripts/dev.sh build  [--dir NAME] [--only "a;b"] [--modules "a;b"] [--cc-pages "a;b"] [ninja targets...]
scripts/dev.sh run    [--dir NAME] [--shot FILE.png] [--size WxH] [--smoke] [--delay MS]
                      [--light] [--accent NAME] <binary> [args...]
scripts/dev.sh nested [--dir NAME] [--shot FILE.png] [--size WxH] [--wait SECONDS]
                      [--light] [--accent NAME] -- '<commands>'
scripts/dev.sh shell  [--dir NAME]
scripts/dev.sh exec   [--dir NAME] -- <command> [args...]
```

It needs root and the build chroot (`NEXUS_DEV_ARCH` selects the chroot's
architecture). `nested`, and applications whose back-ends call system tools,
need the chroot to have been created with `--dev-tools`.

| Verb | What it does |
|---|---|
| `build` | Configures once (CMake, Ninja, `RelWithDebInfo`) and builds. `--dir` selects the build tree (default `dev`); use one per component so parallel work does not collide. `--only` restricts the components (directory names, e.g. `monitor`, `horizon-shell`); `--modules` restricts the shell modules compiled into `horizon-shell` (`-` for none); `--cc-pages` restricts Control Center pages |
| `run` | Starts a built GUI binary under a virtual X server (Xvfb) with software OpenGL, a throw-away `HOME` and a private session bus. `--shot` saves a screenshot of the main window to `build/work/shots/`; `--smoke` makes the run fail if the QML engine reported a warning or error; `--light`/`--accent` set the appearance. `NEXUS_RUN_TIMEOUT` (default 60 s) limits the run |
| `nested` | A real NEXUS HORIZON session for tests: `kwin_wayland` runs inside Xvfb and `<commands>` run in it as session clients. Inside the commands, `xdo …` sends input with xdotool and `shot NAME.png` takes a screenshot. Logs and files written to `$NEXUS_LOGS` end up in `build/work/shots/nested-logs/<dir>/` |
| `shell` | Interactive shell in the chroot |
| `exec` | One command in the chroot, e.g. `scripts/dev.sh exec -- apt-cache policy nmap` |

Typical cycle for one application:

```sh
sudo env NEXUS_JOBS=2 scripts/dev.sh build --dir monitor --only monitor
sudo scripts/dev.sh run --dir monitor --smoke --shot monitor.png nexus-monitor
sudo scripts/dev.sh run --dir monitor nexus-monitor --dump cpu
sudo tests/desktop/smoke.sh --dir monitor monitor
```

Limits of the chroot: no systemd, no system D-Bus services, no polkit
authority, no hardware. Applications must start there and show accurate
"unavailable" states; behaviour against real services has to be tested in a
booted image. The contract is in
[../applications/development.md](../applications/development.md).

## 4. Generators

| Script | Purpose |
|---|---|
| `scripts/gen-identity.sh <dir>` | identity files from `distro/base/nexus.conf` |
| `scripts/gen-apt-source.sh <dir>` | archive keyring and `nexus.sources` (`NEXUS_ARCHIVE_KEY`, `NEXUS_REPO_URL`) |
| `scripts/gen-tokens.py qml\|render\|docs\|check` | everything derived from the design tokens `desktop/themes/tokens/nexus-tokens.json` |
| `desktop/themes/tools/generate.py [--check\|--list]` | generated theme files (committed) |
| `assets/src/icons/gen-symbolic.py`, `gen-app-icons.py` | symbolic icon set (`desktop/ui/icons`), application and category icons |
| `assets/src/logos/gen-logos.py`, `assets/src/wallpapers/gen-wallpapers.py` | logo family, wallpapers |
| `assets/src/boot/gen-boot-theme.sh <dir>` | binary parts of the GRUB and Plymouth themes |
| `arsenal/tools/gen-control.py`, `gen-profiles-doc.py` | `arsenal/packaging/control.in`, `arsenal/docs/profiles.md` |

Generated artwork and theme files are committed; the generators are run by
hand when their inputs change.

## Building the kernel

Separate pipeline and chroot: [../kernel/README.md](../kernel/README.md).
