# Shadow OS Installer (graphical front-end)

`shadow-installer` (application id `org.shadow.installer`, package `shadow-installer-gui`) is the wizard a user
sees in the live session. It makes no decision about disks itself: it collects choices, asks the installation
engine (`../engine`, `/usr/sbin/shadow-installer-engine`, package `shadow-installer`) what those choices mean,
shows the engine's plan word for word, and — after an explicit confirmation — lets the engine carry it out.

## Steps

| # | Step | What it does | Data source |
|---|------|--------------|-------------|
| 1 | Welcome | language of the installed system, searchable, native + English names | `/usr/share/i18n/SUPPORTED` (UTF-8 locales the engine accepts), names from Qt's CLDR data |
| 2 | Keyboard | layout + variant, try-typing field | `/usr/share/X11/xkb/rules/evdev.lst`; session layout from KWin (`org.kde.keyboard /Layouts`) |
| 3 | System check | firmware, Secure Boot, architecture, memory, disks, medium, power, network; blocking vs. warning | engine `probe` (`checks`, `can_install`, `disks`) |
| 4 | Time zone | region list, search, clock preview | `/usr/share/zoneinfo/zone.tab` (or `zone1970.tab`), `iso3166.tab`, zone files via `QTimeZone` |
| 5 | Disk | erase / alongside in free space / manual; LUKS2, separate /home, swap | engine `probe` (`disks[*]`, `free_regions`, `other_systems`) |
| 6 | User | full name, user name, password, host name, auto-login | rules mirrored from `engine/shadow_installer/answers.py` (`validation.cpp`) |
| 7 | Software | optional SHADOW Arsenal profiles; step is hidden when the directory has none | `/usr/share/shadow/arsenal/profiles/*.json` (`id`, `name`, `summary`, `description`, `includes`, `metapackage`, `tools[].package`) |
| 8 | Summary | the engine's steps, what is destroyed, warnings; acknowledgement + typed device name | engine `plan --answers -` |
| 9 | Installation | steps as the engine announces them, percentage only when the engine sends one, elapsed time, collapsible details | engine `install --answers - --confirm <sha256>` (JSON lines) |
| 10 | Finish | success: restart (logind) or keep the live session; failure: engine message, hint, log location, copy log | engine events, exit status; `org.freedesktop.login1.Manager.Reboot` |

Behaviour worth knowing:

* **Nothing touches a disk before the Summary is confirmed.** `probe` only reads. The optional scan for installed
  operating systems (`probe --other-systems`, os-prober, mounts filesystems read-only) is started only from the
  button "Look for installed systems" after a confirmation dialog.
* The **installation medium is never offered** as a target (`is_live_medium`); it is listed under "Not offered"
  with the engine's reason, as are disks the engine calls unusable.
* **"Alongside" is offered only when the engine's rules allow it** (GPT table and unallocated space); "erase" is
  refused for a disk that is mounted or has active mappings; in manual mode `/` is always formatted, UEFI needs
  `/boot/efi`, BIOS needs a boot-loader disk. The engine validates everything again when it plans.
* **Install enables** only with a valid plan, a ticked acknowledgement and — when a step is marked destructive —
  the typed name of the device that loses data (the erased disk, or in manual/alongside mode the first existing
  partition that is formatted, else the target disk).
* **Exit status 3** (plan changed): the wizard returns to the Summary, explains why, and shows the new plan, which
  has to be confirmed again.
* **No cancel** during the installation, and the window refuses to close while the engine runs.
* **Keyboard layout of the session**: "Use in this session" exists only in a live session, with KConfig compiled in
  and KWin answering on the session bus; it writes `kxkbrc`, signals KWin and reports "applied" only after KWin
  itself names the requested layout. Otherwise the page says that typing uses the session's current layout.
* Password hints are facts (length, kinds of characters) and remarks about concrete weaknesses. There is no score.
* The wizard itself is English only (no translations are shipped); the language step configures the installed
  system. Strings go through `qsTr()`/`tr()`.

## Privilege separation

* The GUI runs as the logged-in user. The engine is started with an **argument vector** through
  `/usr/bin/pkexec /usr/sbin/shadow-installer-engine <verb> …` (directly, without pkexec, only when the GUI itself
  already runs as root, which is not a supported way to use it).
* polkit action **`org.shadow.installer.run`** (`polkit/org.shadow.installer.policy`,
  `org.freedesktop.policykit.exec.path = /usr/sbin/shadow-installer-engine`), default `auth_admin` (not kept).
* **`polkit/49-shadow-installer-live.rules`** grants exactly that action without a password to a local, active
  member of group `sudo` *while `/run/shadow/live-session` exists* (checked with
  `polkit.spawn(["/usr/bin/test", "-e", …])`). The marker is created by `system/live/2000-shadow-session` on the
  live medium only. Both files are installed by this component; the package is purged from the installed system
  by the engine (`LIVE_PACKAGES`).
* Outside a live session the destructive phase is **refused in C++** (`Installer::startInstall`) unless the
  program was started with `--allow-installed-system`; three authentication prompts then appear (probe, plan,
  install).

### Secrets

* The user's password and the encryption passphrase go from the text fields into `Secret` (one unshared buffer,
  wiped with `explicit_bzero`) and from there **only into the engine's standard input** (`--answers -`): the
  answer document is assembled as bytes, written, and the buffer wiped. They are never put on a command line, in
  the environment, in a file, in a property readable from QML or in the log shown by the wizard.
* A copy of the answers **without** `user.password` and `storage.passphrase` is kept as a record in
  `$XDG_RUNTIME_DIR/shadow-installer-XXXXXX/answers.json` (directory 0700, file 0600, removed on exit). The engine
  does not read it — see "Engine changes" below.
* After a successful installation the secrets are wiped and the password fields cleared. After a failure they
  are kept so that "Back to the summary" can retry, and wiped when the program exits.
* Limits: Qt keeps its own copies of what is typed (the `TextField` contents, the QString handed to C++, the
  QProcess write buffer); these cannot be wiped from here.

## Failure handling

Every engine call ends in a state with a message: engine not installed / not found; `pkexec` missing; pkexec exit
126 (dismissed / not authorised) and 127 (no agent, wrong password); non-zero exit (the engine's JSON error line
`{event: error, message, hint}` is shown, otherwise the tail of stderr); output that is not the expected JSON;
no answer within 120 s (probe, plan) or 300 s (operating-system scan); death by signal; `install` ending with
exit 0 but without a `done` event. A root-owned engine cannot be signalled by the GUI, so a timed-out call is
abandoned, not waited for.

## Command line

```
shadow-installer [--allow-installed-system] [--profiles-dir DIR]
shadow-installer --dump environment|locales|keyboard|timezones|profiles|probe|rules

testing:
  --engine PATH        engine to run instead of /usr/sbin/shadow-installer-engine ($SHADOW_INSTALLER_ENGINE)
  --probe-file JSON    TEST MODE: synthetic hardware; labelled in the title, a banner and the sidebar;
                       Installer::startInstall() refuses to install
  --step NAME          (test mode) open at welcome|keyboard|check|timezone|disk|user|software|summary|progress|result
  --preset JSON        (test mode) pre-fill the wizard from an answer file
  --simulate SCENARIO  (test mode, with --engine testing/test-engine) let the test engine play an event stream
```

In test mode the engine is called without pkexec and with an extra `--probe-file` argument. The real engine does
not know that option and exits with a usage error before doing anything, so test mode can only plan with
`testing/test-engine`, which runs the engine's real `answers.parse()` and `plan.build()` on the synthetic report
and whose `install` is a timer-driven simulation without a command runner.

`testing/`: `probes/*.json` (generated by `make-probes.py` with the engine's own check code), `presets/*.json`,
`test-engine`, `check-rules.py` (compares `validation.cpp` with `answers.py` through `--dump rules`). Nothing in
`testing/` is installed.

## Run-time dependencies (Debian packages)

`shadow-installer` (engine), `shadow-ui`, Qt 6.8 QML run time as for every SHADOW application
(`qml6-module-qtquick`, `-controls`, `-layouts`, `-templates`, `-window`), `libqt6dbus6`, `libkf6configcore6`,
`pkexec`, `polkitd`, a polkit authentication agent in the session (Shadow Horizon provides one), `tzdata`,
`xkb-data`, `locales`, `systemd` (logind, for "Restart now"), `dbus-user-session`. Optional: `os-prober` (used by
the engine for the operating-system scan), `shadow-arsenal` data in `/usr/share/shadow/arsenal/profiles`.

## Limitations

* The engine names its log `/var/log/shadow-installer/install-*.log` in the live session (root only, mode 0600) and
  copies it to `/var/log/installer/` on the installed system. "Copy log" therefore copies what the engine sent to
  the wizard during this run (events and stderr), not that file.
* Swap and partition sizes are the engine's automatic choice; the wizard does not offer `root_size_gib`,
  `swap_size_gib`, `bootloader.*` or `kernel.extra_cmdline`.
* Manual mode assigns existing partitions only (the engine has no partition editor) and has no encryption.
* Locales that glibc lists without the `.UTF-8` suffix (`hi_IN`, `vi_VN`, …) are rejected by the engine and are
  therefore not selectable; the search says so.
* The user-name check also rejects names of system accounts/groups of the *running* system, as the best
  available picture of the image that gets copied.

## What was tested, and how

All inside the build chroot (no systemd, polkit daemon, KWin live session or disks), final build:

* `SHADOW_JOBS=2 scripts/dev.sh build --dir installer --only installer` — builds without compiler warnings.
* `tests/desktop/smoke.sh --dir installer gui` — 3 passed, 0 failed (plain start; synthetic disk step; synthetic
  summary with a real engine plan). The component name is `gui` because `smoke-tests.list` lives in this directory.
* Every step and state rendered headless at 1024×700 with zero QML warnings (`scripts/dev.sh run --smoke --shot`),
  screenshots `build/work/shots/installer-*.png`: all ten steps; blocked system check; BIOS, arm64 and manual
  disk layouts; light theme; plan-changed (exit 3) return to the summary; progress; success with an engine warning;
  failure; and the failure states `probe-garbage`, `probe-fail`, `plan-garbage`, `plan-fail`, engine missing, real
  engine in test mode, `crash`, `garbage`, `no-done`, `silent-exit`, `fail-early`, `internal`.
* The engine's real planner on synthetic probes (`testing/test-engine plan --text`) for erase+LUKS+/home,
  alongside, manual and BIOS presets.
* A keyboard-only walk through the wizard in a nested KWin session (`scripts/dev.sh nested`, xdotool): Alt+Right,
  Tab, Space, typed names and passwords (mismatch → blocked, match → Next), acknowledgement, typed device name,
  simulated run to the result; "Show details", "Copy log", and the refusal to close while the engine runs.
* The bytes sent to the engine: a recording wrapper around the test engine showed argv
  `plan --answers -` / `install --answers - --confirm <hash>` and a stdin document whose password and passphrase
  (with quotes, backslash, tab, non-ASCII and emoji) were byte-identical to the input; the record file was 0600 in
  a 0700 directory and contained neither.
* The pkexec path as an unprivileged user (`setpriv --reuid=65534 … --dump probe`): real pkexec, no polkit
  daemon → exit 127 → the "could not be obtained" message; and the "engine not installed" message.
* `--dump environment|locales|timezones|keyboard|profiles|probe`; `testing/check-rules.py` (89 inputs, 0
  disagreements; the front-end is stricter for names of existing system accounts and for a trailing line break).
* `polkit/org.shadow.installer.policy` validated with `xmllint --dtdvalid policyconfig-1.dtd`; the logic of the
  rules file evaluated with a stub `polkit` object under Node.js (granted only for this action, local + active +
  sudo + marker present).

## Not tested

* The real engine behind this front-end (`probe`/`plan`/`install` as root on real or virtual hardware), i.e. a
  real installation — it must be exercised in the QEMU installation tests on a built live image.
* pkexec with a running polkitd and agent: the passwordless grant of the rules file in a live session, the
  authentication prompt elsewhere, exit 126 on dismissal (the code path is the same as the tested 127 path).
* "Use in this session" (kxkbrc + KWin reload) and the session-layout read-out in a live session.
* "Restart now" against logind.
* `--allow-installed-system`, and time-outs of 120/300 s (logic only).
* Screen-reader output (roles and names are set; not checked with Orca).
