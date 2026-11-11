# NEXUS Files

`nexus-files` (id `org.nexus.files`) is the file manager of NEXUS OS: local
folders, the Trash, removable and internal drives, and network locations in one
window with tabs, a split view and a preview panel.

```
nexus-files [path-or-url...] [--select <file>]
nexus-files --dump places|devices|list [path-or-url]|protocols
```

Every positional argument opens in its own tab (`/path`, `~`, `trash:/`,
`ftp://host/dir`, …). `--select` opens the folder of a file and selects it.

## What it does

| Area | Behaviour |
|---|---|
| Browsing | Grid and details view (Ctrl+1 / Ctrl+2), icon sizes 48–128 px, sorting by name, size, date or type, folders first, breadcrumb bar with an editable location field (Ctrl+L), back/forward/up history per pane, type-ahead selection, rubber band, keyboard navigation. |
| Tabs, split view | Ctrl+T / Ctrl+W / Ctrl+Tab, middle click on a folder opens a background tab; F3 splits the tab into two panes, F6 switches between them. |
| Filter and search | The toolbar field filters the current folder while typing; Enter searches the folder and all sub-folders by name (substring, or a `*`/`?` pattern). |
| Hidden files | Ctrl+H; the status bar says how many items are hidden. |
| Previews | Thumbnails in both views; the preview panel (Alt+P) shows a larger image, the first lines of text files, and type, size, dates, owner and permissions. |
| Properties | Type, MIME type, location, size (folders are measured in the background), dates; owner, group and an rwx matrix that can be changed (for folders optionally including everything inside; files then only receive an execute bit if they already have one). |
| File operations | Copy, cut, paste, "Copy to…", "Move to…", duplicate, rename (inline, F2), new folder (Ctrl+Shift+N), new file, link (Ctrl+Shift+drop), undo (Ctrl+Z). Running operations are listed behind the toolbar indicator with progress, amounts, speed and a cancel button; failed ones stay there until dismissed. |
| Conflicts | A dialog offers Replace/Merge, Skip, Rename…, Keep both and Stop, with "do this for all remaining items". Errors on single items offer Skip/Retry/Stop. Copies pasted into the folder they came from are renamed automatically. |
| Deleting | Delete moves to the Trash. Shift+Delete and "Delete permanently…" always ask first; so does "Empty the Trash…". |
| Trash | freedesktop.org Trash specification through KIO's `trash:/` (`~/.local/share/Trash`, per-volume `.Trash-$uid`): view, restore to the original location, delete, empty. |
| Archives | "Compress…" (tar.gz, tar.xz, tar.zst, zip), "Extract here", "Extract to…". Nothing existing is overwritten; a failed or cancelled run leaves nothing behind. |
| Network | "Connect to server…" builds an FTP, SFTP, SMB, WebDAV(S), NFS or FISH address; servers can be kept in the sidebar. Protocols whose KIO worker is not installed are marked and cannot be connected to; without any network worker the entry is not offered at all. |
| Drives | Volumes with a file system in the sidebar: click mounts and opens, capacity bar, unmount / safely remove, eject for optical drives. Without the system bus or UDisks2 the sidebar says so instead. |
| Drag and drop | Onto folders, the panes, tabs, sidebar places, drives and the Trash; from and to other applications (`text/uri-list`). Without a modifier: move within one file system, copy otherwise; Ctrl copies, Shift moves, Ctrl+Shift links. |
| Open | Default application of the type, "Open with…" with an optional "always use"; "Open terminal here" (F4, context menus, menu). |
| Storage information | Free and total space of the current file system in the status bar; a "Read-only" marker where the folder cannot be written to. |

Keyboard shortcuts: Ctrl+T/W/Tab, Ctrl+L, Ctrl+F, Ctrl+H, Ctrl+1/2, Ctrl++/-/0,
Ctrl+A/C/X/V, Ctrl+Z, Ctrl+Shift+N, F2, F3, F4, F5, F6, Alt+P, Alt+Left/Right/Up,
Alt+Home, Alt+Enter (properties), Delete, Shift+Delete, Backspace, Menu, Escape.

## Where the data comes from

| Data / action | Source |
|---|---|
| Folder contents, changes | `KCoreDirLister` (KIO); local folders are watched, remote ones and the Trash refresh after each operation. |
| File operations, undo | KIO jobs (`KIO::copy/move/link/trash/del/mkdir/chmod/restoreFromTrash/emptyTrash`), `KIO::FileUndoManager`. Jobs run in KIO worker processes/threads, never on the UI thread; their questions are routed to the QML dialogs through `KIO::AskUserActionInterface` (`src/interaction.cpp`). |
| Thumbnails | `KIO::PreviewJob` (thumbnailer plugins, cache in `~/.cache/thumbnails`); fallback for images Qt can decode: `QImageReader` on a worker thread. |
| Search | Local: `std::filesystem` walk on a worker thread; other locations: `KIO::listRecursive`. |
| Archives | `bsdtar` (libarchive-tools) with an argument vector via `Nexus::Command`; extraction goes to a temporary folder inside the destination and is moved into place on success. |
| Drives | Solid (`Solid::StorageAccess`, `StorageVolume`, `OpticalDrive`), i.e. UDisks2 on the system bus; UDisks2 authorises mounting through its own polkit policy. Space: `QStorageInfo`. |
| Network protocols | `KProtocolInfo` (installed KIO workers). Passwords are asked for by KIO's password service (`kpasswdserver` in `kiod6`, started through the session bus), not by this application; it never sees or stores them. |
| Applications | `KApplicationTrader` / `KIO::ApplicationLauncherJob`; a default is stored in `~/.config/mimeapps.list`. |
| Terminal | `nexus-terminal --workdir <dir>` (the option nexus-terminal implements), else `x-terminal-emulator` started in the folder. |
| Icons | freedesktop icon theme by MIME type (Breeze as fallback); symbolic UI glyphs from NexusUI. |
| Settings | `~/.config/nexus/files.conf`: `[View]`, `[Bookmarks]`, `[Servers]`. |

No operation needs a helper of its own; the application has no polkit action.

## Run-time dependencies (Debian packages)

Besides the shared libraries (`libkf6kiocore6`, `libkf6kiogui6`,
`libkf6kiowidgets6`, `libkf6solid6`, `libkf6service6`, `libkf6coreaddons6`,
`libkf6configcore6`, Qt 6) and `nexus-ui`:

| Package | Needed for |
|---|---|
| `kio6` | KIO workers `file`, `trash`, `ftp`, `http`/`webdav(s)`; `kiod6` with the password service. **Required.** |
| `libarchive-tools` | `bsdtar` — compress/extract. Without it the archive entries are not shown. |
| `breeze-icon-theme`, `shared-mime-info` | File type icons and MIME detection. |
| `udisks2` | Listing, mounting, unmounting and ejecting drives (recommended). |
| `kio-extras` | `sftp`, `smb`, `nfs`, `fish` workers and more thumbnailers (recommended; without it these protocols are shown as "not installed"). |
| `nexus-terminal` | "Open terminal here" (recommended; the entry is hidden without a terminal). |
| `nexus-archive-manager` | "Open with Archive Manager" (optional; hidden without it). |
| `qt6-image-formats-plugins`, `kdegraphics-thumbnailers`, `ffmpegthumbs` | More thumbnail formats (optional). |

The session must export its display to the D-Bus activation environment
(`dbus-update-activation-environment WAYLAND_DISPLAY QT_QPA_PLATFORM …`), or
`kiod6` cannot show the password prompt for servers that need one; the folder
then reports "Communication with the local password server failed".

## Limitations

* The password prompt and the "retry authentication" question for network
  locations are KIO's own (Qt Widgets) windows, not NexusUI dialogs.
* Untrusted TLS certificates (WebDAVS, FTPS) are refused, never overridden.
* Archives are created and extracted on local folders only; encrypted
  archives are not supported. Extraction relies on bsdtar's own refusal of
  absolute paths and `..` members.
* Encrypted (LUKS) volumes are not unlocked here; they appear once unlocked.
  Unmounting a removable drive also powers it off when Solid/UDisks2 decide so.
* No batch rename, no ownership (chown) editing, no ACL or extended-attribute
  view, no file-content search.
* Permissions can only be edited for one item at a time (optionally
  recursively for a folder).
* KIO reports some refusals only as "cancelled" (e.g. deleting inside an
  immutable folder as root); the application then says that the operation
  was stopped without being able to name the exact reason.
* One window per process: a second `nexus-files` start activates the running
  instance (Nexus::App single-instance handling).

## Tests

Nothing under `tests/` is installed. All of it runs in the build chroot:

```
applications/files/tests/run-ui.sh <scenario>      # nested NEXUS HORIZON session (KWin/Wayland), as root
applications/files/tests/run-user.sh perms-user    # plain X server, as the unprivileged user "nobody"
scripts/dev.sh exec --dir files -- bash /src/applications/files/tests/devices-udisks.sh
```

The scenarios (`tests/ui/*.sh`) drive the real window with xdotool, print the
state of the scratch directories under `/tmp/files-test-*` after every step
and save screenshots as `build/work/shots/files-<scenario>-*.png`. Clicks use
fixed window coordinates (1600x900 screen, default window size, dark theme),
so a layout change needs the coordinates in the scenario adjusted; always look
at the screenshots as well as the printed state. A nested session is ended
after 180 seconds, and copy speed in the chroot varies a lot with the load of
the machine: `large` may need a second run when it is slow.

### Tested (build chroot, results of the last run)

| Scenario | What was verified on disk / on screen |
|---|---|
| smoke (`tests/desktop/smoke.sh --dir files files`) | Both launch lines pass with no QML warnings. |
| `ops` | Copy+paste of a tree, inline rename, new folder, Delete → Trash (`.trashinfo` with `Path` and `DeletionDate`), undo restores it, cut+paste moves a folder, Shift+Delete asks first; Escape keeps the file, "Delete" removes it without touching the Trash; the source tree is unchanged. |
| `conflict` | Keep both (`a (1).txt`), Replace, Skip, Rename…, Stop, Replace for all, Skip for all — each with the expected file contents; folder-onto-folder offers Merge. |
| `large` | 600 MiB file plus 2000 small files: progress indicator and operations list (item, amounts, speed) while copying, switching views and opening/closing a tab during the copy, then SHA-256 of the big file and `diff -r` of the tree identical. |
| `cancel` | A copy of 4096 files cancelled from the operations list stops (file count no longer grows), silently; what was already copied stays. |
| `trash` | Three items trashed, `trash:/` view, Restore of one item to its original folder, Empty the Trash (confirmation) leaves `files/` and `info/` empty. |
| `archive` | tar.gz of two items and zip of a folder (member lists checked with `bsdtar -tf`, a file name starting with `-` included), an existing archive name is refused in the dialog, Extract here for both (multi-entry archive → folder named after it, taken name → `docs (1)`), `diff -r` against the originals, a file that is not an archive reports bsdtar's error and leaves nothing behind. |
| `views` | Grid, hidden files, details view, preview panel for an image, a text file and a binary file, filter, recursive search and opening a result, two tabs, split view, closing a tab. |
| `props` | Properties of a file and a folder; 640 → 744 on a file; recursive 750 → 755 on a folder (inner folder 755, files 600 → 644), checked with `stat`. |
| `errors` | Immutable folder (`chattr +i`) as root: recognised as read-only (no Paste/New folder), Trash, permanent delete and rename each fail with a visible message and the file stays; a missing folder shows an error state; renaming onto an existing name asks instead of overwriting. |
| `perms-user` | As `nobody`: read-only folder marker, unreadable source file → Skip/Retry/Stop dialog and the readable file is copied, delete / trash / rename inside a folder without write permission fail with "Access denied", paste is not possible there, a folder without `x` permission shows "You do not have permission…", foreign files explain why their permissions cannot be changed. |
| `dnd` | Drag onto a folder (move), Ctrl+drag (copy), drag onto the sidebar Trash, drag between the panes of the split view, a drop onto the item's own folder does nothing. |
| `launch` | F4 and the folder menu start `nexus-terminal --workdir <dir>` (path with a space included; process list checked), "Open with…" starts the chosen application with the file, "always use" writes `mimeapps.list` and Return then uses it. |
| `ftp` | Against `tests/ui/ftpd.py`: Connect dialog (protocol, host, typed port), keep in sidebar, listing, download of a file and a folder (compared with `cmp`), upload, new folder, rename and permanent delete on the server. |
| `ftp-auth` | Server that requires a password: honest error while the password service cannot start; with the session's display exported to the bus, KIO's prompt appears, dismissing it shows "Opening this location was cancelled" (not an empty folder), a wrong password offers a retry, the right one lists the folder. |
| `devices` | Solid **fake hardware backend** (`SOLID_FAKEHW=tests/ui/fakehw.xml`): list of a USB stick, an internal partition and an optical disc, `--dump devices`, click mounts and opens, unmount, eject, context menus. |
| `devices-udisks.sh` | `--dump devices` without a system bus, with a bus but no UDisks2, and with a real `udisksd` on a private system bus: three distinct, accurate "why is the list empty" messages. |
| `--dump` | `list` compared with `ls -la` (names, sizes, modes, hidden flag, symlink), `places`, `protocols` (ftp/webdav available, sftp/smb/nfs/fish not, as installed in the chroot), unknown topic and missing folder exit with status 2. |

### Not tested

* Mounting, unmounting and ejecting **real** block devices through UDisks2,
  hot-plugging, polkit prompts for mounting: the chroot has no udev, no block
  devices and no polkit agent. Only the application's side of it was exercised
  (fake backend) plus the daemon-present/absent states.
* SFTP, SMB, NFS and FISH (`kio-extras` is not installed in the chroot) and
  WebDAV (no test server); only their "not installed" state and FTP were run.
* Drag and drop between two windows/applications in the Wayland session,
  dragging over tabs, thumbnails from optional thumbnailer packages, the light
  theme, volumes with their own `.Trash-$uid`, copying across file systems,
  running out of disk space, and behaviour on real hardware speeds (the
  chroot renders in software on a loaded machine).
