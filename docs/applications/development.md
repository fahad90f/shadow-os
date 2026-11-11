# Developing SHADOW applications

This is the contract every SHADOW application follows. The smallest complete
example is `applications/sysinfo/` — read it alongside this page.

## 1. Anatomy of an application

```
applications/<name>/
├── CMakeLists.txt             one shadow_add_app() call (+ helpers, policies)
├── main.cpp                   Shadow::App setup, --dump topics
├── <backend>.{h,cpp}          C++ back-ends, QML_ELEMENT types
├── Main.qml (+ other .qml)    UI, built only from ShadowUI components
├── org.shadow.<name>.desktop   launcher (Icon=org.shadow.<name>)
├── smoke-tests.list           binaries the GUI smoke test launches
└── README.md                  purpose, data sources, runtime deps, limits
```

Components are **discovered**: any directory `applications/*/` (also
`desktop/*/`, `system/installer`, `system/recovery`, `arsenal/manager`) with a
`CMakeLists.txt` is built. Never edit the top-level `CMakeLists.txt`, `cmake/`,
`desktop/ui/` or `debian/` to add an app.

```cmake
shadow_add_app(shadow-<name>
    URI org.shadow.<name>
    SOURCES main.cpp backend.cpp backend.h
    QML Main.qml pages/Foo.qml
    DESKTOP org.shadow.<name>.desktop
    LIBS Qt6::Network            # extra link libraries, if any
)
```

`main.cpp` always goes through `Shadow::App` (`desktop/ui/src/app/shadowapp.h`),
which applies the SHADOW style, registers the icon providers, handles
`--help/--version/--dump`, single-instance activation and the test hooks.

## 2. Rules

1. **Real data only.** Every number, name and state on screen comes from the
   system at run time. No sample rows, no hard-coded values, no invented
   scores. If something cannot be determined, say so (`NxEmptyState`,
   `NxBanner`, "—") and say *why* (service not running, tool missing,
   permission denied).
2. **Every control works.** A button that cannot do its job yet does not ship.
   Disable a control only for a real, explained reason (tooltip or caption).
3. **Never block the UI thread** on I/O that can take more than a few
   milliseconds. Use `Shadow::Command::run()` (async), D-Bus async calls, or a
   worker (`QtConcurrent`). Reading small `/proc` and `/sys` files is fine.
4. **No shell.** External tools are run with an argument vector
   (`Shadow::Command`), never through `sh -c` with interpolated strings.
5. **Parse stable interfaces.** Prefer D-Bus APIs and machine-readable output
   (`ip -j`, `lsblk -J`, `nmcli -t`, `systemctl --output=json`, `nft -j`) over
   scraping human-oriented text. Tools are run with `LC_ALL=C.UTF-8`.
6. **Degrade honestly.** The build chroot has no systemd, NetworkManager,
   polkit agent or hardware. The app must start there without QML warnings and
   show accurate "unavailable" states. Behaviour against real services is
   verified later in the QEMU integration tests through `--dump`.
7. **Tokens only.** Colours, sizes, radii, fonts and durations come from
   `Theme` (generated from `desktop/themes/tokens/shadow-tokens.json`). No
   literal colours or pixel font sizes in app QML.
8. **Accessible and keyboard-usable.** Icon-only buttons carry a tooltip
   (`NxIconButton.tooltip`), custom interactive items set `Accessible.name`
   and are reachable with Tab; destructive actions ask first
   (`NxConfirmDialog`, with `requiredPhrase` for irreversible ones).
9. **User-visible strings** go through `qsTr()` / `tr()`.
10. **Licence header** on every source file: `SPDX-License-Identifier: GPL-3.0-or-later`.

## 3. Diagnostic mode: `--dump <topic>`

Every application exposes the data behind its UI as JSON without opening a
window:

```
shadow-sysinfo --dump system
```

Declare topics with `app.setDumpTopics({...})`, handle them after
`app.processArguments()` and return `app.finishDump(json)`. Asynchronous
back-ends use `app.waitFor(obj, SIGNAL(ready()))`. Dump mode forces the
`offscreen` Qt platform, so it works over SSH and the serial console — this is
how `tests/integration/` checks each back-end against the system's own tools
inside a booted VM. Topics should cover every data set the UI displays.

## 4. Privileged operations

Order of preference:

1. **An existing system service with its own polkit policy**: systemd
   (`org.freedesktop.systemd1`), `timedate1`, `locale1`, `hostname1`, `login1`,
   NetworkManager, UDisks2, PackageKit, AccountsService, UPower. The user is
   prompted by the session's polkit agent; the app needs no extra privilege.
2. **A SHADOW helper run through `pkexec`**, when no service covers the task:

   * installed to `${SHADOW_LIBEXEC_DIR}` → `/usr/libexec/shadow/<name>-helper`
   * one polkit action per helper in
     `/usr/share/polkit-1/actions/org.shadow.<name>.policy`
     (`org.freedesktop.policykit.exec.path` annotation; `auth_admin` for
     anything that changes the system; read-only status helpers may use
     `allow_active=yes`)
   * the helper accepts a **closed set of sub-commands**, validates every
     argument against an allow-list or strict pattern, never passes input to a
     shell, and prints one JSON document on stdout
   * Python 3 (standard library only) or C++; no third-party modules

Never run the GUI itself as root, never store or log passwords, never weaken
a security mechanism to make something work.

## 5. Build, run, test

Everything is compiled inside the Debian build chroot
(`build/chroot/create.sh` once), never against host libraries:

```bash
sudo scripts/dev.sh build --dir <name> --only <name>      # configure + build one component
sudo scripts/dev.sh run   --dir <name> --smoke --shot <name>.png shadow-<name>
sudo scripts/dev.sh run   --dir <name> shadow-<name> --dump <topic>
sudo scripts/dev.sh run   --dir <name> --light --accent violet --shot <name>-light.png shadow-<name>
```

* `--dir` selects the build tree `/build/<name>` inside the chroot; use a
  different name per component so builds never collide.
* `--smoke` fails (exit 2) if the QML engine emitted any warning or error.
* `--shot` saves a screenshot of the main window to `build/work/shots/`.
* `SHADOW_SCREENSHOT_DELAY=<ms>` / `--delay` gives slow back-ends time to load.

`smoke-tests.list` lists, one per line, the commands
`tests/desktop/smoke.sh` runs for this component (`<binary> [args...]`).

## 6. README.md of an application

State, concretely: what the app does; where each kind of data comes from
(files, commands, D-Bus interfaces); **runtime dependencies** as Debian
package names; privileged operations and their polkit actions; known
limitations and anything not yet verified against a real service.

## 7. Control Center pages

`applications/control-center/` is one application made of independent pages.
A page is `pages/<Name>Page.qml` (root item: `NxPage`) plus, when it needs
system access, a back-end `backends/<name>backend.{h,cpp}` that is a
`QML_ELEMENT` of `org.shadow.controlcenter` and is instantiated by the page.
Both directories are globbed by CMake; the page is listed in the `pages`
table in `Main.qml` (key, title, icon, section, source, keywords).

* Register the page's data for diagnostics:
  `SHADOW_CC_DUMP("<key>", [] { return QJsonValue(Backend().toJson()); })`.
* Reach window services through the attached property, not by id:
  `ApplicationWindow.window.toaster.show(...)`.
* Helpers and polkit policies go to `helpers/` (`*-helper`, `*.policy`); they
  are installed automatically.
* `shadow-control-center <key>` opens a page directly; other applications link
  to settings this way.
