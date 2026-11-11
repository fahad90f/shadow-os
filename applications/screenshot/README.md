# SHADOW Screenshot (`shadow-screenshot`, org.shadow.screenshot)

Captures the whole screen, one monitor, a window or a region selected with
the mouse; then shows the picture in a window where it can be annotated
(crop, arrow, rectangle, text, pixelate — with undo/redo), saved (PNG, JPEG,
WebP), copied to the clipboard, and its folder opened. Also a non-interactive
command for scripts and shortcuts. The desktop shell binds **Print** to
`shadow-screenshot` and **Meta+Shift+S** to `shadow-screenshot --region`.

## How it works

| What | Source |
|---|---|
| Pixels | KWin's D-Bus interface `org.kde.KWin.ScreenShot2` on `org.kde.KWin`, object `/org/kde/KWin/ScreenShot2`: `CaptureWorkspace` (all screens; `CaptureArea` over the virtual geometry if the compositor predates it), `CaptureScreen(name)`, `CaptureActiveWindow`, `CaptureInteractive(0)` (KWin lets the user click a window). The application passes the write end of a pipe; KWin replies with `width/height/stride/format/scale` and writes the raw pixels into the pipe (`screengrabber.cpp`). Options sent: `include-cursor`, `native-resolution`, for windows `include-decoration`. |
| Authorisation | KWin serves only applications whose installed `.desktop` file lists the interface: `X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2` (in `org.shadow.screenshot.desktop`). |
| Fall-back | Only if KWin's interface cannot be used: `org.freedesktop.portal.Screenshot.Screenshot` (xdg-desktop-portal). It delivers the whole workspace; monitor/region are cut out of it, single windows are not possible. |
| Region | The whole workspace is captured first ("frozen"), then `RegionOverlay.qml` shows that picture full-screen on every monitor; dragging selects, releasing confirms, Esc/right button cancels. The result is cut out of the frozen picture, so the overlay itself is never in it. |
| Monitors | `QGuiApplication::screens()` (names as KWin reports them); `--list-monitors`. |
| Clipboard | `KSystemClipboard` (KGuiAddons), i.e. the compositor's data-control protocol, offering `image/png`. On Wayland the content lives in the process that copied it, so the process stays (without a window) until another application replaces the clipboard; `--clipboard` forks before Qt starts so the command returns at once while the child keeps serving. Built without KGuiAddons it falls back to `QClipboard` (needs a focused window). |
| Saving | `QImageWriter`; only formats the installed Qt plug-ins can write are offered. Default `<Pictures>/Screenshots/Screenshot_YYYY-MM-DD_HH-MM-SS.png` (never overwrites: `_2`, `_3`…). "Save as…" takes a path; the ending decides the format. |
| Open folder | `shadow-files <dir>` if installed, otherwise `QDesktopServices::openUrl`. |

Annotations are drawn into the image itself (`document.cpp`), so what is
shown is what is saved/copied; the live preview uses the same painting code.

## Command line

```
shadow-screenshot                  capture all screens, open the result window
shadow-screenshot --region         select a region first
  --full | --monitor NAME | --window | --region     what to capture
  --delay N        wait N seconds (0–60)      --pointer    include the mouse pointer
  --output FILE    write .png/.jpg/.webp and exit, no window
  --clipboard      copy and return, no window (a background process holds the clipboard)
  --list-monitors  print monitor names        --dump backends   JSON: which back-ends are usable
```

Exit codes with `--output`/`--clipboard`: **0** success · **1** capture failed
(no usable back-end, compositor refused) · **2** invalid command line ·
**3** region/window selection cancelled · **4** output file could not be
written · **5** clipboard could not be set.

## Run-time dependencies (Debian packages)

`kwin-wayland` (the ScreenShot2 interface; OpenGL compositing required),
`libkf6guiaddons6`, `qt6-image-formats-plugins` (WebP; JPEG/PNG are in
`libqt6gui6`), `qml6-module-qtquick-controls`, `-layouts`, `-window`,
`qml6-module-qtqml-models`. Optional: `xdg-desktop-portal` + a backend (fall-back), `shadow-files`.

## Limitations

* KWin offers ScreenShot2 only while it composites with **OpenGL**. With
  QPainter compositing (no GPU/DRM device at all) capture is impossible and
  the application says so.
* A region cannot span two monitors (one selection surface per monitor).
* With the portal fall-back: no window capture, no pointer option.
* Text annotations are single-line, fixed size relative to the picture.
* After the window is closed the process stays alive while it owns the clipboard.
* `SHADOW_SCREENSHOT_KWIN_SERVICE=<bus name>` redirects the ScreenShot2 calls to another provider of the interface (test hook, see below).

## What was tested, and how

Environment: build chroot, `scripts/dev.sh nested --dir disks` (KWin 6.3.6
Wayland inside Xvfb). **In that session KWin has no `/org/kde/KWin/ScreenShot2`
object**: there is no DRM device in the build machine's kernel, KWin falls
back to QPainter compositing (also with `--virtual` and `KWIN_COMPOSE=O2`), and
its screenshot plug-in needs OpenGL. The real interface therefore could **not**
be exercised here. To test everything else for real, `tests/fakekwin.cpp`
implements the same D-Bus interface and pipe protocol and takes its pixels
from the X server that displays the nested session (i.e. the true screen
content); the application is pointed at it with
`SHADOW_SCREENSHOT_KWIN_SERVICE`.

```
scripts/dev.sh nested --dir disks --wait 1 -- 'GUI_STEPS=/src/applications/screenshot/tests/gui-steps.sh; . /src/applications/screenshot/tests/nested-test.sh'
```

Result of the last run: **38 passed, 0 failed**. Covered:

* without the double: `--dump backends` reports no back-end, capture exits 1 with the reason;
* `--full`, `--monitor`, `--delay 3` (waits 3 s), `--output` as PNG/JPEG/WebP; the PNG is pixel-identical (ImageMagick `compare -metric AE` = 0) to `shot` (X root window grab);
* exit codes 2 (bad options), 3 (Esc in region selection), 4 (unwritable path), 1 (window capture refused by the double);
* `--region`: overlay on the real Wayland compositor, drag with `xdotool`, result identical to the same area of the X screenshot;
* `--clipboard`: command returns 0, an independent Qt client (`tests/cliptest.cpp`, ordinary `wl_data_device` path) pastes an identical image; the holder exits when another client takes the clipboard (`wl-paste` is not installed in the chroot);
* result window with real mouse/keyboard input: rectangle, arrow, text, pixelate, 4× undo (file equals the original capture), 4× redo (file equals the annotated one), crop, Save (timestamp name in `~/Pictures/Screenshots`), Save as… (WebP), Copy (pasted image identical to the saved file), region capture started from the window (own window not in the picture), Open folder (starts `shadow-files`), closing the window while holding the clipboard (process stays, paste still works, exits after replacement);
* `tests/desktop/smoke.sh --dir disks screenshot`: pass, no QML warnings (shows the "could not be captured" state).

**Not tested** (needs a session where KWin composites with OpenGL — e.g. the
QEMU integration test): interoperability with the real KWin implementation
of ScreenShot2 (the client follows KWin's protocol, but only the test double
has answered it), the `.desktop` authorisation without
`KWIN_SCREENSHOT_NO_PERMISSION_CHECKS`, `--window`/"Active window"/"Pick
window", the pointer option, scale factors ≠ 1, several monitors, the
xdg-desktop-portal fall-back, and the Print / Meta+Shift+S bindings of the shell.
