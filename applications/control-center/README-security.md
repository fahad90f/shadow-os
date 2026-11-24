# Control Center — Users, Privacy, Firewall, Security, Updates

This file covers five pages of `shadow-control-center`
(`pages/{Users,Privacy,Firewall,Security,Updates}Page.qml`,
`backends/{users,privacy,firewall,security,updates}backend.{h,cpp}`,
`backends/privacykillswitchbackend.{h,cpp}` (Privacy: kill switch and panic),
`helpers/{users,updates}-helper`,
`helpers/org.shadow.controlcenter.{users,updates}.policy`). The other pages
are documented by their authors (`README-network-hardware.md`, …).

Open a page with `shadow-control-center <key>`; the data behind it is printed
by `shadow-control-center --dump <key>` (keys: `users privacy killswitch
firewall security updates`).

## What each page does and where its data comes from

| Page | Functions | Sources and sinks |
|---|---|---|
| Users (`users`) | list of human accounts; add (full name, user name, password, administrator); remove with or without the home folder (typed confirmation); full name; password; administrator role; automatic sign-in; picture | AccountsService `org.freedesktop.Accounts` on the system bus: `ListCachedUsers`, properties of `…Accounts.User`, `CreateUser`, `DeleteUser`, `User.SetRealName`, `User.SetAccountType` (administrator = `sudo` group on Debian), `User.SetIconFile`, signals `UserAdded`/`UserDeleted`/`User.Changed`. Passwords: see below. Automatic sign-in: SDDM `[Autologin] User`, read from `/usr/lib/sddm/sddm.conf.d/*.conf`, `/etc/sddm.conf.d/*.conf`, `/etc/sddm.conf` (SDDM's order, last wins), written by `users-helper`. Signed-in users: `org.freedesktop.login1.Manager.ListUsers`. |
| Privacy (`privacy`) | network kill switch: state in force (checked against the kernel), mode Off / Tor only / VPN only / Tor or VPN, allow the local network, keep after a restart, reload a table that is not loaded; panic: state, reconnect (release), the shortcut, the options (read-only); screen lock (automatic lock, delay, lock on resume, grace period); clear the recently-used-files list; clear the launcher's usage counters; empty the trash, delete the thumbnail cache and old temporary files, with measured sizes; effective MAC-address and IPv6 privacy defaults of NetworkManager (read-only) | `kscreenlockerrc` `[Daemon] Autolock, Timeout, LockOnResume, LockGrace` through KConfig (so `/etc/xdg/kscreenlockerrc` supplies the defaults), applied with `org.kde.screensaver /ScreenSaver configure`; `~/.local/share/recently-used.xbel` (+ `~/.local/share/RecentDocuments/*.desktop`); `~/.config/shadow/horizon-usage.conf` `[Launches]`; `~/.local/share/Trash`, `~/.cache/thumbnails`, the user's own regular files in `/tmp` and `/var/tmp` not modified for 24 h (`lstat`, in a worker thread); NetworkManager configuration files; kill switch and panic: `/run/shadow/killswitch.json`, `/run/shadow/panic.json`, `/etc/shadow-os/{killswitch,panic}.json`, `/run/shadow-anon-state.json`, `pkexec shadow-security-status --section killswitch,panic`, `pkexec /usr/sbin/shadow-killswitch set …|reassert`, `pkexec /usr/sbin/shadow-panic release`, KGlobalAccel `shortcut(["shadow-horizon","panic",…])` on the session bus |
| Firewall (`firewall`) | state (active, starts at boot, policies, rule count, foreign nftables tables); turn on / off (warning first); opened ports; open a port or range with description (validated); close one (confirmation); close all (confirmation); reload | `/usr/sbin/shadow-firewall` (`security/firewall/shadow-firewall`) |
| Security (`security`) | read-only: recommendations as the tool words them, and one row per examined area (firewall, AppArmor, disk encryption, Secure Boot, updates, listening services, system services, login activity, audit, kernel hardening) with the state and the evidence the report contains; "Open Security Center" when `shadow-security-center` is in `PATH` | `/usr/sbin/shadow-security-status --section firewall,apparmor,encryption,secureboot,updates,listening,services,logins,audit,hardening,recommendations` (`security/audit/shadow-security-status`) |
| Updates (`updates`) | automatic check on/off and interval; result of the last check; "Check now"; unattended-upgrades state and the two `APT::Periodic` switches; restart-required notice; "Open SHADOW Updater" | `~/.config/shadow/updater.conf` (`Updates/AutoCheck`, `Updates/CheckIntervalHours`, read-only `LastCheck/*`; same `QSettings` store as SHADOW Updater); `shadow-updater --check`; `/usr/bin/unattended-upgrade` (installed?); `apt-config dump` (effective `APT::Periodic::Update-Package-Lists` / `Unattended-Upgrade`; `/etc/apt/apt.conf.d/20auto-upgrades` parsed directly when `apt-config` is missing); `systemctl is-enabled apt-daily-upgrade.timer` when systemd runs; `/run/reboot-required` |

Details worth knowing:

* **Security page shows nothing of its own.** Every state badge and evidence
  line is a field of the report turned into a sentence
  (`backends/securitybackend.cpp`, one function per section); a section the
  tool could not examine shows the tool's `reason`. There are no scores. The
  colour of a badge follows a boolean of the report (e.g. `active`, `enabled`,
  `root.encrypted`), neutral where the report has none. A recommendation's
  `action` hint becomes a button only when it can be honoured from here:
  `firewall-enable` → the Firewall page (if built in), `open-updater` →
  `shadow-updater` (if installed).
* **Firewall status without a password.** `shadow-firewall status` needs root.
  For an ordinary user the page reads the same document through
  `pkexec shadow-security-status --section firewall` (no prompt for an active
  local session). If that fails, the opened ports are read from the
  world-readable `/etc/shadow-os/firewall.json` and the live state is shown as
  unknown, with the reason. Port and description are validated with the
  tool's own grammar before `pkexec` is started.
* **Privacy → Network identity** merges NetworkManager's files in its order:
  `/usr/lib/NetworkManager/conf.d/*.conf` (unless a file of the same name
  exists in `/run` or `/etc`), `/run/NetworkManager/conf.d/*.conf`,
  `/etc/NetworkManager/NetworkManager.conf`, `/etc/NetworkManager/conf.d/*.conf`.
  For each of `[device] wifi.scan-rand-mac-address`,
  `[connection] wifi.cloned-mac-address`, `ethernet.cloned-mac-address`,
  `ipv6.ip6-privacy` it shows the value, the file it comes from and, when it
  is not the plain `[connection]`/`[device]`, the section; or says that it is
  not set. The value is the one NetworkManager applies to every device: named
  `[connection-…]`/`[device-…]` sections are searched before the plain one
  (later files first, within a file top to bottom), a named section without
  `match-device` applies to every device, one with `match-device` (some
  devices only, e.g. the shipped `[connection-shadow-mac-fixed-drivers]`) is
  not evaluated, and `stop-match=yes` without `match-device` leaves the rest
  unset. A boot with `shadow.netprivacy=off` is one such case:
  `/run/NetworkManager/conf.d/93-shadow-escape.conf` has
  `[connection-shadow-escape]` with `*.cloned-mac-address=preserve`, so the
  rows read "Unchanged" from that file and the section says "Real addresses
  for this boot" (as Network Center → Privacy does) instead of naming the
  SHADOW defaults. Whether `80-shadow-privacy.conf` is present (in either
  directory) is stated otherwise. Nothing is changed here; per-network
  overrides are on the Wi-Fi page.
* **Privacy → Network kill switch and Panic** (`backends/privacykillswitchbackend.h`;
  the tools and files are described in
  [docs/security/kill-switch.md](../../docs/security/kill-switch.md)).
  * *State.* Read from `/run/shadow/killswitch.json` and
    `/run/shadow/panic.json` (both world-readable, rewritten atomically by the
    tools; their directories are watched, plus a 10-second timer). The rule of
    the contract is applied as in the shell's status module: only `active &&
    loaded && error == null` in a sound document counts as protected. A file
    that cannot be read, a field of the wrong type, a document that
    contradicts itself (mode `tor` with `active: false`) or an unknown mode is
    an error that is shown, never "protected". Error, escape-hatch
    (`shadow.killswitch=off`), "no status recorded" and "not installed" each
    have their own text. The mode after a restart comes from the status
    (`persistent_mode`), else from `/etc/shadow-os/killswitch.json`; a broken
    configuration file is reported together with what the tool then does at
    start-up (fail closed, Tor or VPN).
  * *Checked against the kernel.* When the page opens and after every change,
    `pkexec shadow-security-status --section killswitch,panic` (no password for
    an active local session) says whether the tables are in the kernel now. A
    status file that says "loaded" while the kernel has no table (for example
    a table deleted by hand, or `nft flush ruleset`) is shown as "not
    protected". A check made before the status file last changed is marked as
    such and not used.
  * *Changing.* The mode control runs `pkexec /usr/sbin/shadow-killswitch set
    MODE --lan|--no-lan` (plus `--runtime` while "Keep after a restart" is
    off); turning it on, or a stricter mode, first asks for confirmation,
    because the tool then closes connections (the same rule as its
    `needs_flush`). "Allow the local network" can only be changed while the
    kill switch is on. "Keep after a restart" follows the state (a `--runtime`
    mode in force shows it off); switching it on while a runtime mode is in
    force saves that mode for start-up at once (it neither loosens nor
    tightens anything now); switching it off only changes how the next change
    is made — there is no way to un-save a mode without loading another one,
    and loading `off` first would open a gap. A failed change shows the tool's
    own message without its warnings (those are in the status file and shown
    under "Last change"); a refused authorisation says so. A change the tool
    could not make while the previous mode stayed in force is its
    `change_error` (and a warning), not an `error`: the previous mode is still
    shown as protected. "Load again" (`reassert`) is offered while the kill
    switch is on and has a problem; it reports success only if the status it
    then reads has none left. The problem banner says "Traffic may leave
    outside Tor or the VPN" only when the mode's table is not in force
    (`trafficMayLeave` in `--dump killswitch`), otherwise "Kill switch
    problem" (for example a broken configuration while it fails closed).
  * *Hide Me.* When `/run/shadow-anon-state.json` says Hide Me engaged the kill
    switch (`killswitch_by`) and the mode is still `tor`, the page says for
    what and that Hide Me puts the previous mode back.
  * *Panic.* State, and "Reconnect…" (`pkexec /usr/sbin/shadow-panic release`,
    administrator every time) while a panic is engaged or its table is
    loaded. Engaging is left to the shortcut (the shell starts
    `shadow-panic.service`). The key shown is the one KGlobalAccel has bound to
    the shell's `panic` action in this session (method `shortcut` of
    `org.kde.KGlobalAccel`, only when the service is already running, never by
    D-Bus activation); otherwise the shell's default Ctrl+Alt+Shift+Esc with a
    note. The options (radios, lock, clipboard) come from
    `/etc/shadow-os/panic.json`, read with the tool's own rules (missing or
    broken: all on, and the page says why). They are shown read-only:
    `shadow-panic` has no command that changes them.
  * Development hooks: `SHADOW_KILLSWITCH_TOOL`, `SHADOW_PANIC_TOOL`,
    `SHADOW_SECURITY_STATUS_TOOL`, `SHADOW_NETGUARD_RUN_DIR`,
    `SHADOW_NETGUARD_CONFIG_DIR`, `SHADOW_ANON_STATE`.
* **Privacy → Hide Me** (`backends/privacybackend.{h,cpp}`, the
  `shadow-anon` part) runs `pkexec /usr/bin/shadow-anon COMMAND` (the tool
  directly as root) and reads `/run/shadow-anon-state.json` and the rotation
  timer's drop-in.
  * *Exit status.* 0: done, with a message saying what happened. 3: the tool
    could not switch a protection on or off; the page shows the tool's own
    last stderr line (`shadow-anon: error: …` without the prefix), and the
    state record's `error` stays on the page as a warning until a later change
    works (it also reports failures of the boot unit and the rotation timer).
    1 or 2 (not root, bad use) are failures too, shown the same way. pkexec's
    126 (authentication cancelled or refused) and 127 (not authorised, or the
    program cannot be run) say that nothing was changed. The same mapping
    applies to `shadow-harden`, which exits 1 when something could not be
    applied (that used to be taken for a cancelled dialog and not shown).
  * *Kill switch.* "Kill switch: Tor only" runs `shadow-anon kill-switch-on`
    / `kill-switch-off` (never `--persist`), which hand over to
    `shadow-killswitch` mode `tor` and put the previous mode back. Its state is
    read from the same document as the Network kill switch section
    (`KillSwitchBackend`, `/run/shadow/killswitch.json`), never from Hide Me's
    own record, so the two cannot disagree: another mode in force is named;
    "Tor only" chosen in the Network section is shown as on there and is
    changed there, but not with the local network allowed: that is shown as
    "Tor only, local network allowed", is not counted as Hide Me's Tor only,
    and the switch stays usable to make it strict (`set tor --no-lan`); a Ghost Mode whose kill switch is no longer in force is
    reported as such (danger banner, row in red); the transparent proxy shows
    "ACTIVE, no kill switch" in that case. Each section is disabled while the
    other one is changing something.
  * *Tor status.* "Tor: running" / "Tor: stopped" on the Rotate identity row
    (`torRunning`, also in `--dump privacy`) is `systemctl is-active --quiet
    tor@default`, the check shadow-anon's `tor_running()` makes, asked each
    time the page reads its state (when it opens, after every Hide Me action
    and on "Measure again"). Debian's `tor.service` is a oneshot running `/bin/true` that
    stays active for the whole boot while `tor@default` fails, so it is never
    asked; no answer within 2 s counts as stopped.
  * *Texts* follow the reworked tool: Ghost Mode engages the kill switch first
    and changes nothing else when that fails; the threat levels never change
    the network address (Maximum is Ghost Mode without that step); "Rotate
    identity" gives each connected network a new address that stays stable
    for that network, at most once in 5 minutes; the timer ("Auto-rotate Tor
    circuits", interval read from its drop-in) only renews Tor circuits, at
    least 60 s apart (`startAutoRotate` raises anything shorter to 60, as the
    tool does); USB storage blocking does not stop other USB devices. Ghost
    Mode and threat level Maximum ask for confirmation first (they close
    connections outside Tor and wipe logs).
  * *OS integrity → Filesystem hardening* says that `fs-harden` creates the
    group `proc` and adds polkitd to it (`hidepid=2,gid=proc`), so polkit and
    pkexec keep working, and names the escape hatch `shadow.hidepid=off`.
    That is what `/usr/bin/shadow-harden` of the same version does: it is in
    the package `shadow-hardening`, which the Control Center depends on at
    its own version or later (in 0.2.0 the tool was only copied into the
    image, and upgrades kept the old one); the package upgrade also rewrites
    a boot unit the old tool wrote without the group.
  * Development hooks: `SHADOW_ANON_TOOL`, `SHADOW_HARDEN_TOOL`,
    `SHADOW_ANON_STATE`, `SHADOW_ANON_TIMER_DROPIN`, and
    `SHADOW_NM_CONFIG_ROOT` (read in place of `/` for NetworkManager's
    configuration files and `/usr/sbin/NetworkManager`, for Network
    identity). `--dump privacy` has a `hideMe` object and a `network` object
    (`settings` with `group` per value, `netPrivacyEscapeFile`).
* **Privacy → Network identity** also shows `connection.stable-id`
  (`${CONNECTION}/${BOOT}`: a new private address at every boot) and points to
  Network Center → Privacy, where the policy is changed.
* **Location services**: Shadow OS ships none (no GeoClue anywhere in the
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
| another account's password; the password of a new account | `pkexec /usr/libexec/shadow/users-helper set-password`, `<user>:<password>` on **stdin**, passed on to `chpasswd` on its stdin | `org.shadow.controlcenter.users.manage` (`auth_admin`) |
| one's own password | no privilege: `passwd` is run as the user with current and new password on **stdin**; PAM verifies the current one and applies the system's password rules | — |
| automatic sign-in | `pkexec users-helper set-autologin <user>` / `clear-autologin`: writes `/etc/sddm.conf.d/20-autologin.conf` (the file the installer uses; `Session=shadow-horizon` when that session is installed) and removes competing `[Autologin] User=` lines from the other SDDM files under `/etc` | `org.shadow.controlcenter.users.manage` |
| firewall changes | `pkexec /usr/sbin/shadow-firewall enable|disable|allow|remove|reload|reset` | `org.shadow.firewall.manage` (existing, `security/permissions/org.shadow.security.policy`) |
| security report, firewall status | `pkexec /usr/sbin/shadow-security-status --section …` | `org.shadow.security.read-status` (existing; no prompt for an active session) |
| APT periodic settings | `pkexec /usr/libexec/shadow/updates-helper set-auto-upgrades <0|1> <0|1>`: sets the two keys in `/etc/apt/apt.conf.d/20auto-upgrades`, keeps other lines | `org.shadow.controlcenter.updates.configure` (`auth_admin`) |

Both helpers are Python 3 (standard library), accept a closed set of
sub-commands, validate every argument (user names: `^[a-z_][a-z0-9_-]{0,31}$`,
must exist, uid within `UID_MIN…UID_MAX` of `/etc/login.defs`), never use a
shell, write files atomically and print one JSON document. No password is
ever put on a command line or into the environment, and none is logged.
When the Control Center itself runs as root the tools are started directly
instead of through `pkexec`.

Development-only environment variables (they only change which program the
*unprivileged* GUI starts; `pkexec` then asks polkit about that path, and the
shipped policies match the installed paths only): `SHADOW_FIREWALL_TOOL`,
`SHADOW_SECURITY_STATUS_TOOL`, `SHADOW_CC_HELPER_DIR`.

## Run-time dependencies (Debian packages)

`accountsservice`, `polkitd` + `pkexec` and a polkit agent in the session,
`passwd` (`passwd`, `chpasswd`), `python3`, `sddm` (automatic sign-in only),
`systemd` (logind for "signed in"; optional), `libkf6configcore6`,
`kwin-wayland` with `libkscreenlocker` (the `org.kde.screensaver` service),
`nftables` and the SHADOW packages that install `/usr/sbin/shadow-firewall`,
`/usr/sbin/shadow-security-status`, `/etc/nftables.conf` and
`org.shadow.security.policy`, `apt` (`apt-config`), optional
`unattended-upgrades`, optional `shadow-updater`, `shadow-security-center`;
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
  setting of the Power page (`~/.config/shadow/power.conf`, followed by the
  shell) are two separate mechanisms.
* Privacy: only the trash in the home directory is measured and emptied
  (not `.Trash-<uid>` folders on other file systems). "Old temporary files"
  can remove a file a long-running application still uses; the confirmation
  says so. NetworkManager sections restricted to particular devices
  (`[connection-…]`, `[device-…]`) are not evaluated.
* Firewall: only what `shadow-firewall` offers — inbound TCP/UDP ports and
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
  shadow-control-center <key>` for each of the five keys, dark and light:
  `SHADOW-SMOKE-OK`, no QML warnings (states without any service:
  `ccsec-<key>-1.png`, `ccsec-<key>-light.png`).
* **Firewall** — real `security/firewall/shadow-firewall` and
  `/etc/nftables.conf` from `security/firewall/`, inside `unshare -n` (own
  network namespace, real nftables): `--dump firewall` equals
  `shadow-firewall status --json` before and after changes; through the GUI:
  turn on (table loaded, policies "drop", 32 rules, `ccsec-fw-b1.png`), open
  `8000-8090/udp` with description and `22/tcp` (validation message for
  `8000-80`, `ccsec-fw-b2.png`; result `ccsec-fw-c1.png`), close one port
  (confirmation, `ccsec-fw-c2.png`), reload, close all (`ccsec-fw-c4.png`),
  turn off after the warning (`ccsec-fw-c3.png`); `shadow-firewall list` /
  `status` confirmed each step. As uid 65534 without polkit: state reported as
  unknown with the reason.
* **Security** — real `security/audit/shadow-security-status` as root:
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
  `shadow-updater` (built in `ccsecupd`): without a system bus the updater's
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
  `10-shadow-live.conf` and `/etc/sddm.conf`.
* **Privacy** — prepared home directory: `--dump privacy` reports the
  measured sizes exactly (trash 409 613 bytes in 3 files, thumbnails 51 200,
  old temporary files 65 541 in 2 files, a fresh file and another user's old
  file not counted), 4 recent entries, 2 launcher counters, `Timeout=10` from
  `/etc/xdg/kscreenlockerrc`, and NetworkManager values with the right source
  file when `90-local.conf` overrides `80-shadow-privacy.conf`
  (`ccsec-priv-b1.png`, `-b2.png`). GUI: every lock control writes
  `~/.config/kscreenlockerrc`; both history lists cleared (other groups of
  `horizon-usage.conf` kept); trash emptied (`files`/`info` kept), thumbnails
  deleted, old temporary files deleted while the fresh and the foreign file
  stayed (confirmation dialogs `ccsec-priv-c3.png`, `-c6.png`).
* **Privacy → Hide Me** (lab chroot, the program as `nobody` through a stand-in
  `pkexec` that records its arguments or exits 126/127):
  `tests/test_hideme_backend.py` with `tests/harness/cchideme` — 28 tests, all
  passing: the exact command line of every control, no `--persist` from the
  page, the 60 s minimum of the rotation timer, the tool's last stderr line on
  exit 3 and 1, the state record's `error` shown and cleared, pkexec 126 and
  127, the tool not installed, `shadow-harden` failures reported, the state
  record and the timer drop-in read (also through `--dump privacy`), the Tor
  status from `tor@default` through a stand-in `systemctl` (stopped while
  `tor.service` is active and `tor@default` failed, running when
  `tor@default` is active, stopped when systemctl does not answer; also
  through `--dump privacy`), the Network identity values, root
  running the tools without pkexec. Run like the kill switch tests below, with
  `SHADOW_CC_HIDEME_HARNESS=/tmp/cc-harness/cchideme`. Screenshots (Ghost Mode
  on; Ghost Mode whose kill switch was turned off elsewhere, with a recorded
  error; VPN only set in the Network section), dark and light, were reviewed
  during development.
* **Privacy → kill switch and panic** (a Debian trixie lab chroot of the work
  tree; the program ran as the user `nobody`, so every
  privileged call went through a stand-in `pkexec` that records its arguments):
  * `tests/test_killswitch_backend.py` — 33 tests, all passing:
    `--dump killswitch` for every kind of document (protected, on but not
    loaded, off but loaded, the tool's own error, contradictions, invalid
    JSON, wrong types, unknown mode, a file that is too large, a dangling link,
    a directory, escape hatch, mode after a restart from the status / the
    configuration / a broken configuration, live check agreeing, contradicting,
    refused and failing, every panic state, the panic options, Hide Me), and
    `tests/harness/cckillswitch` for the actions (exact `pkexec … set` command
    lines with and without `--runtime` and `--lan`, saving a runtime mode for
    start-up, the local-network switch refused while off, the tool's failure
    message without its warnings, pkexec 126/127, reassert, release, which
    changes close connections, a status file replaced by another program
    while the page is open, root running the tool without pkexec). Three of
    the tests read documents that the **real** `shadow-killswitch`,
    `shadow-panic` and `shadow-security-status` of this tree published in a
    throw-away network namespace with real nftables (kill switch on with panic
    engaged; both off; a table deleted behind the tool's back, which the page
    reports as not protected).
  * GUI in the lab's HORIZON session (KWin 6.3, xdotool): choosing "Tor only"
    opens the confirmation; "Turn on" runs `pkexec … shadow-killswitch set tor
    --no-lan`, the toast and the new state appear, and the live check that
    follows is shown. The panic shortcut row read Ctrl+Alt+Shift+Esc from the
    session's KGlobalAccel (the shell registers `panic`). Screenshots of the
    sections in dark and light theme, and of the error, escape-hatch and
    not-installed states were reviewed during development.
  * Smoke list: 21 of 21 pages without QML warnings (after fixing three
    undefined `Theme` properties in `WifiPage.qml`/`WifiAuditPage.qml`).
  * To run them (build chroot, from the top of the tree; `BUILD` is a build
    directory of the Control Center):

    ```sh
    cmake -S applications/control-center/tests/harness -B /tmp/cc-harness -G Ninja -DSHADOW_SRC="$PWD"
    ninja -C /tmp/cc-harness
    sudo applications/control-center/tests/real-tool-documents.sh /tmp/cc-docs   # optional; root, a throw-away netns
    SHADOW_CC_BIN=$BUILD/bin/shadow-control-center SHADOW_CC_HARNESS=/tmp/cc-harness/cckillswitch \
      SHADOW_CC_CONTRACT_DIR=/tmp/cc-docs PYTHONDONTWRITEBYTECODE=1 \
      python3 applications/control-center/tests/test_killswitch_backend.py -v
    ```

    As root the programs are started as `nobody` (setpriv), so the pkexec
    path is the one tested; without the variables the tests are skipped.

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
* The pages inside a full build with all other pages, and in a real SHADOW
  HORIZON session.
* Kill switch and panic against the real tools on a running system: the
  pkexec prompts of `org.shadow.killswitch.manage` and `org.shadow.panic.release`,
  and `org.shadow.security.read-status` answering without a prompt.
