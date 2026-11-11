# horizon-shell module: notifications

The session's notification service and its user interface: popups, the
notification centre and the bell in the panel.

## What it does

* **Service** — `org.freedesktop.Notifications` at `/org/freedesktop/Notifications`,
  Desktop Notifications Specification 1.2: `Notify`, `CloseNotification`,
  `GetCapabilities` (`actions body body-hyperlinks body-markup icon-static persistence`),
  `GetServerInformation` (`Shadow Horizon`, `SHADOW`, shell version, `1.2`); signals
  `NotificationClosed` (1 expired, 2 dismissed by the user, 3 closed by the client),
  `ActionInvoked` and, before it when the compositor hands out a token,
  `ActivationToken`.
* **Handled**: `replaces_id`; `expire_timeout` (-1: 5 s for low, 7 s for normal urgency;
  0: stays; other values clamped to 1.5–120 s; critical never expires); hints `urgency`,
  `category`, `desktop-entry` (name and icon are then taken from the installed
  application through the shell's `applications` service), `image-data`/`image_data`,
  `image-path`/`image_path`, `icon_data`, `app_icon`, `transient` (popup only, never in
  the centre), `resident` (an action does not close it), `value` (progress bar).
* **Popups** (`NotificationPopups.qml`): a layer-shell surface on the overlay layer,
  top right under the panel, exactly as large as the stack, never takes the keyboard.
  At most 4 at a time, 8 more wait. Click: default action (or dismiss), buttons for
  the other actions, close button, links in the body. The pointer over the stack
  stops all time-outs.
* **Centre** (`NotificationCenter.qml`): a `PopupSurface` registered as the overlay
  `notifications` (bell button, Meta+N). History grouped by application, newest group
  first; dismiss one, a group or all; Do Not Disturb switch; empty state; a banner
  when the service is not this shell. Keyboard: Up/Down, Return (default action),
  Delete (dismiss), Escape. While it is open no popups appear; closing it marks
  everything read.
* **Panel** (`NotificationButton.qml`, order 40): `PanelButton` with the `bell` glyph
  (`bell-off` in Do Not Disturb), `badge` = unread count, `checked` while the centre is open.
* **Do Not Disturb**: holds back popups except those of critical urgency; what arrives
  is in the centre. Persisted in `~/.config/shadow/notifications.conf`
  (`[Notifications] DoNotDisturb=true|false`); the file is watched, so another program
  (Control Center) may change it.

The bus name is requested without the replace flag: if another notification daemon
owns it, the shell does not take it, `notifications.available` is false, the centre
says so, and the shell becomes the service as soon as that daemon leaves (it waits in
the name's queue).

## QML API: context property `notifications`

| | |
|---|---|
| `available` (bool), `unavailableReason` (`""`, `"starting"`, `"no-bus"`, `"other-service"`) | whether this shell is the session's notification service |
| `doNotDisturb` (bool, **read/write**) | bound by quick settings; persisted |
| `unreadCount`, `count` (int) | unread / all notifications in the centre |
| `popups`, `history` | list models; roles `notificationId appKey appName summary body bodyPlain urgency critical actions hasDefaultAction imageSource iconSource progress timestamp unread resident interactive` |
| `popupsHovered`, `centerOpen` (bool, read/write) | set by the module's own QML |
| `activate(id, window)`, `invokeAction(id, key, window)`, `dismiss(id)`, `dismissApp(appKey)`, `clearAll()`, `markAllRead()`, `openLink(id, link)`, `group(appKey)` | |

Other modules must cope with the property not existing (the module may not be built).

## Untrusted input

Every client of the session bus can call `Notify`; what it sends is treated as hostile.

* **Text** (`notificationtext.*`): application name ≤ 64, summary ≤ 200, action labels
  ≤ 48, category ≤ 64 characters, single line, control and bidirectional-override
  characters removed; shown with `Text.PlainText`. The body is parsed and written anew:
  the result is escaped text plus `<b> <i> <u> <br> <a href="N">` only (N is an index
  into at most 8 stored link targets; only `http`, `https` and `mailto` without
  credentials are accepted). `<img>` leaves its `alt` text, `<script>`/`<style>`
  content is dropped, at most 16 000 characters are looked at and 2000 kept.
* **Images** (`notificationimage.*`): raw data up to 1024×1024 with a consistent
  header, checked before the pixels are copied; files only if absolute/`file:` without
  host, regular, ≤ 4 MiB, ≤ 4096 px and of a known image format; anything with a URL
  scheme is ignored — nothing is ever fetched. Kept as copies of at most 128 px.
  Shown through the module's own image provider (`image://nxnotify/...`, registered
  with `Shadow::App::engine()`).
* **Counts**: 6 action buttons, 250 notifications in the centre and 50 per
  application (the oldest are closed with reason 1), one popup allowance per client
  (a burst of 10, then one per second; the rest is in the centre only).
* **Ownership**: only the sending connection may replace or close a notification —
  or anybody once that connection has left the bus (`notify-send --replace-id`).
  Action buttons disappear when the client that would hear them has left.
* The history model is updated at most every 30 ms and is detached from its view
  while the centre is closed.

Deviations from the letter of the specification, on purpose: `NotificationClosed`,
`ActionInvoked` and `ActivationToken` are sent to the notification's owner, not
broadcast; `CloseNotification` for an unknown id returns without an error.

## Diagnostics

`horizon-shell --dump notifications` prints configuration, capabilities, limits, who
owns the service and — if that is a Shadow Horizon shell — its state (counts, popup
queue, statistics; per notification only flags and lengths). The texts of
notifications are included only when the *shell* was started with
`SHADOW_NOTIFICATIONS_DEBUG=1`, because every program that may talk to the notification
service could otherwise read everybody's notifications. The state comes from the
method `GetState` of the interface `org.shadow.horizon.Notifications` on the service's object.

## Run-time dependencies (Debian packages)

Those of the shell itself (`libqt6dbus6`, `libqt6svg6`, `libkf6windowsystem6`), plus
`qt6-svg-plugins` (SVG notification images; `qt6-image-formats-plugins` for WebP) and
`xdg-utils` (opening links). An icon theme for `app_icon` names. Testing:
`libnotify-bin`, `libglib2.0-bin` (gdbus), `python3-gi`.

## Tests

* `tests/notificationselftest.cpp` (own CMake project, QtCore only): 215 checks of the
  text sanitiser — markup subset, link targets, injection attempts, size and time bounds.
  ```
  scripts/dev.sh exec --dir modnotif -- cmake -S /src/desktop/horizon-shell/modules/notifications/tests -B /build/modnotif-tests -G Ninja
  scripts/dev.sh exec --dir modnotif -- ninja -C /build/modnotif-tests
  scripts/dev.sh exec --dir modnotif -- /build/modnotif-tests/notificationselftest
  ```
* `tests/notify-client.py` (python3-gi): a client that stays on the bus and prints the
  signals it gets: `send` (actions, hints, replace/close sequences), `flood`,
  `oversized` (hostile payloads), `occupy` (a competing daemon), `state`.

### What was tested (nested KWin session, `scripts/dev.sh nested --dir modnotif`)

* `tests/desktop/smoke.sh --dir modnotif horizon-shell`: pass, no QML warnings
  (notifications alone and together with the tray module).
* `notify-send`/`gdbus`: server information, capabilities, urgencies, icon name,
  `value` hint, markup and injection attempts in summary and body.
* Replace (same id, progress 0→50→100, one notification), close by client (reason 3),
  transient + time-out (reason 1, not in the centre), normal time-out (popup goes,
  stays unread in the centre, no signal), a second client cannot replace or close a
  live client's notification.
* Clicks with xdotool: action button (`ActivationToken`, `ActionInvoked reply`,
  `NotificationClosed 2`), body (`default`), resident (not closed by its action), close
  button, popup without default action; hover holds a 2 s popup for 5 s; a focused
  window keeps the keyboard while a popup shows.
* Hostile payloads (`oversized`): 1 MB summary, 8 MB body, 100 k application name, 4 MB of
  white space, 400 actions, 1 MB action key, raw images 2048² / lying header / negative
  size (refused), 1024² (kept as 128²), `http:`/`https:` image and icon (a local HTTP
  listener saw no request), `/dev/zero`, `/proc/self/mem`, `file://host/…`, hints of
  wrong types, deep and unterminated markup. Longest time in one `Notify`: 13 ms. A
  message with a 64 MB image is refused by the message bus itself (the client is disconnected).
* Flood: 500 notifications from one client in one application (answered in 0.14 s,
  50 kept, 10 popups), 500 over 20 applications with the centre open (250 kept), 5
  clients × 100 in parallel; the service answered within 30 ms right after, memory
  +23 MB, 2 s CPU in total.
* Centre: Meta+N and the bell, grouping, scrolling, clear all, dismiss with the
  keyboard, empty state, Do Not Disturb switch (file written; normal held back,
  critical pops up), light and dark theme.
* Another daemon owning the name before the shell starts: not taken over, banner
  shown, `--dump` reports the other owner; the shell became the service when it left.
* `desktop-entry` (name and icon of an installed SHADOW application), `image-path` as
  `file:` URL (SVG), `app_icon` as path and as theme name.

### Not tested

* Opening a link from a body (`openLink` → `xdg-open`): no browser in the test session.
* A change of `notifications.conf` by another program while the shell runs; the
  quick-settings binding to `doNotDisturb` (that module is separate).
* Whether an application really raises its window with the `ActivationToken`.
* Real applications (browsers, mail, chat), several screens (popups and centre are on
  the primary screen), HiDPI, the installed VM image.

## Limitations

* No sounds (`sound-file`, `sound-name`, `suppress-sound` are ignored), no action
  icons, no inline reply, no persistence of the history across a shell restart.
* Popups and the centre appear on the primary screen only.
* A popup that is among the oldest of a flood can be closed by the history limit
  while it is still showing.
