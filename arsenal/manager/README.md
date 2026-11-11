# NEXUS Arsenal manager (`nexus-arsenal`, org.nexus.arsenal)

Browse the Arsenal profiles, see what is installed, install or remove a profile
or a single tool through PackageKit, and find out how to start each tool.

## What it shows and where the data comes from

| Data | Source |
|---|---|
| Profiles, tools, categories, external tools | Manifests in `/usr/share/nexus/arsenal/profiles` (package `nexus-arsenal-data`); `--profiles-dir <dir>` overrides |
| Installed state, installed version, size on disk, priority | `dpkg-query -W -f …` for all manifest packages (one call) |
| Version in the repositories, download size, long description | `apt-cache show --no-all-versions` (one call) |
| "Still to install" size of a profile | `apt-get install --simulate -qq <missing tools>`, then `apt-cache show name=version` of every package APT would install; one profile after the other |
| Architecture | `dpkg --print-architecture` |
| PackageKit availability | `org.freedesktop.DBus.Properties.GetAll` on `org.freedesktop.PackageKit` (system bus) |
| Whether a tool can be started | The desktop entry exists (`XDG_DATA_DIRS/applications/<id>.desktop`); the command resolves in `PATH`, `/usr/sbin`, `/sbin` |
| Acknowledgement | `~/.config/nexus/arsenal.conf`, `[ResponsibleUse]` |

All commands run asynchronously with an argument vector (`Nexus::Command`).
Nothing is cached between runs.

## What it changes, and how

Only through PackageKit (PackageKit-Qt6), which asks for authorisation through
polkit itself; the application has no helper and never runs apt or dpkg to
change anything.

1. *resolve* the package names, 2. *simulate* the transaction, 3. show every
package that would be installed or removed and the download size, 4. after
confirmation run the real transaction with `only-trusted`, showing PackageKit's
status, percentage and current package; "Cancel" calls `Transaction.Cancel`
while PackageKit allows it. Errors show PackageKit's error code and text.

* **Install profile** installs the metapackage `nexus-arsenal-<id>`. If no
  enabled repository offers it, the dialog says so and offers to install the
  profile's missing tools directly.
* **Remove profile** is offered only when the metapackage is installed; it
  removes the metapackage with autoremove, so tools that came with it and that
  nothing else needs go too, and tools installed by hand stay.
* **Remove tool** is not offered for essential packages or packages of
  priority required/important/standard. A removal that would take a `nexus-*`
  package other than `nexus-arsenal-*` with it is refused after the
  simulation. Packages removed along with the requested one are listed in a
  warning.
* Unsigned packages are never installed. Licence agreements, new signing keys
  and media changes are reported as errors, not handled.

Without PackageKit the buttons stay usable: they open a dialog with the D-Bus
error and the exact command (`sudo apt install nexus-arsenal-web`,
`sudo apt install <tools…>`, `sudo apt remove <tool>`,
`sudo apt remove --autoremove nexus-arsenal-web`) and a copy button.

## Starting tools

* GUI: the `Exec` line of the tool's desktop entry is split into an argument
  vector (field codes dropped) and started detached; entries with
  `Terminal=true` are run in NEXUS Terminal.
* CLI: `nexus-terminal --hold -T <name> -e <command> <help arguments…>` — the
  real `-e` option of `applications/terminal` (everything after `-e` is the
  command's argv; `--hold` keeps the tab open). The help arguments come from
  the manifest (`help`, default `--help`). The command line is also shown for
  copying.

## Command line

```
nexus-arsenal [--profiles-dir DIR] [--page overview|tools|external|responsible-use|<profile id>]
nexus-arsenal --dump profiles|tools|state [--profiles-dir DIR]
```

* `--dump profiles`: per profile the counts, metapackage state, size estimate,
  tool package list, external entries, apt command.
* `--dump tools`: every tool with state, versions, sizes, launchability.
* `--dump state`: architecture, installed packages with versions, PackageKit
  availability and error, acknowledgement, manifest errors.

Single instance: a second launch activates the window and honours `--page`.

## Run-time dependencies (Debian packages)

`nexus-ui`, `nexus-arsenal-data`, `dpkg`, `apt`, `libpackagekitqt6-2`,
`libqt6dbus6`; recommended: `packagekit`, `polkitd` + a polkit authentication
agent, `nexus-terminal`, a web browser for the external links (otherwise the
address is copied to the clipboard). Icons: `nexus-themes` (category icons
`nexus-*.svg` in hicolor); when a profile icon is not in the theme the glyph of
the profile's main category is shown.

## Tested (build chroot, amd64)

* `NEXUS_JOBS=2 scripts/dev.sh build --dir arsenal --only manager` — builds
  without compiler warnings.
* `tests/desktop/smoke.sh --dir arsenal manager` — 6 passed, 0 failed (overview,
  a profile, all tools, external, responsible use, and the default directory
  that does not exist in the chroot).
* `--dump state|tools|profiles` with `--profiles-dir /src/arsenal/profiles`:
  12 of 159 tools installed, identical to `dpkg-query`; size estimates equal to
  `tools/profile-size.py`; PackageKit reported unavailable with the D-Bus error.
* Nested KWin session, driven with xdotool: the acknowledgement cannot be
  continued without ticking the box; after ticking, `arsenal.conf` is written
  and a second start does not ask again; `--page`; "Install" without PackageKit
  opens the apt dialog with `sudo apt install nmap`; tool details of an
  installed tool; "Show usage in terminal" opened NEXUS Terminal with
  `whois --help`; "Open" started a desktop entry with `Terminal=true`
  (temporary test manifest pointing at `python3.13.desktop`).
* Dark and light theme screenshots.

## Not tested

* Every PackageKit transaction path (no system bus in the chroot): resolve,
  simulate, confirmation list, progress, cancel, errors, polkit denial, the
  "metapackage unavailable → install tools directly" follow-up, refresh after
  a transaction.
* Starting a real graphical tool from its desktop entry (none is installed in
  the chroot; only the `Terminal=true` path was run).
* Clipboard contents after "Copy" (no clipboard tool in the chroot); opening
  links and documents with a browser/viewer.
* The "not built for this architecture" state (needs an arm64 system).
* Keyboard-only operation and a screen reader were not walked through.
