# Architecture overview

This page describes how NEXUS OS is put together and why. It describes the
repository as it is; statements about what has been tested are in
[../testing.md](../testing.md).

## Layers

```
┌──────────────────────────────────────────────────────────────────────┐
│ Applications      NEXUS applications (applications/*, arsenal/manager,│
│                   system/installer/gui) and any Debian application    │
├──────────────────────────────────────────────────────────────────────┤
│ Shell             horizon-shell: wallpaper, panel, dock, launcher,    │
│                   session menu, polkit agent + modules (tray,         │
│                   notifications, status)                              │
├──────────────────────────────────────────────────────────────────────┤
│ Compositor        KWin (Wayland) + Xwayland; KScreenLocker; SDDM      │
├──────────────────────────────────────────────────────────────────────┤
│ System services   systemd, logind, D-Bus, polkit, NetworkManager,     │
│                   BlueZ, UPower, power-profiles-daemon, UDisks2,      │
│                   PackageKit, PipeWire/WirePlumber, nftables, auditd, │
│                   AppArmor — all from Debian                          │
├──────────────────────────────────────────────────────────────────────┤
│ Kernel            Debian's signed linux-image (6.12). A NEXUS kernel  │
│                   configuration exists; it is not shipped             │
├──────────────────────────────────────────────────────────────────────┤
│ Base              Debian 13 "trixie": main, contrib, non-free-firmware│
│                   + trixie-updates + trixie-security                  │
└──────────────────────────────────────────────────────────────────────┘
```

| Layer | Source in this repository | What NEXUS adds |
|---|---|---|
| Base | `distro/base/package-lists/*.list`, `distro/base/hooks/` | package selection, APT sources (one Debian suite, HTTPS), identity files (`nexus-base-files` diverts `os-release`, `issue`, `issue.net`) |
| Boot | `system/boot/`, `assets/boot/` | `nexus-boot` (boot-loader maintenance), live-media GRUB menu, GRUB theme, Plymouth theme |
| Kernel | `kernel/` | configuration fragments on top of Debian's source and configuration; not in the image ([../kernel/README.md](../kernel/README.md)) |
| System services | `security/`, `system/live/` | `nexus-hardening` (sysctl, modprobe, kernel command line, nftables ruleset, audit rules, journald, sudo, NetworkManager defaults), live-session configuration. No service of its own is added: NEXUS ships no daemon |
| Compositor | `desktop/horizon-shell/session/`, `desktop/themes/` | KWin defaults (`/etc/xdg/kwinrc`), screen-locker defaults, SDDM theme and configuration, lock-screen QML |
| Shell | `desktop/horizon-shell/`, `desktop/ui/` | the whole shell and the NexusUI library |
| Applications | `applications/`, `arsenal/manager/`, `system/installer/` | NEXUS applications |

NEXUS OS adds no system daemon. Everything that needs privileges goes
through an existing Debian service with its own polkit policy, or through a
small helper started with `pkexec` (see below).

## Session and process architecture

```
systemd
 └─ sddm                               display manager (theme "nexus"; greeter on Wayland,
     │                                 composited by kwin_wayland)
     └─ /usr/bin/nexus-horizon-session        session script: environment, rendering mode
         └─ kwin_wayland_wrapper --xwayland   compositor and window manager
             │   --exit-with-session=/usr/libexec/nexus/horizon-session-leader
             ├─ Xwayland                      for X11 applications
             ├─ kscreenlocker_greet           when the screen is locked
             └─ horizon-session-leader        supervisor
                 ├─ systemctl --user start nexus-horizon-session.target
                 │    (binds graphical-session.target, pulls in XDG autostart)
                 └─ /usr/bin/horizon-shell    one process: panel, dock, launcher, wallpaper,
                      │                       session menu, polkit agent, modules
                      └─ applications         started detached by the shell
```

1. SDDM shows the login screen and starts the session named
   `nexus-horizon` (`/usr/share/wayland-sessions/nexus-horizon.desktop`). In
   the live session `system/live/2000-nexus-session` configures auto-login.
2. `nexus-horizon-session` exports the session environment
   (`XDG_CURRENT_DESKTOP=NEXUS:KDE`, Wayland-first toolkit settings), decides
   between GPU and CPU rendering, applies the saved appearance to third-party
   toolkits and replaces itself with KWin.
3. KWin starts `horizon-session-leader` once it accepts clients. When the
   leader exits, KWin exits and the session ends.
4. The leader publishes the environment to the systemd user manager and
   D-Bus activation, rebuilds the application database (`kbuildsycoca6`),
   starts `nexus-horizon-session.target` and runs `horizon-shell`.
   * Exit status 0 of the shell means "log out": the leader stops the target
     and exits.
   * Any other status is a crash: the shell is restarted, at most five times
     within a minute; after that the session is ended.
   * Only one leader runs per session. If KWin restarts after a crash and
     starts a second copy, that copy waits for the first one instead of
     starting a second shell.

Details: [../desktop/architecture.md](../desktop/architecture.md).

## How NEXUS components talk to the system

The rule is in [../applications/development.md](../applications/development.md),
section 4. In order of preference:

1. **An existing system service over D-Bus.** The service's own polkit policy
   decides; the session's polkit agent (part of `horizon-shell`) asks the
   user. The application holds no privilege.
2. **A helper through `pkexec`**, when no service covers the task. One polkit
   action per helper, bound to the helper's path; the helper accepts a closed
   set of sub-commands, validates every argument and is called with an
   argument vector, never through a shell. Secrets travel on standard input.

| Component | Talks to | Mechanism |
|---|---|---|
| Shell: power actions | `org.freedesktop.login1` | D-Bus with interactive authorisation |
| Shell: lock | `org.freedesktop.ScreenSaver` (KWin/KScreenLocker) | D-Bus, session bus |
| Shell: windows, workspaces | KWin | Wayland protocol `org_kde_plasma_window_management`; D-Bus `org.kde.KWin.VirtualDesktopManager` |
| Shell: global shortcuts | `org.kde.kglobalaccel` | KGlobalAccel |
| Shell: authentication | polkit | registers as the session's authentication agent (PolkitQt6) |
| Status module | NetworkManager, BlueZ, UPower, power-profiles-daemon, logind, PipeWire (PulseAudio protocol), MPRIS players, `/dev/rfkill` | D-Bus and library clients, all asynchronous |
| Store, Updater, Arsenal manager | PackageKit | D-Bus (PackageKit-Qt); PackageKit's polkit actions |
| Monitor | `/proc`, `/sys`; systemd `org.freedesktop.systemd1` | files; D-Bus; `pkexec /usr/bin/kill` and `pkexec /usr/bin/renice` for other users' processes |
| Network Center | NetworkManager; `ip`, `ss`, `ping`, … | D-Bus; argument-vector commands; helper `/usr/libexec/nexus/network-helper` via `pkexec` (actions `org.nexus.networkcenter.sockets`, `.capture`) |
| Control Center | NetworkManager, BlueZ, UPower, logind, KWin, PipeWire | `nmcli`, D-Bus, `kscreen-doctor`, KConfig |
| Graphical installer | installer engine | `pkexec /usr/sbin/nexus-installer-engine` (action `org.nexus.installer.run`) |
| Security status report | kernel, nftables, AppArmor, audit, APT | `pkexec /usr/sbin/nexus-security-status` (action `org.nexus.security.read-status`, read-only) |
| Firewall management | nftables | `pkexec /usr/sbin/nexus-firewall` (action `org.nexus.firewall.manage`) |

Applications also expose the data behind their UI as JSON with
`<binary> --dump <topic>`, without opening a window; this is the interface
tests use.

## Packaging model

* **One source package.** `debian/` describes the source package `nexus-os`
  (format `3.0 (native)`). It builds every NEXUS binary package.
* **CMake install component = Debian package.** The top-level
  `CMakeLists.txt` discovers components (`desktop/*`, `applications/*`,
  `system/installer`, `system/recovery`, `arsenal/manager`); each installs
  into a CMake install component named after its package (`nexus-<directory>`
  unless mapped otherwise, e.g. `horizon-shell` → `nexus-horizon`).
  `debian/rules` runs `cmake --install --component <package>` once per
  package and fails if a component installs nothing.
* **Data packages** (`nexus-artwork`, `nexus-themes`) come from the same
  CMake project configured with `-DNEXUS_DATA_ONLY=ON`, which needs no
  compiler. The remaining `Architecture: all` packages (`nexus-base-files`,
  `nexus-boot`, `nexus-installer`, `nexus-live`, `nexus-archive-keyring`,
  `nexus-hardening`, `nexus-arsenal-data`) are assembled by `debian/*.install`
  files; the `nexus-arsenal-*` profile packages are metapackages defined in
  `debian/control` only.
* **Identity from one file.** `distro/base/nexus.conf` is the single source
  for name, version, codename, channel and Debian suite. The build fails if
  `debian/changelog` and `nexus.conf` disagree on the version.
* Packages are built in a pristine Debian chroot of the target architecture,
  published into a signed APT repository with reprepro, and installed into
  the image from that repository with APT — the same way a user's system
  would install them. See [../packaging/building.md](../packaging/building.md).

## Design decisions

### Debian Stable as the base, one suite only

Hardware support, security updates and the signed boot chain come from
Debian. The image enables `trixie`, `trixie-updates` and `trixie-security`
and nothing else; no packages are mixed in from testing, unstable or another
distribution (`distro/base/hooks/80-apt-sources.sh`). Arsenal profiles list
only packages from trixie `main`.

### Debian's signed boot chain and kernel

Using Debian's Microsoft-signed shim, Debian-signed GRUB and Debian's signed
kernel lets the system boot under Secure Boot with no key enrolment and no
signing infrastructure of the project's own. The cost is that the boot-loader
directory on the ESP must be called `debian`
([boot.md](boot.md)) and that a NEXUS-built kernel would not boot under
Secure Boot unless the project signs it with a key users enrol.

### KWin as the compositor

A Wayland compositor is the part of a desktop that owns the display and
input devices, isolates clients from each other, and has to handle every GPU
driver, multi-monitor layout, HiDPI scaling, input device, Xwayland and
screen locking correctly. KWin does this today and is maintained and
security-supported in Debian. NEXUS HORIZON therefore uses KWin and builds
only the layers above it: shell, session, settings and applications. The
shell uses public interfaces — the layer-shell protocol, KWin's window
management protocol, D-Bus, KGlobalAccel — and a small number of KDE
Frameworks libraries. It does not run `plasmashell`, and it is a project
rule that no code or artwork is copied from KDE or other desktops (their
libraries and public protocols are used).

What this costs: dependencies on KWin-specific interfaces
(`org_kde_plasma_window_management`, `org.kde.KWin.*` D-Bus objects,
KScreenLocker's package format, `kglobalaccel`), on `plasma-integration` for
theming third-party Qt applications, and on KDE's portal back-end.

### The path to more independence

In rough order of effort:

1. Keep every KWin-specific call behind the shell's own classes
   (`Workspaces`, `WindowModel`, `ShellEffects`, `Shortcuts`, `Session`), so a
   different compositor means new back-ends, not a new shell. This is how
   the code is organised today.
2. Prefer cross-compositor Wayland protocols where they exist
   (`wlr-layer-shell` is already used; `ext-foreign-toplevel-list`,
   `ext-workspace` and `ext-session-lock` could replace the KWin-specific
   window list, workspace and lock interfaces as compositors implement them).
3. A lock screen and greeter that do not depend on KScreenLocker's package
   format.
4. A native compositor.

**A native compositor is future work and is not implemented.** Nothing in
the repository starts it. An honest assessment of building one on Smithay
(Rust):

* Smithay provides the building blocks (protocol implementations, DRM/KMS
  and libinput back-ends, renderer abstractions), not a compositor. Window
  management policy, effects, output configuration, Xwayland integration,
  screen locking, screen capture portals, input-method support, fractional
  scaling, VRR/HDR, direct scan-out and multi-GPU handling have to be written
  or integrated and then kept working across GPU drivers.
* The compositor is the most security-critical and the most
  hardware-dependent process of the desktop. It can only be validated on a
  wide range of real hardware, which this project has had none of so far.
* It would be a second language and toolchain (Rust) next to C++/QML.
* A realistic first step would be a minimal compositor that implements
  exactly the protocols the shell needs and runs the existing shell
  unchanged in a virtual machine — useful as a proof of the abstraction in
  step 1, far from a replacement for KWin on real hardware.

Until that work exists and has been tested on real machines, KWin stays.

### One shell process, modules compiled in

`horizon-shell` is a single process. Optional features (tray, notifications,
status/quick settings) are modules in `desktop/horizon-shell/modules/<name>`
that the core does not know: removing a directory removes the feature
([../desktop/shell-modules.md](../desktop/shell-modules.md)). One process
keeps start-up and memory use low; the cost is that a crash in a module
takes the shell down (the session leader restarts it).

### The installer is an engine with front-ends

The engine (`system/installer/engine`) is a non-interactive root program
with three verbs — `probe`, `plan`, `install` — and `install` only runs when
given the SHA-256 of the plan that was shown. The graphical installer and
unattended installations are both front-ends to it, so what is tested
unattended in QEMU is the code path the GUI uses
([../installation/unattended.md](../installation/unattended.md)).

## Security architecture summary

| Area | Mechanism | Document |
|---|---|---|
| Boot chain | shim (Microsoft-signed) → GRUB (Debian-signed) → kernel (Debian-signed); kernel lockdown under Secure Boot | [boot.md](boot.md) |
| Boot-loader updates | dpkg trigger re-installs the signed binaries to the ESP | [boot.md](boot.md), [../security/updates.md](../security/updates.md) |
| Disk encryption | optional LUKS2 (AES-XTS 512-bit, Argon2id) with LVM inside; `/boot` and the ESP stay unencrypted | [../security/hardening.md](../security/hardening.md) |
| Kernel hardening | sysctl settings, kernel command-line parameters, module auto-load blacklist | [../security/hardening.md](../security/hardening.md) |
| Network | nftables: inbound default-deny, forward default-deny except local VMs/containers, outbound allowed; NetworkManager MAC and IPv6 privacy defaults | [../security/hardening.md](../security/hardening.md) |
| Mandatory access control | AppArmor as shipped by Debian (`nexus-hardening` depends on `apparmor`; no NEXUS profiles are shipped) | [../security/hardening.md](../security/hardening.md) |
| Auditing | auditd with a small rule set (identity, privilege, modules, time, boot chain, APT sources); persistent, size-bounded journal | [../security/hardening.md](../security/hardening.md) |
| Privilege | root account locked; `sudo` for the first user; polkit for graphical actions; no GUI runs as root; helpers take argument vectors | [../applications/development.md](../applications/development.md) |
| Packages | Debian archive and Debian security over HTTPS, authenticated by Debian's keyring; NEXUS packages authenticated by the NEXUS archive key | [../packaging/repositories.md](../packaging/repositories.md) |
| Security tools | not installed by default; profiles are metapackages over Debian `main` packages | [../security/toolkit.md](../security/toolkit.md) |
