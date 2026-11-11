# NEXUS Store

Graphical software store of NEXUS OS (`nexus-store`, `org.nexus.store`).
Applications come from the AppStream catalogue, packages and every change to
the system go through PackageKit. Nothing is simulated: without PackageKit the
catalogue can still be browsed, and every action that needs it is disabled with
the D-Bus error shown.

`pkcommon/` in this directory is the PackageKit/APT plumbing shared with
NEXUS Updater (`applications/updater`); it is a static library plus a few QML
components compiled into both applications.

## What it does

| Area | Source |
| --- | --- |
| Categories, application list, search, descriptions, releases, licences, links, content rating | AppStream (`AppStreamQt` 1.0: the distribution's DEP-11 catalogue in `/var/lib/swcatalog`, `/var/lib/apt/lists/*dep11*` and installed metainfo) |
| Screenshots, icons of applications that are not installed | URLs from the AppStream metadata, fetched by `RemoteImage`: **HTTPS only** (redirects may not leave HTTPS), at most 12 MiB, 25 s, PNG/JPEG/WebP/GIF only, at most 16384 px per side, cached under the application cache directory (`QStandardPaths::CacheLocation`/images). No picture is replaced by a stand-in. |
| Package facts (versions, origin repository, architecture, sizes, licence field, description) | PackageKit `Resolve`, `GetDetails` |
| Dependencies | PackageKit `DependsOn` (direct dependencies) and a *simulated* `InstallPackages`/`UpdatePackages` (what would additionally be installed, removed, downloaded) |
| Trust | The same simulation with `only-trusted`: PackageKit's `untrusted` package info / `need-untrusted` exit. The repository list adds what the APT sources say (signed `InRelease`/`Release.gpg` in `/var/lib/apt/lists`, `trusted=yes`, `allow-insecure`). |
| Package search ("All packages") | PackageKit `SearchNames` / `SearchDetails` |
| Installed list | PackageKit `GetPackages(installed)`; read-only fallback `dpkg-query -W` while PackageKit is unreachable (nothing can be removed then) |
| Install / remove / update | PackageKit `InstallPackages`, `RemovePackages`, `UpdatePackages` — always simulated first; removals are always confirmed with the complete list |
| Updates | PackageKit `GetUpdates` (+ `GetUpdateDetail` for changelogs); security updates are the ones PackageKit reports with info `security` |
| Repositories | PackageKit `GetRepoList`, `RepoEnable`; `RefreshCache` |

Transactions (`NexusPk::Job` in `pkcommon/pkjob.*`): simulate → (untrusted
prompt) → (confirmation) → run, with status, percentage, per-package progress,
download remaining/rate, a log that can be copied, and Cancel for as long as
PackageKit reports `AllowCancel`. Licence agreements, repository signing keys
and media changes that PackageKit asks about are shown and answered
(`AcceptEula`, `InstallSignature`). Packages that cannot be authenticated are
refused; they are only installed after the user ticks an explicit
acknowledgement in a warning dialog, and then PackageKit's own polkit policy
(`org.freedesktop.packagekit.package-install-untrusted`) still decides.

Privileges: the store never runs as root and has no helper. PackageKit
authorises through polkit (`org.freedesktop.packagekit.*` actions).

## Command line

```
nexus-store                       overview
nexus-store <AppStream ID | package name | appstream://ID>
nexus-store --search <text>
nexus-store --page overview|installed|updates|repos|<category key>
nexus-store --dump status|categories|installed|repos|updates
nexus-store --dump search <text>         AppStream search + PackageKit SearchNames ("packages")
nexus-store --dump component <ID|package>  AppStream component + PackageKit Resolve/GetDetails/DependsOn/simulation
```

All `--dump` topics are read-only (the PackageKit calls are queries and
simulations; nothing is refreshed, installed or removed).

## Run-time dependencies (Debian packages)

`packagekit` (with its APT back-end), `libpackagekitqt6-1`, `libappstreamqt3`,
`appstream` (its APT hook downloads the DEP-11 catalogue), `polkitd` and a
polkit authentication agent in the session, `dpkg`, `ca-certificates`,
`qt6-image-formats-plugins` (WebP screenshots), Qt 6 (`libqt6core6`,
`libqt6gui6`, `libqt6qml6`, `libqt6quick6`, `libqt6dbus6`, `libqt6network6`,
`libqt6concurrent6`), the NEXUS UI library. Optional: `nexus-updater`
(“Open NEXUS Updater”, repair).

## Limitations

* Only DEB packages through PackageKit; no Flatpak/Snap, no ratings or reviews.
* No adding/removing of repositories or keys, only enable/disable of the ones
  PackageKit lists.
* Local `.deb` files are not handled.
* `GetUpdateDetail` of the APT back-end downloads the changelog and can take
  long when the changelog server is slow; requests still running are cancelled
  when a transaction is started.
* While PackageKit runs an exclusive transaction, queries of the pages wait in
  its queue ("Asking PackageKit…").

## Tested

Build chroot (Debian 13, PackageKit 1.3.1, PackageKit-Qt 1.1.2, AppStream 1.0.5):

* `NEXUS_JOBS=2 scripts/dev.sh build --dir store --only "store;updater"` — builds without warnings.
* `tests/desktop/smoke.sh --dir store store` — 9 passed, 0 failed (no QML warnings); no system bus in this mode: all PackageKit states show "not reachable".
* `--dump status|categories|search|component|repos|updates|installed` without a system bus (AppStream catalogue: 2592 components, 2104 applications) — and against a real `packagekitd`:

**Against real PackageKit.** `pkcommon/tools/overlay-tests/run.sh` starts a
private system bus, `polkitd` and `packagekitd` inside one `dev.sh exec`
invocation, after mounting throw-away overlays over `/usr`, `/etc` and `/var`
(private mount namespace), so the shared chroot is never modified. Copy the
directory to `/tmp/store-pktest` in the chroot and run
`scripts/dev.sh exec --dir store -- /tmp/store-pktest/run.sh <script>`.
Results seen:

* all `--dump` topics of both applications (`dumps.sh`): repositories matched to APT sources and "signed", `SearchNames editor` → 101 packages, `component kate` → candidate, sizes, 51 direct dependencies, simulation "74 to install, 43.9 MiB, trusted", installed list from PackageKit (1002 packages).
* with `nexus-pkcommon-cli` (the same `Job` code; `tx.sh`, `tx2.sh`): remove with the question declined → cancelled, nothing changed; install `hello` → installed; remove `--yes` → removed; install `kate` cancelled during download → cancelled, not installed; `RefreshCache`; `RepoEnable` off/on (sources file rewritten by PackageKit); unsigned local repository → package reported untrusted, install refused when declined, installed only with the explicit answer.
* in the GUI under Xvfb with `xdotool` (`gui2.sh`): Installed → Remove → confirmation dialog → removed; application page → Install → Cancel during download → "Cancelled"; untrusted-package dialog; "All packages" search; updates list with security updates; repositories page with an unsigned third-party source.

Screenshots in `build/work/shots/`: `store-st-*.png` (no PackageKit),
`store-pk-store-*.png`, `store-pk-i2.png` (removal confirmation),
`store-pk-j2.png` (finished), `store-pk-c2.png` (cancelled),
`store-pk-g2.png` (untrusted prompt), `store-pk-g3.png` (repositories),
`store-pk-b2.png` (dependencies), `store-pk-s1.png` (package search).

## Not tested

* polkit authentication dialogs: `packagekitd` was driven as root, which PackageKit does not ask polkit about. Behaviour as an ordinary user with a polkit agent needs the VM.
* Screenshot/icon download: the chroot could not resolve `appstream.debian.org`; only the failure state was seen. HTTPS-only and size limits are implemented but were not observed against a server.
* Licence-agreement, signing-key and media-change prompts (APT repositories with these were not available).
* Multi-arch (foreign architecture) packages.
