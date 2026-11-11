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

Everything below ran in the Debian 13 build chroot (no systemd, no hardware),
build directory `ccsec`:
`scripts/dev.sh build --dir ccsec --only control-center --cc-pages "users;privacy;firewall;security;updates"`.
The GUI was driven with xdotool under Xvfb; each `scripts/dev.sh exec`
invocation has a private mount namespace, in which `/etc` (and where needed
`/var`, `/home`, `/tmp`, `/var/tmp`, `/var/lib/AccountsService`) was covered
by a throw-away overlay or tmpfs first, so the shared chroot was not changed.
The Control Center ran as root there unless stated, i.e. tools were started
directly, not through `pkexec`.

* **Smoke**: `tests/desktop/smoke.sh --dir ccsec control-center` → 1 passed,
  0 failed; additionally `scripts/dev.sh run --dir ccsec --smoke
  nexus-control-center <key>` for each of the five keys, dark and light:
  `NEXUS-SMOKE-OK`, no QML warnings (states without any service:
  `ccsec-<key>-1.png`, `ccsec-<key>-light.png`).
* **Firewall** — real `security/firewall/nexus-firewall` and
  `/etc/nftables.conf` from `security/firewall/`, inside `unshare -n` (own
  network namespace, real nftables): `--dump firewall` equals
  `nexus-firewall status --json` before and after changes; through the GUI:
  turn on (table loaded, policies "drop", 32 rules, `ccsec-fw-b1.png`), open
  `8000-8090/udp` with description and `22/tcp` (validation message for
  `8000-80`, `ccsec-fw-b2.png`; result `ccsec-fw-c1.png`), close one port
  (confirmation, `ccsec-fw-c2.png`), reload, close all (`ccsec-fw-c4.png`),
  turn off after the warning (`ccsec-fw-c3.png`); `nexus-firewall list` /
  `status` confirmed each step. As uid 65534 without polkit: state reported as
  unknown with the reason.
* **Security** — real `security/audit/nexus-security-status` as root:
  `--dump security` and the page (`ccsec-sec-a1.png`, `ccsec-sec-a3.png`) show
  what the tool reports for the chroot (firewall not active, AppArmor module
  not loaded, legacy BIOS, no pending updates, baseline not installed, …) and
  its two recommendations; the "Firewall settings" button opens the Firewall
  page (`ccsec-sec-c1.png`). As uid 65534 without polkit: the report is
  produced unprivileged and marked incomplete (`privileged: false`,
  "polkit did not authorise the request"). The rendering of the branches the
  chroot cannot produce (UEFI, LUKS, pending security updates, failed units,
  failed logins, sysctl drift, …) was checked with a **test double** that
  prints a hand-written report in the tool's format
  (`ccsec-sec-testdouble-1.png` … `-3.png`; not real data).
* **Updates** — `updates-helper` directly: usage errors, `1 1`, `0 0` on a
  file with other lines (kept), refusal as non-root. GUI: automatic check
  off → `AutoCheck=false`; interval → `CheckIntervalHours=72`; "Refresh the
  package lists" switch → helper → `20auto-upgrades` written, then switched
  off again; `/run/reboot-required` banner. "Check now" with a real
  `nexus-updater` (built in `ccsecupd`): without a system bus the updater's
  error is shown (`ccsec-upd-b1.png`); against a real `packagekitd` on a
  private system bus (overlay over `/var`) the check ran and the page showed
  "No updates were available" with the time the Updater recorded
  (`ccsec-upd-c2.png`).
* **Users** — real `accounts-daemon` and `polkitd` on a private system bus,
  two accounts created with `useradd` in the overlay. `--dump users` lists
  them with role, home, lock state. GUI as root (`ccsec-users-a1.png`,
  `-b1…b7`, `-d3`, `-g1…g3`): add an administrator (checked with `id`,
  `getent passwd`, a yescrypt hash in shadow, home created); make a standard
  user an administrator (`sudo` group); change the full name; set another
  account's password through the helper (hash changed; `su` with the new
  password works, with a wrong one fails); automatic sign-in on →
  `/etc/sddm.conf.d/20-autologin.conf`; picture → 256×256 PNG in
  `/var/lib/AccountsService/icons/`, shown round in the list; remove an
  account keeping its files, and another one with its files (typed
  confirmation); removing the auto-login account clears the SDDM file; last
  administrator: switch and Remove disabled with the reason. GUI as the
  ordinary user `ada` (no polkit agent): "this is you", Remove blocked;
  changing the own password with a wrong current password → "the current
  password is not correct" (`ccsec-users-e3.png` of an earlier run), with the
  right one → hash changed (`ccsec-users-e4.png`); own full name changed
  through AccountsService; adding a user and automatic sign-in are refused
  and say so, switches fall back (`ccsec-users-f1.png`, `-f3.png`).
  `users-helper` directly: every refusal path (system account, unknown user,
  invalid name, empty password, control characters, missing separator,
  non-root) and `set-autologin` / `clear-autologin` with competing entries in
  `10-nexus-live.conf` and `/etc/sddm.conf`.
* **Privacy** — prepared home directory: `--dump privacy` reports the
  measured sizes exactly (trash 409 613 bytes in 3 files, thumbnails 51 200,
  old temporary files 65 541 in 2 files, a fresh file and another user's old
  file not counted), 4 recent entries, 2 launcher counters, `Timeout=10` from
  `/etc/xdg/kscreenlockerrc`, and NetworkManager values with the right source
  file when `90-local.conf` overrides `80-nexus-privacy.conf`
  (`ccsec-priv-b1.png`, `-b2.png`). GUI: every lock control writes
  `~/.config/kscreenlockerrc`; both history lists cleared (other groups of
  `horizon-usage.conf` kept); trash emptied (`files`/`info` kept), thumbnails
  deleted, old temporary files deleted while the fresh and the foreign file
  stayed (confirmation dialogs `ccsec-priv-c3.png`, `-c6.png`).

## Not tested

* Any polkit password prompt (`pkexec` and AccountsService with an agent in a
  real session): in the chroot the privileged paths ran as root, and the
  unprivileged paths only up to polkit's refusal.
* `org.kde.screensaver configure` against a running locker (KWin's locker is
  not started in the test sessions); only the file is verified.
* logind's `ListUsers` ("signed in" block), SDDM actually logging a user in,
  the `Session=` line (the session file is not installed in the chroot),
  AccountsService's `Changed` signals arriving from another program.
* unattended-upgrades installed, `apt-daily-upgrade.timer` state, a check
  that finds updates (the Updater's own tests cover its result format).
* The pages inside a full build with all other pages, and in a real NEXUS
  HORIZON session.
