# SHADOW Updater

System updates for Shadow OS (`shadow-updater`, `org.shadow.updater`): check,
review and install updates through PackageKit, see what changed, find out
whether a restart is needed, and repair the package system after an
interrupted operation. The PackageKit code is shared with SHADOW Store and
lives in `applications/store/pkcommon` (see the README there).

## Pages and data sources

| Page | Source |
| --- | --- |
| Updates | PackageKit `RefreshCache`, `GetUpdates`, `Resolve` (installed versions), `GetDetails` (sizes), `GetUpdateDetail` (changelog, CVE/bug links, restart need). Groups: **security** = PackageKit reports the update with info/severity `security` (the APT back-end derives it from the security archive) — never guessed from names; kernel (`linux-image*`, …) and SHADOW packages (`shadow-*`) are separate groups for display; `blocked` = held back. Install all / security only / selected: `UpdatePackages`, simulated first, removals confirmed. |
| History | `/var/log/apt/history.log*` (every apt, apt-get and PackageKit run), completed with who started a PackageKit run from PackageKit `GetOldTransactions` |
| Last transaction | the log of the last job run from the Updater, kept in `~/.local/state/shadow/updater/last-transaction.log`; "Copy details for a bug report" |
| Recovery | `dpkg --audit`, `/var/lib/dpkg/updates`, `dpkg-query` (half-installed/unconfigured packages) — read-only checks; **Repair** = PackageKit `RepairSystem` (the APT back-end's equivalent of `dpkg --configure -a` and `apt-get -f install`), simulated first, after a confirmation dialog |
| Software sources | `/etc/apt/sources.list`, `/etc/apt/sources.list.d/*.{list,sources}`, signed indexes in `/var/lib/apt/lists` |
| Settings | `~/.config/shadow/updater.conf` (`Updates/AutoCheck`, `Updates/CheckIntervalHours`, last check result) |
| Restart banner | `/run/reboot-required` and `.pkgs`, PackageKit `RequireRestart` of the transactions run here, a newer kernel in `/boot` than the running one; "Restart now" = logind `Reboot` (asked first; `CanReboot` decides whether it is offered) |

Privileges: none of its own. PackageKit and logind authorise through polkit.
There is no pkexec helper.

## Command line

```
shadow-updater [updates|history|log|recovery|sources|settings]
shadow-updater --check
shadow-updater --dump status|updates|update-details|history|reboot|health|sources|repos|settings
```

`--check` (no window, works without a display): refreshes the lists without
ever raising an authentication dialog, prints one JSON object
(`ok`, `updates`, `securityUpdates`, `heldBack`, `downloadSize`, `refreshed`,
`restartRequired`, `error`/`refreshError`) and records the result in the
settings file. Exit code: `0` no updates, `10` updates, `11` security updates,
`1` the check failed. It is meant for a timer in the session; it does not send
a notification itself.

`--dump` topics are read-only (`updates`, `update-details`, `repos`, `history`
and `status` talk to PackageKit: `GetUpdates`, `GetUpdateDetail`,
`GetRepoList`, `GetOldTransactions`; nothing is refreshed).

## Run-time dependencies (Debian packages)

`packagekit`, `libpackagekitqt6-1`, `polkitd` and a polkit agent in the
session, `dpkg`, `apt` (its history log), `systemd` (logind, for the restart),
Qt 6 (`libqt6core6`, `libqt6gui6`, `libqt6qml6`, `libqt6quick6`,
`libqt6dbus6`, `libqt6concurrent6`), the SHADOW UI library.

## Limitations

* The periodic check is not scheduled by this package: something in the
  session (shell module or systemd user timer) has to run
  `shadow-updater --check` when `Updates/AutoCheck` is true and show the
  notification. The window itself refreshes on start when the lists are older
  than the configured interval.
* No offline ("install on restart") updates, no automatic installation.
* Repair is PackageKit's `RepairSystem` only; if `packagekitd` itself cannot
  start, repair from a terminal or the recovery system.
* Changelogs are whatever the APT back-end returns (it downloads them; often
  "not yet available" for fresh security uploads).

## Tested

Build chroot (Debian 13, PackageKit 1.3.1):

* `tests/desktop/smoke.sh --dir store updater` — 6 passed, 0 failed (every page, no system bus → honest "PackageKit service is not reachable").
* `--dump reboot|health|sources|settings|history` without a bus (APT history of the chroot parsed: 6 entries).
* Against a real `packagekitd` in a throw-away overlay (see `applications/store/README.md`, `pkcommon/tools/overlay-tests`):
  * a security update was produced by downgrading `curl`/`libcurl4t64` to the release version inside the overlay: `--check` → exit 11, `securityUpdates: 2`; `--dump updates` → group `security`, installed/new versions, sizes; `--dump update-details`; exit 0 after updating; exit 1 with the APT error when name resolution failed.
  * GUI (Xvfb + xdotool): list with the security group, "Install all" → progress → "Finished successfully", log on the Last-transaction page, History entry "Upgraded 2 · PackageKit · started by root with shadow-updater" (`store-pk-upd-updates.png`, `store-pk-u2.png` … `store-pk-u5.png`).
  * Recovery: `dpkg --unpack` of a package → health "needs repair" with the dpkg audit text → `RepairSystem` → consistent again.
  * `/run/reboot-required` + `.pkgs` → `--dump reboot` and the restart banner (`store-pk-restart.png`; logind absent → "restarting from here is not possible").
* Screenshots without PackageKit: `store-upd-*.png`.

## Not tested

* polkit prompts (PackageKit was driven as root) and `--check` as an ordinary user in a real session.
* `Reboot` through logind; PackageKit `RequireRestart` signals (no update that needs one was available).
* Updates that remove packages (confirmation path was exercised only with the Store's removal), held-back updates, kernel/SHADOW groups with real updates.
* The History page with rotated/compressed logs other than those in the chroot.
