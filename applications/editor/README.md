# SHADOW Text Editor

`shadow-editor` (application id `org.shadow.editor`) is a programmer's text
editor: tabs, syntax highlighting, find/replace with regular expressions,
safe handling of text encodings and line endings, and no surprises when a file
is too big or is not text.

```
shadow-editor [file[:line] ...]      open files; a second launch adds tabs to the running window
shadow-editor --probe <file ...>     JSON: detected encoding, BOM, line endings, syntax (no window)
shadow-editor --resave <in> <out>    decode <in> and write it back as the editor would (no window)
shadow-editor --dump <topic>         syntaxes | encodings | themes | limits | settings
SHADOW_EDITOR_BENCH=1 shadow-editor <file>   time loading/rendering/editing of <file>, print, quit
```

## What it does

| Area | Behaviour |
|---|---|
| Tabs | One document per tab; Ctrl+N, Ctrl+W, Ctrl+Tab; middle click closes; a dot marks unsaved changes. |
| Open / save | Ctrl+O (several files at once), Ctrl+S, Ctrl+Shift+S, "Save all", "Reload from disk". Saving is atomic (`QSaveFile`: temporary file, `fsync`, rename). Only where the directory cannot hold a temporary file the file is written in place (`setDirectWriteFallback`). |
| Encodings | Detection order: byte order mark (UTF-8, UTF-16 LE/BE, UTF-32 LE/BE) → UTF-16 without BOM (NUL pattern) → strict UTF-8 → an 8-bit fallback that maps every byte to a character and back (windows-1252, else ISO-8859-1) and is verified by re-encoding before it is accepted. A file is written back in the encoding and with the BOM it was read with. Characters that the chosen encoding cannot represent make the save fail with an explanation; nothing is replaced by `?`. The status bar opens a dialog to *reopen* with another encoding or to *convert*. |
| Binary files | Files containing NUL bytes that are not UTF-16/32 are refused, with the reason. |
| Line endings | LF / CRLF / CR are detected, kept on save and can be converted from the status bar. Mixed files are flagged and the user is told what saving will write. |
| External changes | `QFileSystemWatcher` plus a size/mtime check whenever the window is activated. A bar offers "Reload"; saving over a newer file asks first. Deleted files are reported. |
| Syntax highlighting | KSyntaxHighlighting definitions (about 400 languages), chosen from the file name, then the MIME type; changeable from the status bar. Colours: two themes shipped with the application, `themes/shadow-dark.theme` and `themes/shadow-light.theme`, built from the SHADOW design tokens; they follow the light/dark appearance. |
| Editing | Line numbers, current-line highlight, word wrap (Alt+Z), undo/redo, auto-indent (keeps the indentation; one level more after `{ [ (` and, in Python/YAML, after `:`), Tab/Shift+Tab indent the selected lines, Backspace steps back one level in leading spaces, tabs or spaces with a width of 1–16. Ligatures are switched off. |
| Find / replace | Ctrl+F / Ctrl+H, F3 / Shift+F3. Plain or regular expression (PCRE, `^`/`$` per line), match case, whole words, "n of m" counter, all visible matches highlighted, replace one / all (`\1` or `$1` insert groups; "replace all" is one undo step). Invalid expressions show the error. |
| Go to line | Ctrl+G, `line` or `line:column`; also `file:line` on the command line. |
| Font | Any installed fixed-pitch family (default: JetBrains Mono, 11 pt), Ctrl++ / Ctrl+- / Ctrl+0. |
| Status bar | Line/column, selection length, line count, indentation, encoding (+BOM), line endings, syntax. |
| Unsaved changes | Closing a tab or the window asks per document: Save / Discard / Cancel. |
| Recent files | Clock button in the toolbar (15 entries). |
| Session | The list of open files, the active tab and the cursor positions are restored at the next start (can be switched off). **Unsaved text is not restored** — there are no recovery files. |
| Drag and drop | Dropping files on the window opens them (implemented with a QML `DropArea`; see "Not tested"). |
| Launcher | `MimeType=` in `org.shadow.editor.desktop` covers `text/plain` and the common source types; `Exec=shadow-editor %F`. |

## Architecture and data sources

* `textfile.{h,cpp}` — bytes ↔ text (detection, encoding, line endings). Used by the GUI and by `--probe`/`--resave`.
* `document.{h,cpp}` — `EditorDocument`: asynchronous load and save (`QtConcurrent`), file watching, KSyntaxHighlighting, search, indentation edits. Text is read back with `QTextDocument::toRawText()`, not `toPlainText()`, because the latter turns no-break spaces into spaces.
* `codeeditor.{h,cpp}` — the text area. See the next section.
* `backend.{h,cpp}` — recent files, session, lists for the menus, directory listing for the file chooser.
* `Main.qml`, `EditorView.qml`, `FindBar.qml`, `FileNotice.qml`, `PathPicker.qml` — the UI, ShadowUI components only.
* Settings: `~/.config/shadow/editor.conf` (`[Editor]`, `[Recent]`, `[Session]`).
* No system services, no privileged operations, no network.

## Large files — what was measured

The brief allowed a QML `TextArea` "if performance is adequate". It is not.
Measured in the build chroot (software rendering, 1280×860 window, a shared and
loaded 4-core machine, `SHADOW_EDITOR_BENCH=1`), first with a QML `TextEdit`,
then with the current implementation:

| File | QML `TextEdit` | Current (`QPlainTextEdit` engine) |
|---|---|---|
| 30 KiB C++, highlighted | keystroke 75–290 ms | keystroke 8–33 ms |
| 1 MiB, 12 624 lines | putting the text in: 5.5–14 s; **one keystroke: 1.9–9.9 s**; undo 1.8–2.1 s | text in: 0.11 s; keystroke 13–15 ms |
| 1 MiB C++, highlighted | — | text in: 0.47 s; keystroke 17–26 ms |
| 3.9 MiB C++, highlighted | — | text in: 1.7 s; keystroke 29–34 ms |
| 5 MiB, 61 863 lines | — | text in: 0.6 s; keystroke 1–14 ms |
| **50 MiB, 604 958 lines** | — | **opens; text in: 10 s (window frozen meanwhile, "Opening…" shown); afterwards keystroke 21–27 ms, jump to end/middle 24–28 ms, word-wrap toggle 0.2 s** |
| 70 MiB | — | refused with an explanation |
| 2.2 MiB JSON on **one line** | — | opens with a warning; **every keystroke takes 4–6 s** |

Consequences, all visible to the user:

* The editing engine is a `QPlainTextEdit` (it lays out only the lines on
  screen). The widget is never mapped; `CodeEditor`, a `QQuickPaintedItem`,
  renders it into the QML scene on the GUI thread and forwards keyboard, mouse,
  wheel, focus and input-method events. Toolbar, tabs, dialogs and scroll bars
  stay QML/ShadowUI.
* Files larger than **64 MiB** are not opened (`EditorLimits::maxFileBytes`);
  the dialog says why. Memory use is several times the file size (UTF-16 text
  plus per-line layout data).
* Above **4 MiB of characters** syntax highlighting is switched off and the
  status bar says "(highlighting off)".
* A line longer than **100 000 characters** produces a warning when the file is
  opened: Qt lays a paragraph out as a whole, so such files are slow to edit
  however small they are. This is not solved.
* Loading reads and decodes in a worker thread, but putting the text into the
  document happens on the GUI thread: the window does not react during that
  time (about 0.2 s per MiB on the test machine).

## Run-time dependencies (Debian packages)

`libkf6syntaxhighlighting6`, `libkf6syntaxhighlighting-data`,
`libqt6widgets6`, `libqt6concurrent6`, `libqt6quick6`, `qml6-module-qtquick-controls`,
`qml6-module-qtquick-layouts`, `qml6-module-qtcore`, `fonts-jetbrains-mono`
(default font; any fixed-pitch font works), the SHADOW UI library
(`org.shadow.ui`, `org.shadow.style`). `qml6-module-org-kde-syntaxhighlighting`
is **not** needed: the highlighter is driven from C++.

## Limitations

* No recovery of unsaved text after a crash or power loss.
* Text containing U+2029 (paragraph separator) or NUL characters cannot be kept
  exactly by `QTextDocument`; the file opens with a warning that saving changes
  those characters.
* Mixed line endings are normalised to the dominant one on save (announced).
* A file whose directory is not writable is saved in place, not atomically.
* The file chooser is the application's own simple dialog (the Qt Quick
  fallback dialog is not styled): no bookmarks, no type filter, no search.
* Dragging selected text with the mouse is not supported (a press inside a
  selection places the cursor); cut/copy/paste work.
* No code folding, bracket matching, multiple cursors, split views, printing
  or spell checking. No "open with sudo".
* Input methods: events are forwarded to the widget, but no input method was
  available to test with.
* Accessibility: the text area is announced as an editable text with the file
  name; its content is not exposed line by line to screen readers.

## Tested (build chroot, Debian 13, Qt 6.8.2, KF 6.13)

Commands are relative to the repository root; `arced` is the build directory.

* Build: `SHADOW_JOBS=2 scripts/dev.sh build --dir arced --only "archive-manager;editor"` — no warnings.
* Smoke: `tests/desktop/smoke.sh --dir arced editor` — PASS, zero QML warnings.
* Encoding layer: 20 crafted files through `--probe` and `--resave`
  (UTF-8 ± BOM, UTF-16 LE/BE ± BOM, UTF-32, Latin-1, windows-1252, every byte
  1–255, CRLF, CR, mixed, no final newline, empty, U+2028, U+2029, ELF binary):
  detection as expected; output byte-identical except, as announced, for the
  two files with mixed line endings; the binary was refused.
* The same through the real GUI (nested KWin Wayland session, `xdotool`):
  12 files opened as tabs, one character typed and deleted in each, Ctrl+S,
  then `cmp` with the originals — all 12 byte-identical, all rewritten.
* In the nested session, driven with `xdotool`, checked on screenshots and on
  the files on disk: typing; auto-indent after `:` in Python; Ctrl+S; find
  with match counter and highlighted matches; regular-expression "replace all"
  with a group (2 replacements, file content verified) and its single-step undo
  (file byte-identical to the original again); go to `100:5`; external change
  (`echo >> file`) → warning bar; close-tab prompt; "Save as" through the file
  chooser (file created with the typed text); second launch from another
  working directory with `sample.cpp:42` → new tab in the running window at
  line 42; quit with unsaved changes → Save → file written and process ended;
  restart without arguments → previous tabs restored; binary file → refusal
  dialog and 70 MiB file → refusal dialog; Tab / Shift+Tab on a selection
  (file content verified); line-ending menu LF → CRLF (file verified);
  encoding dialog; word wrap.
* Large files: the table above (`SHADOW_EDITOR_BENCH=1`).
* Light and dark theme screenshots.

## Not tested

* Drag and drop of files from another application (no drag source could be
  driven in the test session).
* Input methods, touch screens, high-DPI scaling other than 1×.
* Switching between light and dark while the editor is running (both were
  only tested from start-up).
* Launching from a file manager. The association itself was exercised once:
  with `text/plain=org.shadow.editor.desktop` in `mimeapps.list`, `gio open
  file.txt` (issued by the Archive Manager's preview) started `shadow-editor`
  with the file through the `.desktop` entry.
* Behaviour on a hardware-accelerated display; all timings are from software
  rendering.
