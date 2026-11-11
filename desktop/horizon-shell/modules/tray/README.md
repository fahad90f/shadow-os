# horizon-shell module: tray

The system tray: a host for StatusNotifierItems (the protocol of KDE, Ayatana/
libappindicator, Electron, Qt's `QSystemTrayIcon` on Wayland) with the items' menus.

**XEmbed tray icons (the legacy X11 "system tray" protocol) are not supported**, and
there is no XEmbed→StatusNotifierItem bridge in this module.

## What it does

* **Watcher** — `org.kde.StatusNotifierWatcher` at `/StatusNotifierWatcher`
  (`RegisterStatusNotifierItem`, `RegisterStatusNotifierHost`,
  `RegisteredStatusNotifierItems`, `IsStatusNotifierHostRegistered`, `ProtocolVersion` 0,
  the four signals). The name is requested without replacement and without queueing:
  if another watcher exists the shell registers as a host of it
  (`org.kde.StatusNotifierHost-<pid>`) and shows its items; when that watcher leaves,
  the shell requests the name again, so items see the name come back and register anew.
  Items may register with a bus name or with an object path.
* **Items** (`trayitem.*`): `Id Title Category Status IconName IconThemePath IconPixmap
  OverlayIconName OverlayIconPixmap AttentionIconName AttentionIconPixmap ToolTip Menu
  ItemIsMenu`, read with one asynchronous `Properties.GetAll` (single `Get`s for items
  whose `GetAll` fails) and again on `NewTitle NewIcon NewAttentionIcon NewOverlayIcon
  NewToolTip NewStatus NewMenu NewIconThemePath` — at most once per 150 ms per item.
  Calls `Activate`, `SecondaryActivate`, `ContextMenu`, `Scroll` and
  `ProvideXdgActivationToken`, all asynchronous. An item disappears when its service
  leaves the bus or when it never answers.
* **Menus** (`traymenu.*`): `com.canonical.dbusmenu` — `AboutToShow`, `GetLayout`,
  `Event` (`opened`, `closed`, `clicked`), `LayoutUpdated`, `ItemsPropertiesUpdated`;
  properties `label` (mnemonic underscores removed), `enabled`, `visible`, `type`
  (separator), `icon-name`, `icon-data`, `toggle-type`, `toggle-state`,
  `children-display`. Read when a menu is opened and kept current while it is open.
* **Panel** (`TrayArea.qml`, `TrayIcon.qml`, order 10): one icon per item that is not
  `Passive`; no space is taken while there is none. Left click: `Activate` (or the menu,
  if the item is only a menu or answers `Activate` with an error); middle click:
  `SecondaryActivate`; right click: the menu (or `ContextMenu` if it exports none);
  wheel: `Scroll`. `NeedsAttention` shows the attention icon, a dot and a pulse.
  Reachable with Tab; Return/Space and the Menu key work.
* **Menu overlay** (`TrayMenuOverlay.qml`): an `OverlaySurface` drawing the menu in the
  SHADOW style under the icon — check marks, radio dots, icons, separators, disabled
  entries, submenus as further cards that keep to one side and stay on the screen, a
  scrolling list when a menu is taller than the screen, "asking the application / does
  not answer / empty" states. Keyboard: Up/Down/Home/End, Right or Return opens a
  submenu, Left or Escape leaves it, Return/Space chooses, Escape or a click elsewhere
  closes. The position comes from the icon's place in the panel, which spans the screen
  from its left edge (a layer surface cannot ask where it is).
* **Tool tip** (`TrayToolTip.qml`): a small surface of its own under the icon (the
  panel is only as high as its buttons), with the item's tool tip title and text as
  plain text.

## QML API: context property `tray`

A list model (role `trayItem`: the item object with `itemId title status shown
needsAttention iconSource hasIcon toolTipTitle toolTipText hasMenu menu itemIsMenu` and
`activate/secondaryActivate/contextMenu(x, y, window)`, `scroll(dx, dy)`), plus
`available`, `watcher` (`own`, `foreign`, `none`), `shownCount`, `menuItem`,
`showMenu(item, x)`, `showToolTip(item, x)`, `hideToolTip()`.

Icons are served by the image provider `image://nxtray/...`, registered with
`Shadow::App::engine()`; a symbolic (single grey tone) icon that would be invisible on
the panel is recoloured with the text colour.

## Untrusted input — limits (`trayutil.h`)

64 items, 16 per client; bus names and object paths checked for syntax; texts single
line, without control or bidirectional-override characters (id/title ≤ 128, tool tip ≤
480 characters in ≤ 6 lines, markup removed); pixmaps > 1024 px ignored, the rest
kept at ≤ 128 px (8 sizes); icon files ≤ 2 MiB and ≤ 2048 px; `IconThemePath` only
probed for a bounded set of file names; menus ≤ 6 levels, ≤ 150 entries per level,
≤ 600 in total, labels ≤ 160 characters, `icon-data` PNG only, ≤ 256 KiB, ≤ 512 px;
menu entries may name theme icons but not files; calls time out (3–5 s).

## Diagnostics

`horizon-shell --dump tray` runs beside the shell as an observer: it lists the items
of the session's watcher and reads each item and its menu with the tray's own code,
without registering anything and without sending `AboutToShow`. (`iconFound` refers to
the dumping process, whose icon theme can differ from the shell's.)

## Run-time dependencies (Debian packages)

Those of the shell itself (`libqt6dbus6`, `libqt6svg6`, `libkf6windowsystem6`) and an
icon theme (`hicolor-icon-theme`; `breeze-icon-theme` for the common names). Building
the test client needs `libkf6statusnotifieritem-dev`; the test scripts `python3-gi`.

## Tests (`tests/`, never part of the product build)

* `traytest` (KStatusNotifierItem, own CMake project): main item (theme icon, tool tip
  with markup, menu with icon entries, a checkable entry, a radio group, separators, a
  disabled entry, an entry that changes while the menu is open, two levels of
  submenus), pixmap item with overlay icon, status item cycling Passive → Active →
  NeedsAttention, `--hostile` item (100 000-character title, 1500/2048 px pixmaps,
  markup in the tool tip, 3000 entries, 12 levels). Prints every call it receives.
  ```
  scripts/dev.sh exec --dir modtray -- cmake -S /src/desktop/horizon-shell/modules/tray/tests -B /build/modtray-tests -G Ninja
  scripts/dev.sh exec --dir modtray -- ninja -C /build/modtray-tests
  ```
* `fake-watcher.py`: another program owning the watcher name.
* `minimal-item.py`: an item on the bare protocol — registers with an object path, icon
  through `IconThemePath`, no menu (`/NO_DBUSMENU`), no `Activate`, emits `NewIcon`.

### What was tested (nested KWin session, `scripts/dev.sh nested --dir modtray`)

* `tests/desktop/smoke.sh --dir modtray horizon-shell`: pass, no QML warnings (tray
  alone and together with the notifications module, built in `modnotif`).
* Items appear with theme icon, pixmap icon and overlay; tool tip surface; left /
  middle click and wheel arrive in the client with the icon's screen position.
* Menu: rendering (check, radio, icons, disabled, separators, live label update every
  second), clicks on a checkable and a radio entry (client reports the trigger, state
  shown on reopening), click on a disabled entry (nothing, menu stays), submenus by
  hover and by keyboard down to the depth limit, entries chosen with Space/Return,
  Left/Escape, click outside, quit through the menu (items disappear).
* Status changes hide, show and mark the icon; attention icon.
* Hostile item: title cut to 128, pixmaps reduced to 16 and 128 px, tool tip to 6
  lines, menu to 150 entries per level and 6 levels, long label elided, scrolling menu
  stays on screen; `--dump tray` in 0.3 s.
* Watcher: items started before the shell register when it appears; a foreign watcher
  is not replaced (shell registers as host, shows and operates its items) and is
  succeeded when it leaves; shell restart; killed client; invalid registrations are
  refused with `InvalidArgs`; a service without an item object is dropped.
* Bare-protocol item: object-path registration, `IconThemePath`, `ContextMenu`,
  `SecondaryActivate`, `Scroll`, icon change on `NewIcon`.

### Not tested

* `ItemIsMenu` items and the fallback from a failing `Activate` to the menu (the
  bare item has no menu to fall back to); the single-`Get` fallback for items whose
  `GetAll` fails; the per-client and total item limits.
* Real applications (Electron, GTK/libayatana-appindicator, Qt `QSystemTrayIcon`),
  whether an application raises its window with the activation token.
* Several screens (the tray is in the panel of the primary screen), HiDPI, touch,
  the installed VM image.

## Limitations

* No XEmbed icons (see above). No shortcuts shown in menus, no access keys, no
  type-ahead; `disposition` is ignored.
* While a menu is open, a click on another tray icon closes the menu first (as with
  the shell's other overlays); the second click opens the other menu.
* A seventh menu level is not read; an entry there opens an empty card.
* If the pointer rests where an icon appears, that icon may look hovered until the
  pointer moves.
