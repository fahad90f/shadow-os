# NEXUS Terminal

`nexus-terminal` (app id `org.nexus.terminal`) is the terminal emulator of
NEXUS OS: tabs, split panes, profiles, SSH connections, search in the
scrollback and session restore.

It is the one NEXUS application built with **Qt Widgets** instead of QML: the
terminal view is [QTermWidget](https://github.com/lxqt/qtermwidget) (a
`QWidget`), so the window around it is made of widgets as well. It does not use
`Nexus::App`; `main.cpp` implements the same contract itself (`--help`,
`--version`, `--dump <topic>`, `NEXUS_SMOKE_TEST`, `NEXUS_SCREENSHOT`,
`NEXUS_SCREENSHOT_DELAY`, `NEXUS_WINDOW_SIZE`).

## Features

| Area | What it does |
|---|---|
| Tabs | New (`Ctrl+Shift+T`), close (`Ctrl+Shift+W`, middle click, ×), reorder by dragging or `Ctrl+Shift+PgUp/PgDn`, rename in place (double-click), next/previous (`Ctrl+PgDn/PgUp`, `Ctrl+Tab`), `Alt+1…9`. The title is the one the program sets (OSC 0/2), else the directory and the foreground program. |
| Panes | Split right (`Ctrl+Shift+R`) / down (`Ctrl+Shift+D`), close (`Ctrl+Shift+X`), focus by direction (`Alt+Arrow`), resizable splitters, accent outline on the active pane. |
| Shells | Default is the login shell from the passwd database. A profile can pick any shell of `/etc/shells` or a custom command. |
| Environment | `TERM=xterm-256color`, `COLORTERM=truecolor`, `TERM_PROGRAM=nexus-terminal`, `SHELL`; locale and everything else is passed through (`LANG=C.UTF-8` only if no locale is set at all). Test-hook variables are not passed on. |
| Search | `Ctrl+Shift+F`: incremental, previous/next, match case, regular expressions. |
| SSH | "New SSH Connection" (`Ctrl+Shift+S`): host, user, port, identity file, jump host; saved connections. The tab stays open when ssh ends ("Reconnect"). |
| Clipboard | `Ctrl+Shift+C/V`, `Shift+Ins` and middle click for the primary selection, optional copy on select, context menu. Pasted text is sanitised and multi-line pastes are confirmed (see *Security*). |
| Profiles | Name, command, working directory rule, font family (fixed-width only) and size, colour scheme, cursor shape/blink, scrollback (0 … 1 000 000 lines or unlimited), bell (none/visual/sound), opacity, "keep the pane open". Create, duplicate, delete, choose the default. |
| Zoom | `Ctrl++`, `Ctrl+-`, `Ctrl+0`, `Ctrl`+wheel — per pane, on top of the profile's size. |
| Colour schemes | "NEXUS Obsidian" and "NEXUS Daylight" (generated from the design tokens) plus every scheme QTermWidget ships. *Automatic* follows the system appearance. |
| Shortcuts | Preferences → Shortcuts lists every command with two editable bindings; conflicts and keys that would be taken away from the terminal are refused. |
| Directory | New tabs and splits start in the directory of the active terminal (`QTermWidget::workingDirectory()`); `--workdir`; "Open File Manager Here" starts `nexus-files <dir>`. |
| Processes | Foreground process (name, PID) in the status bar and the tab tool tip; Terminal → Send Signal (INT, TERM, KILL with confirmation, STOP, CONT) to the foreground process group; closing asks when something other than an idle shell would be ended. |
| Session | On exit the window geometry and, per tab, the split layout with profile and directory of every pane are saved. The next plain start offers them in a banner (or restores at once / never — Preferences → General). |
| Window | Full screen (`F11`), compact mode without menu bar (`Ctrl+Shift+M`), optional status bar and scroll bar. |

Command line:

```
nexus-terminal [--workdir <dir>] [--profile <name>] [--no-restore] [--hold] [-e <command> [args...]]
nexus-terminal --dump <topic>
```

`-e` must be the last option; everything after it is the command's argument
vector. A window opened with `-e` closes when the command ends unless `--hold`
is given, and it never restores or overwrites the saved session.

## Where the data comes from

| Data | Source |
|---|---|
| Login shell, fallback home directory | passwd database (`getpwuid_r`); `$HOME` wins for the home directory |
| Selectable shells | `/etc/shells`, entries that exist and are executable |
| Working directory of a terminal | `QTermWidget::workingDirectory()` → `/proc/<pid>/cwd` of the pane's program |
| Foreground process | `QTermWidget::getForegroundProcessId()` (`tcgetpgrp` on the pty) and `/proc/<pid>/comm` |
| Terminal title | escape sequences of the program (`QTermWidget::title()`), else derived from the two rows above |
| Appearance (dark/light, accent, font scale, transparency) | `Nexus::Appearance` (`~/.config/nexus/appearance.conf`), followed live |
| Colours, sizes, fonts of the chrome | design tokens, rendered at build time (below) |
| Settings, profiles, key bindings, SSH destinations, saved session | `~/.config/nexus/terminal.conf` (QSettings native format; system defaults from `/etc/xdg/nexus/terminal.conf`) |

`terminal.conf` sections: `[General]` (window, clipboard, session options),
`[Profiles]` + one `[Profile_<id>]` each, `[Shortcuts]` (only bindings that
differ from the defaults), `[SshTargets]`, `[Session]` (`Geometry`, `State`).

### Diagnostics

`nexus-terminal --dump <topic>` prints JSON without opening a window:

| Topic | Content |
|---|---|
| `profiles` | every profile with all settings, the resolved command (argument vector) and the default |
| `schemes` | available colour schemes, their origin, the automatic dark/light pair, where the NEXUS schemes were found |
| `shells` | `/etc/shells` entries (path, executable), the shells offered, the default shell and its source |
| `shortcuts` | every command with default and current bindings |
| `ssh` | ssh client found?, saved destinations with validation result and the argument vector they produce |
| `settings` | general options, saved session, path of the settings file |

## Theming

Nothing visual is hard-coded. At build time `scripts/gen-tokens.py render`
produces, for every appearance mode × accent in
`desktop/themes/tokens/nexus-tokens.json`:

* `style/terminal.qss.in` → the Qt style sheet of the chrome,
* `style/theme.json.in` → the colour table used from C++ (palette, icons),
* `tools/glyphs.py` → PNG indicator glyphs (check marks, chevrons) in the
  variant's colours, because Qt loads SVG from a style sheet only with the
  optional `qt6-svg-plugins`,
* `src/tokens.h.in` → sizes, radii, fonts as C++ constants,
* `style/Nexus{Obsidian,Daylight}.colorscheme.in` → the two terminal colour
  schemes, installed to `/usr/share/nexus/terminal/color-schemes` and
  registered with `QTermWidget::addCustomColorSchemeDir()` (a copy is embedded
  in the binary as a fallback).

`Theme` (src/theme.cpp) installs palette, font and style sheet and switches
them when `Nexus::Appearance` changes; terminals on the *Automatic* scheme
switch between Obsidian and Daylight. Icons are the NEXUS symbolic set
compiled into `libnexusui`.

## Security

* **No shell** is involved in starting anything. Profiles' custom commands are
  split into words with POSIX quoting rules and nothing else; ssh, `-e`
  commands, `nexus-files` and new windows are started from argument vectors.
* **SSH**: every field is validated against a strict pattern (a host or user
  can never start with `-`), ssh gets `--` before the host, and saved entries
  are validated again when used (the settings file can be edited by hand).
  There is no password field; nothing secret is stored or logged.
* **Paste**: control characters (ESC, `^C`, …, C1) are removed, so pasted text
  cannot act as key presses or end a bracketed paste; trailing line breaks are
  dropped (optional); several lines — or text from which control characters
  were removed — are shown for confirmation first, with *Cancel* as the
  default. This applies to the clipboard, the primary selection (middle
  click) and dropped text alike.
* **Signals** go to the foreground process group of the terminal only (never
  to group 0 or 1); SIGKILL asks first.
* **Session restore** stores directories, profile ids and the layout — never
  commands, environment or terminal contents — and always starts fresh shells.
  ssh and `-e` panes come back as local shells of their profile.
* Links open only with Ctrl+click or from the context menu, and only
  `http`, `https`, `ftp` and `mailto`.

No privileged operations, no polkit actions.

## Runtime dependencies (Debian packages)

| Package | Needed for |
|---|---|
| `libqtermwidget6-2` (pulls `qtermwidget-data`) | terminal emulation, key bindings, stock colour schemes |
| `libqt6widgets6`, `libqt6gui6`, `libqt6core6t64`, `libqt6svg6` | toolkit; SVG rendering of the icons |
| NEXUS UI library (`libnexusui`, built from `desktop/ui`) | `Nexus::Appearance`, symbolic icons |
| `fonts-jetbrains-mono`, `fonts-inter-variable` | default terminal and UI fonts |
| `bash` | default shell of the distribution; `zsh` is offered when installed |
| `openssh-client` | SSH connections (the dialog says so if it is missing) |
| `xdg-utils` | opening links (`xdg-open` through `QDesktopServices`) |
| `qt6-wayland` | running in the Wayland session |
| `nexus-files` (optional) | "Open File Manager Here" (disabled with an explanation otherwise) |

Build: `libqtermwidget-dev`, `qt6-base-dev`, `qt6-svg-dev`, Python 3. Debian's
`libqtermwidget-dev` references `libutf8proc.so` in its CMake target without
depending on `libutf8proc-dev`; `CMakeLists.txt` drops that link item when the
file is absent.

## Limitations

* **Exit status**: QTermWidget does not report how the program of a pane
  ended, so a pane that is kept open says "has ended" without a status.
* **Search** is QTermWidget's: one match at a time, shown as the selection; no
  match count and no highlighting of all matches. The application's search bar
  drives the library's hidden search bar and its `find*()` slots; if a future
  QTermWidget is laid out differently, the library's own bar is shown instead.
* **Cursor colour and selection colours** cannot be set through QTermWidget
  and follow the colour scheme's foreground/background.
* **Opacity** needs a compositing window manager and is switched off when
  transparency is disabled in the appearance settings.
* **Tab labels** are centred next to their icon (QTabBar draws them).
* **Sessions** are saved when the window closes or the process gets SIGTERM,
  SIGINT or SIGHUP — not after a crash or SIGKILL. With several windows the
  one closed last is the one restored.
* **Remote directories**: the working directory is that of the local process;
  inside ssh it is the directory ssh was started in.
* Text is in English; strings go through `tr()` but no translations ship yet.
