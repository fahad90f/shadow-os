# Control Center — Users, Privacy, Firewall, Security, Updates

This file covers five pages of `nexus-control-center`
(`pages/{Users,Privacy,Firewall,Security,Updates}Page.qml`,
`backends/{users,privacy,firewall,security,updates}backend.{h,cpp}`,
`helpers/{users,updates}-helper`,
`helpers/org.nexus.controlcenter.{users,updates}.policy`). The other pages
are documented by their authors (`README-network-hardware.md`, …).

Open a page with `nexus-control-center <key>`; the data behind it is printed
by `nexus-control-center --dump <key>` (keys: `users privacy firewall security
updates`).

## What each page does and where its data comes from

| Page | Functions | Sources and sinks |
|---|---|---|
| Users (`users`) | list of human accounts; add (full name, user name, password, administrator); remove with or without the home folder (typed confirmation); full name; password; administrator role; automatic sign-in; picture | AccountsService `org.freedesktop.Accounts` on the system bus: `ListCachedUsers`, properties of `…Accounts.User`, `CreateUser`, `DeleteUser`, `User.SetRealName`, `User.SetAccountType` (administrator = `sudo` group on Debian), `User.SetIconFile`, signals `UserAdded`/`UserDeleted`/`User.Changed`. Passwords: see below. Automatic sign-in: SDDM `[Autologin] User`, read from `/usr/lib/sddm/sddm.conf.d/*.conf`, `/etc/sddm.conf.d/*.conf`, `/etc/sddm.conf` (SDDM's order, last wins), written by `users-helper`. Signed-in users: `org.freedesktop.login1.Manager.ListUsers`. |
| Privacy (`privacy`) | screen lock (automatic lock, delay, lock on resume, grace period); clear the recently-used-files list; clear the launcher's usage counters; empty the trash, delete the thumbnail cache and old temporary files, with measured sizes; effective MAC-address and IPv6 privacy defaults of NetworkManager (read-only) | `kscreenlockerrc` `[Daemon] Autolock, Timeout, LockOnResume, LockGrace` through KConfig (so `/etc/xdg/kscreenlockerrc` supplies the defaults), applied with `org.kde.screensaver /ScreenSaver configure`; `~/.local/share/recently-used.xbel` (+ `~/.local/share/RecentDocuments/*.desktop`); `~/.config/nexus/horizon-usage.conf` `[Launches]`; `~/.local/share/Trash`, `~/.cache/thumbnails`, the user's own regular files in `/tmp` and `/var/tmp` not modified for 24 h (`lstat`, in a worker thread); NetworkManager configuration files |
| Firewall (`firewall`) | state (active, starts at boot, policies, rule count, foreign nftables tables); turn on / off (warning first); opened ports; open a port or range with description (validated); close one (confirmation); close all (confirmation); reload | `/usr/sbin/nexus-firewall` (`security/firewall/nexus-firewall`) |
| Security (`security`) | read-only: recommendations as the tool words them, and one row per examined area (firewall, AppArmor, disk encryption, Secure Boot, updates, listening services, system services, login activity, audit, kernel hardening) with the state and the evidence the report contains; "Open Security Center" when `nexus-security-center` is in `PATH` | `/usr/sbin/nexus-security-status --section firewall,apparmor,encryption,secureboot,updates,listening,services,logins,audit,hardening,recommendations` (`security/audit/nexus-security-status`) |
| Updates (`updates`) | automatic check on/off and interval; result of the last check; "Check now"; unattended-upgrades state and the two `APT::Periodic` switches; restart-required notice; "Open NEXUS Updater" | `~/.config/nexus/updater.conf` (`Updates/AutoCheck`, `Updates/CheckIntervalHours`, read-only `LastCheck/*`; same `QSettings` store as NEXUS Updater); `nexus-updater --check`; `/usr/bin/unattended-upgrade` (installed?); `apt-config dump` (effective `APT::Periodic::Update-Package-Lists` / `Unattended-Upgrade`; `/etc/apt/apt.conf.d/20auto-upgrades` parsed directly when `apt-config` is missing); `systemctl is-enabled apt-daily-upgrade.timer` when systemd runs; `/run/reboot-required` |

Details worth knowing:

* **Security page shows nothing of its own.** Every state badge and evidence
  line is a field of the report turned into a sentence
  (`backends/securitybackend.cpp`, one function per section); a section the
  tool could not examine shows the tool's `reason`. There are no scores. The
  colour of a badge follows a boolean of the report (e.g. `active`, `enabled`,
  `root.encrypted`), neutral where the report has none. A recommendation's
  `action` hint becomes a button only when it can be honoured from here:
  `firewall-enable` → the Firewall page (if built in), `open-updater` →
  `nexus-updater` (if installed).
* **Firewall status without a password.** `nexus-firewall status` needs root.
  For an ordinary user the page reads the same document through
  `pkexec nexus-security-status --section firewall` (no prompt for an active
  local session). If that fails, the opened ports are read from the
  world-readable `/etc/nexus/firewall.json` and the live state is shown as
  unknown, with the reason. Port and description are validated with the
  tool's own grammar before `pkexec` is started.
* **Privacy → Network identity** merges NetworkManager's files in its order:
  `/usr/lib/NetworkManager/conf.d/*.conf` (unless a file of the same name
  exists in `/run` or `/etc`), `/run/NetworkManager/conf.d/*.conf`,
  `/etc/NetworkManager/NetworkManager.conf`, `/etc/NetworkManager/conf.d/*.conf`.
  For each of `[device] wifi.scan-rand-mac-address`,
  `[connection] wifi.cloned-mac-address`, `ethernet.cloned-mac-address`,
  `ipv6.ip6-privacy` it shows the value and the file it comes from, or says
  that it is not set. Whether `80-nexus-privacy.conf` is present (in either
  directory) is stated. Nothing is changed here; per-network overrides are on
  the Wi-Fi page.
* **Location services**: NEXUS OS ships none (no GeoClue anywhere in the
  repository), so the Privacy page has no such section.
* **Users → blocks** (enforced in the back-end as well as in the UI): the
  account the Control Center runs as and accounts logind reports as signed in
  cannot be removed; the only (unlocked) administrator can neither be removed
  nor made a standard user. The reason is written next to the disabled
  control. Removing the account that is signed in automatically also clears
  SDDM's auto-login.
* **Users → picture**: the chosen image is cut to a centred square, scaled to
  256 px and handed to AccountsService as a temporary PNG (AccountsService
  refuses large files), then the temporary file is deleted.

## Privileged operations

| What | How | polkit action |
|---|---|---|
| create/remove accounts, other people's name/role/picture | AccountsService asks polkit itself | `org.freedesktop.accounts.user-administration` (own name and picture: `…change-own-user-data`) |
| another account's password; the password of a new account | `pkexec /usr/libexec/nexus/users-helper set-password`, `<user>:<password>` on **stdin**, passed on to `chpasswd` on its stdin | `org.nexus.controlcenter.users.manage` (`auth_admin`) |
| one's own password | no privilege: `passwd` is run as the user with current and new password on **stdin**; PAM verifies the current one and applies the system's password rules | — |
| automatic sign-in | `pkexec users-helper set-autologin <user>` / `clear-autologin`: writes `/etc/sddm.conf.d/20-autologin.conf` (the file the installer uses; `Session=nexus-horizon` when that session is installed) and removes competing `[Autologin] User=` lines from the other SDDM files under `/etc` | `org.nexus.controlcenter.users.manage` |
| firewall changes | `pkexec /usr/sbin/nexus-firewall enable|disable|allow|remove|reload|reset` | `org.nexus.firewall.manage` (existing, `security/permissions/org.nexus.security.policy`) |
| security report, firewall status | `pkexec /usr/sbin/nexus-security-status --section …` | `org.nexus.security.read-status` (existing; no prompt for an active session) |
| APT periodic settings | `pkexec /usr/libexec/nexus/updates-helper set-auto-upgrades <0|1> <0|1>`: sets the two keys in `/etc/apt/apt.conf.d/20auto-upgrades`, keeps other lines | `org.nexus.controlcenter.updates.configure` (`auth_admin`) |

Both helpers are Python 3 (standard library), accept a closed set of
sub-commands, validate every argument (user names: `^[a-z_][a-z0-9_-]{0,31}$`,
must exist, uid within `UID_MIN…UID_MAX` of `/etc/login.defs`), never use a
shell, write files atomically and print one JSON document. No password is
ever put on a command line or into the environment, and none is logged.
When the Control Center itself runs as root the tools are started directly
instead of through `pkexec`.

Development-only environment variables (they only change which program the
*unprivileged* GUI starts; `pkexec` then asks polkit about that path, and the
shipped policies match the installed paths only): `NEXUS_FIREWALL_TOOL`,
`NEXUS_SECURITY_STATUS_TOOL`, `NEXUS_CC_HELPER_DIR`.

## Run-time dependencies (Debian packages)

`accountsservice`, `polkitd` + `pkexec` and a polkit agent in the session,
`passwd` (`passwd`, `chpasswd`), `python3`, `sddm` (automatic sign-in only),
`systemd` (logind for "signed in"; optional), `libkf6configcore6`,
`kwin-wayland` with `libkscreenlocker` (the `org.kde.screensaver` service),
`nftables` and the NEXUS packages that install `/usr/sbin/nexus-firewall`,
`/usr/sbin/nexus-security-status`, `/etc/nftables.conf` and
`org.nexus.security.policy`, `apt` (`apt-config`), optional
`unattended-upgrades`, optional `nexus-updater`, `nexus-security-center`;
`qml6-module-qtquick-dialogs` (picture chooser), `qml6-module-qtquick-effects`
(round pictures); `network-manager` only for what the Privacy page reports
about it.

## Limitations

* Users: accounts are listed as AccountsService reports them
  (`ListCachedUsers`, system accounts skipped). No password hints, no
  account locking, no user-name or home-folder changes, no group editing
  beyond the administrator role. A new account always gets a password; if the
  password step fails after the account was created, the page says so and the
  account stays locked until a password is set.
* Users: the local minimum for a new password is 8 characters; an
  administrator setting someone else's password goes through `chpasswd`,
  which does not apply PAM quality rules. Changing one's own password goes
  through PAM and can be refused by it; the page shows what `passwd` printed.
* Users: automatic sign-in is implemented for SDDM only. The switch is
  disabled, with the reason, when `/usr/bin/sddm` is missing.
* Privacy: the launcher keeps its counters in memory, so a running shell
  writes old counts back for applications started afterwards until it is
  restarted (see "Changes needed elsewhere").
* Privacy: the screen-lock settings here and the "Lock the session after"
  setting of the Power page (`~/.config/nexus/power.conf`, followed by the
  shell) are two separate mechanisms.
* Privacy: only the trash in the home directory is measured and emptied
  (not `.Trash-<uid>` folders on other file systems). "Old temporary files"
  can remove a file a long-running application still uses; the confirmation
  says so. NetworkManager sections restricted to particular devices
  (`[connection-…]`, `[device-…]`) are not evaluated.
* Firewall: only what `nexus-firewall` offers — inbound TCP/UDP ports and
  ranges; no per-address or outbound rules.
* Security: the page needs up to a minute because the tool simulates an
  upgrade with apt for the `updates` section. Recommendations are shown in
  the tool's language (English).
* Updates: `APT::Periodic` values other than 0/1 (for example "7") are shown
  as they are, and a switch replaces them with 0 or 1. Which origins
  unattended-upgrades installs from (`50unattended-upgrades`) is not edited.

## What was tested, and how

See the final section of this file's history in the engineer's report; the
summary, kept current:

TESTED-PLACEHOLDER
