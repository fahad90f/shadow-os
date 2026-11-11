# NEXUS applications

One section per NEXUS application that exists in the repository, taken from
the application's own README. Each README is the authoritative, more
detailed description — including the exact list of what was and was not
tested — and is linked from its section.

Common to all of them (see [development.md](development.md)):

* built on NexusUI ([../desktop/nexusui.md](../desktop/nexusui.md)), except
  the Terminal, which uses Qt Widgets;
* never run as root; privileged work goes to a system service with its own
  polkit policy, or to a helper started through `pkexec` with an argument
  vector;
* `<binary> --dump <topic>` prints the data behind the UI as JSON without
  opening a window;
* show real data only, and an explicit "unavailable" state with the reason
  when a service is missing.

**Verification in general:** the applications were tested in the Debian
build chroot — against the real daemon on a private bus where that was
possible, otherwise against fixtures. polkit prompts have not been observed
for any application (the chroot runs as root, with no polkit authority).
In the booted desktop image only System Information has been started (from
the launcher, in QEMU). Nothing has run on real hardware.

## Overview

| Application | Binary | Package | In the desktop image |
|---|---|---|---|
| [System Information](#system-information) | `nexus-sysinfo` | `nexus-sysinfo` | yes |
| [Monitor / Task Manager](#monitor-and-task-manager) | `nexus-monitor` | `nexus-monitor` | yes |
| [Files](#files) | `nexus-files` | `nexus-files` | yes (listed in the desktop variant) |
| [Control Center](#control-center) | `nexus-control-center` | `nexus-control-center` | yes |
| [Network Center](#network-center) | `nexus-network-center` | `nexus-network-center` | yes |
| [Store](#store) | `nexus-store` | `nexus-store` | yes |
| [Updater](#updater) | `nexus-updater` | `nexus-updater` | yes |
| [Arsenal](#arsenal-manager) | `nexus-arsenal` | `nexus-arsenal` | yes |
| [Installer](#installer) | `nexus-installer` | `nexus-installer-gui` | yes (removed from the installed system) |
| [Terminal](#terminal) | `nexus-terminal` | none yet | **no** |

[In progress](#in-progress): Editor, Archive Manager, Disk Manager,
Screenshot, Security Center, Recovery, further Control Center pages.

---

## System Information

`applications/sysinfo/` — the smallest complete application and the
reference implementation for the others. It has no README; this section is
taken from its sources.

| | |
|---|---|
| Purpose | hardware and software summary: operating system and version, Debian base, kernel, architecture, host name, desktop and session type, firmware type and Secure Boot state, processor, memory and swap, graphics devices, machine vendor and model |
| Binary / package | `nexus-sysinfo` / `nexus-sysinfo` |
| Data sources | `/etc/os-release`, `uname(2)`, `/proc/cpuinfo`, `/proc/meminfo`, `/proc/uptime`, `/sys/class/dmi/id`, `/sys/firmware/efi` (including the `SecureBoot` and `SetupMode` variables), `/sys/class/drm`, `lspci -mm` when available |
| Diagnostics | `nexus-sysinfo --dump system`, `--dump report` |
| Privileges | none |
| Limitations | read-only. A value that cannot be determined is shown as "—" |
| Verified | started from the launcher in the desktop image under QEMU; showed NEXUS OS 0.1.0 on kernel 6.12 |

## Monitor and Task Manager

[`applications/monitor/README.md`](../../applications/monitor/README.md)

| | |
|---|---|
| Purpose | live processor, memory, graphics, disk and network usage with history; sensors and batteries; the process list with end, force kill, suspend, continue and change priority; systemd services with start, stop, restart, enable, disable |
| Binary / package | `nexus-monitor` / `nexus-monitor`. Two launchers: System Monitor, and Task Manager (`nexus-monitor --page processes`). Single instance |
| Pages | `overview`, `processes`, `services`, `disks`, `network`, `sensors` (`nexus-monitor --page <key>`) |
| Data sources | `/proc` and `/sys` (CPU, memory, disks, network, hwmon, thermal zones, batteries, DRM devices); `nvidia-smi` when present; systemd's D-Bus API for services |
| Diagnostics | `--dump cpu\|memory\|gpu\|disks\|network\|processes\|services\|sensors\|battery` |
| Privileges | no helper and no polkit action of its own. Signals are sent directly through a pidfd; for another user's process `pkexec /usr/bin/kill -s … <pid>`; lowering a nice value or another user's process `pkexec /usr/bin/renice -n … -p …` (polkit's stock `org.freedesktop.policykit.exec`, administrator authentication). Service actions go to systemd with interactive authorisation (`org.freedesktop.systemd1.manage-units`, `manage-unit-files`, `reload-daemon`). PID 1 and kernel threads are never signalled |
| Limitations | GPU utilisation only where the driver publishes it (`amdgpu` sysfs, `nvidia-smi`); not for Intel, nouveau or virtual GPUs. Histories are not recorded while the window is hidden. The service list is polled every 10 s. With software rendering the Overview page costs noticeable CPU time. With `pkexec kill`/`renice` a reused PID could be hit between confirmation and authentication |
| Verified (per README) | dumps cross-checked against `free`, `ps`, `df`, `lsblk`, `ip` in the build chroot; the Processes page driven with real key presses and clicks; sensors, batteries, GPUs and the Services page against a synthetic machine and a mock systemd only |
| Not verified | real hwmon/battery/GPU hardware, a real systemd, any successful polkit prompt |

## Files

[`applications/files/README.md`](../../applications/files/README.md)

| | |
|---|---|
| Purpose | file manager: local folders, the Trash, removable and internal drives and network locations, with tabs, a split view and a preview panel |
| Binary / package | `nexus-files` / `nexus-files`. Single instance: a second start activates the running window |
| Command line | `nexus-files [path-or-url...] [--select <file>]`, `--dump places\|devices\|list [path-or-url]\|protocols` |
| Functions | grid and details view, sorting, filter and recursive search by name, hidden files, thumbnails and previews, properties with editable permissions, copy/move/link/rename/duplicate with progress, conflict dialogs and undo, Trash with restore, compress (tar.gz, tar.xz, tar.zst, zip) and extract, "Connect to server…" (FTP, SFTP, SMB, WebDAV(S), NFS, FISH — as far as the KIO worker is installed), mounting and ejecting drives, drag and drop, "Open with…" |
| Data sources | KIO (`KCoreDirLister`, KIO jobs and `FileUndoManager`, `PreviewJob`); `bsdtar` for archives, run with an argument vector; Solid/UDisks2 for drives; `KProtocolInfo` for network protocols; `~/.config/nexus/files.conf` for settings; `~/.config/mimeapps.list` for default applications |
| Privileges | none; no helper and no polkit action. UDisks2 authorises mounting through its own polkit policy. Passwords for network locations are asked for by KIO's password service, not by the application, which never sees or stores them |
| Limitations | KIO's password prompt is a Qt Widgets window, not a NexusUI dialog; untrusted TLS certificates are refused; archives only on local folders, none encrypted; LUKS volumes are not unlocked here; no batch rename, no ownership or ACL editing, no file-content search. SFTP, SMB, NFS and FISH need `kio-extras`, which is only recommended and therefore not in the image; "Open terminal here" needs a terminal, which the image does not have yet |
| Verified (per README) | in the build chroot, driving the real window in a nested session: file operations with undo, every conflict choice, a 600 MiB copy with checksum comparison, cancelling, Trash, archives, views, properties and permission changes, error states as root and as an unprivileged user, drag and drop, launching applications, FTP against a test server including authentication; drives against Solid's fake hardware back-end |
| Not verified | mounting, unmounting and ejecting real block devices and hot-plug; polkit prompts for mounting; SFTP, SMB, NFS, FISH and WebDAV; drag and drop between applications in the Wayland session; the light theme |

## Control Center

[`applications/control-center/README-network-hardware.md`](../../applications/control-center/README-network-hardware.md)
covers the nine network and hardware pages. The directory is shared by
several authors, which is why that file is not called `README.md`.

| | |
|---|---|
| Purpose | system settings, one page per topic |
| Binary / package | `nexus-control-center` / `nexus-control-center`; `nexus-control-center <key>` opens a page |
| Pages documented | `wifi`, `network` (Wired & Proxy), `bluetooth`, `vpn`, `display`, `sound`, `power`, `keyboard`, `mouse`. An About page also exists |
| Data sources | NetworkManager (`nmcli -t` and D-Bus), BlueZ (KF6 BluezQt), `kscreen-doctor` and KWin (displays, Night Light, input devices), PulseAudio protocol via KF6 PulseAudioQt on PipeWire, UPower, power-profiles-daemon, logind, `/sys/class/backlight`, `/sys/class/rfkill`, XKB rules, KConfig files (`kxkbrc`, `kcminputrc`, `kwinrc`), `org.kde.kglobalaccel` |
| Diagnostics | `nexus-control-center --dump <key>` for each key above |
| Privileges | no helper or policy for these pages. NetworkManager, BlueZ, power-profiles-daemon and logind ask polkit themselves. Wi-Fi passwords travel inside the D-Bus message, never on a command line |
| Files written | `~/.config/environment.d/90-nexus-proxy.conf` (manual proxy), `~/.config/nexus/power.conf` (idle time-outs — nothing reads it yet), `~/.config/kxkbrc`, `~/.config/kcminputrc`, `~/.config/kwinrc` |
| Limitations | no enterprise (802.1X) or WEP Wi-Fi profiles, no hotspot; manual proxies exist only in the environment file and take effect after the next login; VPN profiles are imported (WireGuard, OpenVPN), not edited; lid and power-button behaviour is shown, not changed; desktop shortcuts are listed read-only; mouse settings only in a real session |
| Verified (per README) | Displays and Keyboard in a nested KWin session; Sound against real PipeWire with null sinks; Wired & Proxy and VPN against a real NetworkManager in a network namespace; Power against real upowerd and power-profiles-daemon. Bluetooth, Mouse & Touchpad and Wi-Fi against test doubles only |
| Not verified | Night Light, VRR/HDR, real hardware of any kind, a polkit prompt for a non-root user, the pages inside a full NEXUS session |

Further pages (Appearance, Firewall, Startup and others) are in the source
tree but excluded from package builds; see [In progress](#in-progress).

## Network Center

[`applications/network-center/README.md`](../../applications/network-center/README.md)

| | |
|---|---|
| Purpose | state of the network, configuration through NetworkManager, and diagnostic tools |
| Binary / package | `nexus-network-center` / `nexus-network-center` |
| Pages | `overview`, `wifi`, `interfaces`, `ip`, `dns`, `routes`, `connections`, `vpn`, `diagnostics`, `capture` (`nexus-network-center <page>`) |
| Data sources | `ip -j` (addresses, routes, neighbours), `/sys/class/net`, `/etc/resolv.conf` and systemd-resolved, `/sys/class/rfkill`, `ss` (sockets), NetworkManager's D-Bus API; diagnostics with `ping`, `traceroute`/`tracepath`, `dig`/`resolvectl`, and the application's own TCP/TLS connections; capture with `dumpcap` (members of group `wireshark`) or `tcpdump` through the helper |
| Privileges | one helper, `/usr/libexec/nexus/network-helper` (Python 3), through `pkexec`. Action `org.nexus.networkcenter.sockets` (`ss` as root; `auth_admin_keep` for an active session) and `org.nexus.networkcenter.capture` (`tcpdump`; `auth_admin` every time, active local session only). The helper validates interface and filter, never writes files (the capture is a stream; the unprivileged application writes `~/Captures`, files mode 0600), drops capabilities and sets `no_new_privs`, and ends when its standard input closes. NetworkManager changes are authorised by NetworkManager's own polkit policy |
| Limitations | Wi-Fi set-up for WPA/WPA2/WPA3 personal, OWE and open networks only; no permanent secret agent; static routes and MTU are not editable; VPN import for WireGuard and OpenVPN only; helper captures are classic pcap |
| Verified (per README) | kernel data against `ip` and `ss`; diagnostics on loopback including hostile input; 28 hostile helper invocations refused and a real capture on `lo` with dropped privileges; NetworkManager read and change operations against a real daemon (1.52.1) in a private namespace |
| Not verified | Wi-Fi against hardware; the secret prompt; OpenVPN; WireGuard activation; pkexec/polkit (action selection, prompts); NetworkManager as an unprivileged user; dumpcap and Wireshark hand-over; systemd-resolved |

Packet capture is for networks you are authorised to monitor.

## Store

[`applications/store/README.md`](../../applications/store/README.md)

| | |
|---|---|
| Purpose | browse, search, install, update and remove software |
| Binary / package | `nexus-store` / `nexus-store` |
| Data sources | AppStream catalogue (applications, categories, descriptions, screenshot URLs); PackageKit for package facts, dependencies, trust, search, the installed list, updates and repositories; `dpkg-query` as a read-only fallback |
| Command line | `nexus-store [<AppStream ID \| package>]`, `--search <text>`, `--page overview\|installed\|updates\|repos\|<category>`, `--dump status\|categories\|installed\|repos\|updates`, `--dump search <text>`, `--dump component <ID\|package>` |
| Privileges | none of its own. PackageKit authorises through polkit (`org.freedesktop.packagekit.*`). Every transaction is simulated first; removals are always confirmed with the complete list; unauthenticated packages are refused unless explicitly acknowledged, and then PackageKit's own policy still decides |
| Network | screenshots and icons are fetched over HTTPS only, with size and format limits |
| Limitations | DEB packages through PackageKit only — no Flatpak or Snap, no ratings; repositories can be enabled or disabled, not added; local `.deb` files are not handled |
| Verified (per README) | all dumps and the transaction paths (install, remove, cancel, repository enable/disable, untrusted package refusal) against a real `packagekitd` in a throw-away overlay of the build chroot, also through the GUI under Xvfb |
| Not verified | polkit dialogs (PackageKit was driven as root); screenshot download; licence, signing-key and media-change prompts |

`applications/store/pkcommon/` is the PackageKit code shared with the
Updater.

## Updater

[`applications/updater/README.md`](../../applications/updater/README.md) —
user-facing description in [../security/updates.md](../security/updates.md).

| | |
|---|---|
| Purpose | check for, review and install updates; history; restart notices; repair of an interrupted package operation |
| Binary / package | `nexus-updater` / `nexus-updater` |
| Pages | `updates`, `history`, `log`, `recovery`, `sources`, `settings` |
| Data sources | PackageKit (`RefreshCache`, `GetUpdates`, `GetUpdateDetail`, `UpdatePackages`, `RepairSystem`, `GetOldTransactions`); `/var/log/apt/history.log*`; `dpkg --audit`; APT source files and signed indexes; `/run/reboot-required`; logind for "Restart now" |
| Command line | `nexus-updater [page]`, `nexus-updater --check` (no window; exit 0 no updates, 10 updates, 11 security updates, 1 check failed), `--dump status\|updates\|update-details\|history\|reboot\|health\|sources\|repos\|settings` |
| Privileges | none of its own; PackageKit and logind authorise through polkit |
| Limitations | the periodic check is not scheduled by anything yet; no offline updates and no automatic installation; repair is PackageKit's `RepairSystem` only |
| Verified (per README) | against a real `packagekitd` in an overlay: a security update detected (`--check` exit 11) and installed through the GUI; repair of a half-installed package; restart banner from `/run/reboot-required` |
| Not verified | polkit prompts; restart through logind; updates that remove packages; kernel and NEXUS groups with real updates |

## Arsenal manager

[`arsenal/manager/README.md`](../../arsenal/manager/README.md) — user-facing
description in [../security/toolkit.md](../security/toolkit.md).

| | |
|---|---|
| Purpose | browse the Arsenal tool profiles, see what is installed, install or remove a profile or a single tool, find out how to start each tool |
| Binary / package | `nexus-arsenal` / `nexus-arsenal` (manifests in `nexus-arsenal-data`). Single instance |
| Data sources | manifests in `/usr/share/nexus/arsenal/profiles`; `dpkg-query`, `apt-cache show`, `apt-get install --simulate` (read-only); PackageKit availability over D-Bus |
| Command line | `nexus-arsenal [--profiles-dir DIR] [--page overview\|tools\|external\|responsible-use\|<profile>]`, `--dump profiles\|tools\|state` |
| Privileges | changes only through PackageKit (polkit); no helper; never runs apt or dpkg to change anything. Unsigned packages are never installed. Without PackageKit the dialogs show the exact `sudo apt …` command instead |
| First start | an acknowledgement of responsible use must be ticked; stored in `~/.config/nexus/arsenal.conf` |
| Limitations | licence agreements, new signing keys and media changes are reported as errors, not handled; external tools are listed with links only |
| Verified (per README) | builds; smoke test; dumps equal `dpkg-query`; acknowledgement, navigation, the apt fallback dialog and starting a tool's help in NEXUS Terminal in a nested session |
| Not verified | **every PackageKit transaction path** (no system bus in the chroot): install, remove, progress, cancel, errors |

## Installer

[`system/installer/gui/README.md`](../../system/installer/gui/README.md) —
user guide in [../installation/installing.md](../installation/installing.md).

| | |
|---|---|
| Purpose | the installation wizard of the live session; a front-end to the installer engine |
| Binary / package | `nexus-installer` / `nexus-installer-gui`; engine `/usr/sbin/nexus-installer-engine` in `nexus-installer` |
| Data sources | engine `probe` and `plan`; `/usr/share/i18n/SUPPORTED`; XKB rules; zone tables; Arsenal manifests |
| Command line | `nexus-installer [--allow-installed-system] [--profiles-dir DIR]`, `--dump environment\|locales\|keyboard\|timezones\|profiles\|probe\|rules`; test options in the README |
| Privileges | `pkexec /usr/sbin/nexus-installer-engine …`; polkit action `org.nexus.installer.run` (`auth_admin`, not kept). In the live session a polkit rule grants it without a password to a local, active member of group `sudo` while `/run/nexus/live-session` exists. Outside a live session the destructive phase is refused unless started with `--allow-installed-system`. Passwords go only to the engine's standard input |
| Limitations | English only; sizes are the engine's automatic choice; manual mode assigns existing partitions only and has no encryption |
| Verified (per README) | every step and failure state rendered headless; the engine's real planner on synthetic probes; a keyboard-only walk-through with a simulated run; the bytes sent to the engine |
| Not verified | a real installation through this front-end; pkexec with a running polkit daemon; "Restart now" |

## Terminal

[`applications/terminal/README.md`](../../applications/terminal/README.md)

**Not part of package builds or the image yet**: the directory is listed in
`build/packages/wip.exclude` and `debian/control` has no package for it. The
shell's terminal shortcut and the Arsenal manager's "show usage in terminal"
therefore find no terminal in the current image.

| | |
|---|---|
| Purpose | terminal emulator: tabs, split panes, profiles, SSH connections, search in the scrollback, session restore |
| Binary | `nexus-terminal` (Qt Widgets with QTermWidget; does not use `Nexus::App` but implements the same contract) |
| Data sources | passwd database, `/etc/shells`, `/proc/<pid>/cwd` and `comm`, `~/.config/nexus/terminal.conf`, the shared appearance state |
| Command line | `nexus-terminal [--workdir <dir>] [--profile <name>] [--no-restore] [--hold] [-e <command> [args...]]`, `--dump profiles\|schemes\|shells\|shortcuts\|ssh\|settings` |
| Privileges | none; no polkit actions |
| Security properties | nothing is started through a shell; SSH fields are validated and `--` precedes the host; pasted control characters are removed and multi-line pastes confirmed; session restore never stores commands or terminal contents |
| Limitations | no exit status of ended panes; search shows one match at a time; English only |

## In progress

These directories exist but are incomplete or not integrated. They are
excluded from package builds by `build/packages/wip.exclude` and are not
described here until they have a README and a package.

| Path | Intended application |
|---|---|
| `applications/editor` | text editor |
| `applications/archive-manager` | archive manager |
| `applications/disk-manager` | disk manager |
| `applications/screenshot` | screenshot tool (`nexus-screenshot`) |
| `applications/security-center` | Security Center (front-end to `nexus-security-status`) |
| `system/recovery` | recovery tool (`nexus-recovery`) |
| `applications/control-center` pages: Appearance, Accessibility, Startup, Language, Date & Time, Storage, Users, Privacy, Firewall, Security, Updates | further Control Center pages |
| `desktop/horizon-shell/modules/power` | shell power module |

The command-line programs `nexus-security-status` and `nexus-firewall`
(package `nexus-hardening`) are complete and documented in
[../security/hardening.md](../security/hardening.md).
