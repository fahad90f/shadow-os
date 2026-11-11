# SHADOW Security Center

`shadow-security-center` (id `org.shadow.securitycenter`) shows the real security
state of the machine and manages the host firewall. It is a GUI over two
existing root tools and has no security logic of its own:

| Tool | Used for | polkit action |
|---|---|---|
| `/usr/sbin/shadow-security-status [--section a,b]` (`security/audit/`) | every state shown on every page — one JSON report | `org.shadow.security.read-status` (`allow_active=yes`: no prompt in an active local session) |
| `/usr/sbin/shadow-firewall enable\|disable\|allow\|remove\|reload\|reset` (`security/firewall/`) | every change on the Firewall page | `org.shadow.firewall.manage` (`auth_admin_keep`) |

Both are started as `pkexec <tool> <arguments…>` with an argument vector
(`Shadow::Command`, never a shell), always asynchronously. The GUI itself never
runs as root. There is **no score, percentage or "threat level"**: a tile or
badge repeats one field of the report, and the evidence text next to it is
built from the neighbouring fields. A value the tool reports as `null` is
shown as "Not determined" with the reason; a section with
`"available": false` shows the tool's `reason`.

## Pages

| Page (`--page`) | Report section(s) | Shows |
|---|---|---|
| `overview` | all | six tiles (firewall, AppArmor, disk encryption, Secure Boot, updates, network exposure) with state, evidence and link; the tool's `recommendations`; the sections that could not be checked and why; **Copy report** (text/JSON), **Save…**, **Refresh** |
| `firewall` | `firewall` (+ `listening`) | active?, starts at boot, default input/forward policy, rule count, foreign nftables tables; opened ports with add (validated) / close; Enable… and Disable… with warnings, Close all… with confirmation, Reload; ruleset viewer (see "Additions requested") |
| `apparmor` | `apparmor` | module state, LSM list, `apparmor.service`, profile counts by mode, profile list (enforce/complain names), number of confined processes |
| `encryption` | `encryption` | `/`, `/home`, swap: on LUKS or not; LUKS devices (version, cipher, PBKDF, unlocked); live-session notice |
| `secureboot` | `secureboot`, `hardening.kernel/tainted` | firmware mode, Secure Boot, setup mode, shim, MOK count, boot-loader refresh failure, running kernel, lockdown, taint |
| `updates` | `updates` | security/total pending counts, age of the package lists, restart required, pending packages; **Open SHADOW Updater** only when `shadow-updater` is in `PATH` |
| `hardening` | `hardening` | sysctl baseline: expected vs actual; boot parameters present/missing; CPU vulnerability status |
| `network` | `listening`, `connections` (+ `firewall`) | listening sockets with bind address, port, process, PID, scope — sockets on all interfaces highlighted; filter "reachable from the network"; active connections |
| `services` | `services` | failed units, running services (filter) |
| `logins` | `logins`, `audit` | sessions, failed authentication attempts and logins of the last 7 days, auditd state, rules, lost events, event counts per rule key |

Refresh: a complete report at start and on **Refresh**; a timer re-reads the
cheap sections (`firewall,listening,connections,services`) every 30 s while
the window is visible. The expensive sections (`apt-get -s`, a week of
journal, `aureport`) only run on a complete refresh, so the recommendations
are as old as the last complete report (its time is in the Overview
subtitle; every other page shows "Checked hh:mm:ss" for its own section).
After a firewall change the cheap sections are re-read at once, then a
complete report follows.

Export: "Copy report" puts the report on the clipboard as text or as JSON;
"Save…" writes it to a file (mode 0600 — it lists listening services and
login activity). The JSON is the tool's report unchanged; the text form is a
loss-free rendering of the same data.

## Command line

```
shadow-security-center [page] [--page <name>] [--tools-dir <dir>] [--dump <topic>]
```

* pages: `overview firewall apparmor encryption secureboot updates network services logins hardening`
  (single instance: a second launch switches the running window to the page)
* `--dump` topics: every section name (`firewall apparmor encryption secureboot updates listening
  connections services logins audit hardening recommendations`), `report` (everything),
  `report-text` (the text export), `tools` (which tools would be run, and how). A dump runs
  the status tool the same way the UI does (pkexec unless overridden); it fails with the
  reason on stderr and exit code 2 when the tool cannot be run.

### Developer override (build chroot, tests)

The chroot has no polkit, and one is root there. The tools can be run
directly, without pkexec:

* `--tools-dir <dir>` or `SHADOW_SECURITY_TOOLS_DIR=<dir>` — `<dir>` is either flat
  (`<dir>/shadow-security-status`, `<dir>/shadow-firewall`) or the repository layout
  (`--tools-dir /src/security` → `audit/shadow-security-status`, `firewall/shadow-firewall`)
* `SHADOW_SECURITY_STATUS_TOOL=<path>`, `SHADOW_FIREWALL_TOOL=<path>` — one tool each

In this mode the directory of the firewall tool is put first in `PATH`
(the status tool calls `shadow-firewall status --json`). No privilege is
gained: the tools run as the calling user and refuse to act unless that is
root.

## Run-time dependencies (Debian packages)

* `shadow-hardening` — ships both tools, the polkit policy, `/etc/nftables.conf`
  (its own dependencies: `python3`, `nftables`, `iproute2`, `util-linux`,
  `cryptsetup-bin`, `apt`, `systemd`; optional `auditd`, `mokutil`, `apparmor`)
* `pkexec`, `polkitd` and a polkit authentication agent in the session
* `shadow-ui` libraries/QML (`org.shadow.ui`, `org.shadow.style`), Qt 6 (`qml6-module-qtquick*`)
* optional: `shadow-updater` (the "Open SHADOW Updater" button)

## Additions requested from the tools

The tools are outside this directory and were not changed. Where the report
lacks something a page was asked to show, the page shows what exists and
says in one line what is not reported. Each view below is already
implemented and appears as soon as the field exists (rendered in the tests
through `tests/fixture-proposed` and `tests/status-with-ruleset`):

| Wanted | Exact addition to `shadow-security-status` | Today |
|---|---|---|
| Active ruleset viewer | `firewall.ruleset`: string, output of `nft list table inet shadow_filter` when the table is loaded (read-only, so it fits `org.shadow.security.read-status`; alternatively the same key in `shadow-firewall status --json`, which the section embeds) | rule count and policies; hint `sudo nft list table inet shadow_filter` |
| All profiles with mode | `apparmor.profile_names` for every mode (`kill`, `unconfined`, other), not only `enforce`/`complain` | names for enforce/complain; the rest counted only |
| Confined / unconfined processes | `apparmor.processes`: `[{"process","pid","profile","mode"}]` (from `/proc/<pid>/attr/current`; `profile: null` = unconfined) | only the number `confined_processes` |
| Mounted file systems on encrypted storage | `encryption.filesystems`: `[{"mountpoint","device","fstype","encrypted": true/false/null}]` for every real mount | only `/`, `/home`, swap |
| Running kernel signed? | `secureboot.kernel_image` (path) and `secureboot.kernel_signed` (bool/null, e.g. from `sbverify --list`); also report `kernel_lockdown` on BIOS systems (the lockdown file exists there too) | kernel release and taint from `hardening`; lockdown only on UEFI |
| Last update time | `updates.last_upgrade`: ISO time of the last dpkg/apt transaction (`/var/log/apt/history.log` or mtime of `/var/lib/dpkg/status`) | only `last_refresh` of the package lists |
| Recent audit events | `audit.recent_events`: `[{"time","key","type","summary"}]` (e.g. the last 50 from `ausearch -ts recent`) | counts per rule key |
| Module hardening | `hardening.modules`: `[{"module","expected","actual","ok"}]` for the modprobe.d blacklist of the baseline | sysctl, boot parameters, CPU mitigations |

Two more observations about the tools (not worked around, shown as reported):
`firewall.rule_count` does not change when ports are opened (they are set
elements, not rules); `hardening.mitigations[].vulnerable` only looks at the
start of the status string, so "…; BHI: Vulnerable" counts as mitigated.

## Limitations

* No polkit in the build chroot: the successful `pkexec` path (no prompt for
  the report, admin prompt for the firewall) has not been run. What was run
  is the pkexec invocation up to polkit's refusal, and every operation in
  direct mode.
* The report is as complete as the tool: see the table above.
* "Save…" takes a path typed into a field (default: Documents or home); there
  is no file browser.
* Recommendations are recomputed only with a complete refresh.
* The clipboard content could only be checked by pasting it back (no
  `wl-paste` in the chroot).
* Strings are English with `qsTr()`; no translation catalogue yet.

## Tests

Files in `tests/` (none is installed):

* `isolated.sh <cmd>` — runs a command in a new network namespace and a mount
  namespace with a private tmpfs copy of `/etc`, so `shadow-firewall` can be
  exercised for real without touching the firewall or configuration of the
  machine or the chroot.
* `gui-firewall.sh`, `gui-export.sh` — xdotool scripts for the nested session.
* `status-with-ruleset` — the real status tool plus the proposed
  `firewall.ruleset` field (real `nft` output).
* `fixture/`, `fixture-proposed/` — a fixed, invented report of a populated
  machine (UEFI, LUKS, AppArmor, systemd …) and a firewall tool that refuses
  everything; used only to render the branches the chroot cannot produce and
  the failure path. Not real data, never used by the application itself.

What was run (build chroot, as root; `seccenter` build directory):

1. `SHADOW_JOBS=2 scripts/dev.sh build --dir seccenter --only security-center` — builds.
2. `tests/desktop/smoke.sh --dir seccenter security-center` — all lines of
   `smoke-tests.list` pass with zero QML warnings: no tools (honest "not
   installed"), the real tools from `/src/security` on every page, both fixtures.
   The smoke hook exits right after the first frame, usually before the
   status tool has answered, so it proves the loading/unavailable states.
   The loaded states were checked separately: every page was started with
   `scripts/dev.sh run --shot … --delay 4000…5000` against `/src/security`,
   `tests/fixture` and `tests/fixture-proposed`; that mode prints QML
   warnings too, and none appeared (screenshots `build/work/shots/seccenter-*.png`).
3. `--dump tools|firewall|report-text` in default and `--tools-dir /src/security`
   mode; `--dump firewall` equals `shadow-firewall status --json` (+`available`).
4. pkexec path: inside `tests/isolated.sh`, with the two tools bound into
   `/usr/sbin`, `--dump firewall` started `pkexec /usr/sbin/shadow-security-status
   --section firewall`; pkexec failed (no polkitd in the chroot) and the
   application reported "Authorisation failed … Error getting authority".
5. Firewall round trip through the GUI (`scripts/dev.sh nested … gui-firewall.sh`,
   app inside `tests/isolated.sh`, real `nft`): Enable… → confirm, open `22/tcp`
   "SSH", rejected input `70000`, open `6000-6010/udp`, Reload, close the range
   with its trash button, Close all… → confirm, Disable… → confirm. After every
   step `shadow-firewall status --json`, run in the same namespace, showed the
   expected state (log `build/work/shots/nested-logs/seccenter/gui-firewall.log`);
   the chroot's `/etc` and the host ruleset were unchanged afterwards.
6. Failure path through the GUI: with `tests/fixture`, Disable… → confirm shows
   the tool's stderr in the "The firewall was not changed" banner and a toast.
7. Navigation and export through the GUI (`gui-export.sh`): tile link, sidebar
   pages, Copy as text (toast; clipboard pasted back), Save as JSON and as text
   (files written with mode 0600 and checked), "exists and will be replaced".
8. The 30 s timer: the Services page's "Checked" time advanced by 30 s during
   the nested run without any click.

Not tested: a real polkit session (prompt, cancel, `auth_admin_keep`), a
booted Shadow OS with systemd/AppArmor/LUKS/UEFI (those pages were only
rendered from the fixture), `Open SHADOW Updater` (the updater is not in this
build directory), keyboard-only operation beyond Tab order by construction,
screen readers.
