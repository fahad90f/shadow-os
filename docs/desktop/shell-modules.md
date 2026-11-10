# NEXUS HORIZON shell modules

`horizon-shell` is one process. Its core (wallpaper, panel, dock, launcher,
session menu, polkit agent) lives in `desktop/horizon-shell/src` and `qml`.
Everything else — notifications, the system tray, quick settings, the
calendar, on-screen displays — is a **module** in
`desktop/horizon-shell/modules/<name>/`. A module is compiled into the same
binary but the core does not know it: removing the directory removes the
feature and nothing else.

```
modules/<name>/
    *.cpp, *.h          C++ back-end (compiled into horizon-shell, AUTOMOC)
    qml/*.qml           QML; becomes part of the org.nexus.horizon QML module
    module.cmake        optional: find_package(), target_link_libraries(horizon-shell PRIVATE ...)
    smoke-tests.list    optional, see tests/desktop/smoke.sh
    README.md           what it does, D-Bus names it owns, what was tested
```

Rules that keep modules independent:

* A module only edits files inside its own directory.
* QML file names are global (one QML module): prefix them with the module's
  subject, e.g. `NotificationPopups.qml`, `TrayItem.qml`.
* Context-property names are global too; use the module name
  (`notifications`, `tray`, `quickSettings`).
* The module directory is on the include path, so `#include "server.h"` works.
  Header names must therefore not clash with core headers in `src/`.

## Registering

```cpp
#include "module.h"

static void init(Horizon::ShellContext &ctx)
{
    auto *server = new NotificationServer(ctx.parent());     // lives as long as the shell
    ctx.expose(QStringLiteral("notifications"), server);     // context property for all shell QML
    ctx.addPanelItem(QStringLiteral("NotificationButton.qml"), 40);
    ctx.addSurface(QStringLiteral("NotificationPopups.qml"));
    ctx.addSurface(QStringLiteral("NotificationCenter.qml"));
}
HORIZON_SHELL_MODULE(notifications, init)
```

`ShellContext` also gives access to the core services: `app`, `settings`,
`applications`, `windows`, `workspaces`, `session`, `shortcuts`.

`init` runs once, on the GUI thread, before any QML is loaded. It must not
block (no synchronous D-Bus calls that can time out) and must cope with a
session where its service is missing: the headless smoke test runs the shell
without a compositor, a system bus or a login session.

### Panel items

`addPanelItem(file, order)` puts an item on the right side of the panel, left
of the session button; lower `order` is further left.

| order | item |
|-------|------|
| 10 | system tray |
| 30 | status indicators / quick settings |
| 40 | notifications |

The component is created with two initial properties, which it should declare
as `required property var shell` and `required property var panel`. Use
`PanelButton` (icon, text, `checked`, `tooltip`, `badge`, `interactive`) so
items look and behave alike; its height is the panel height.

### Surfaces

`addSurface(file)` instantiates a top-level component once at start-up,
with the initial property `shell`. Base types:

* `ShellSurface` — a layer-shell window (`layer`, `anchorTop/Bottom/Left/Right`,
  `exclusiveZone`, `keyboard`, `scope`). For passive things such as
  notification popups or an on-screen display. Only as large as its content,
  so it does not swallow input elsewhere.
* `OverlaySurface` — full-screen, takes the keyboard, closes on Escape or a
  click outside its content (`open`, `show()`, `hide()`, `modal`, `scrimOpacity`).
* `PopupSurface` — an `OverlaySurface` whose content is a card hanging under
  the panel (`alignment: "left" | "center" | "right"`, `popupWidth`).
* `BlurBehind` — put it inside a translucent background; use its `glass`
  property to decide between `Theme.glassPopup` and an opaque colour.

### Overlays and actions

Only one overlay is open at a time. The shell root object manages that:

```qml
PopupSurface {
    id: popup
    required property var shell
    Component.onCompleted: shell.registerOverlay("quick-settings", popup)
}
```

* `shell.registerOverlay(name, overlay)` — makes the overlay openable by name.
* `shell.trigger(name)` — toggles the overlay registered under `name`, or
  runs the core action of that name. Global shortcuts end up here too, so
  registering an overlay as `quick-settings`, `notifications` or `calendar`
  is all that is needed for Meta+A, Meta+N and the clock button.
* `shell.openOverlayName` — name of the overlay that is open (for a panel
  button's `checked`).
* `shell.present(overlay)`, `shell.toggle(overlay)`, `shell.closeOverlays()`.
* `signal shell.unhandledAction(name)` — shortcut actions that are neither a
  core action nor a registered overlay: `volume-up`, `volume-down`,
  `volume-mute`, `mic-mute`, `brightness-up`, `brightness-down`,
  `media-play`, `media-next`, `media-previous`.
* `shell.notify(message)` — a short message from the shell itself.

A panel button that opens something must only exist (or be `interactive`)
when the thing it opens exists. The clock, for example, is a button only
while an overlay named `calendar` is registered.

## Diagnostics

A module can add `horizon-shell --dump <topic>` output (JSON, no compositor
needed) for tests and bug reports:

```cpp
static QJsonObject dumpTray() { ... }
HORIZON_SHELL_DUMP(tray, "tray", dumpTray)
```

`horizon-shell --dump modules` lists the modules compiled in.

## Testing a module

```
scripts/dev.sh build --dir <yourdir> --only horizon-shell --modules <name>
tests/desktop/smoke.sh --dir <yourdir> horizon-shell
scripts/dev.sh nested --dir <yourdir> --shot result.png -- 'horizon-shell & sleep 6; <clients>; xdo ...; shot step1.png'
```

`--modules` builds only the named module(s) into the shell, so unfinished
work in other module directories cannot break your build.

The nested session is a real KWin Wayland compositor inside a virtual X
server: layer-shell surfaces, global shortcuts and the session D-Bus work;
logind, the system bus services and hardware do not exist there. What cannot
be exercised there has to be tested in the VM image and said so in the
module's README.
