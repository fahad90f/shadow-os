# NEXUS Archive Manager

`nexus-archive-manager` (application id `org.nexus.archivemanager`) opens,
extracts, tests, creates and edits archives. It is a front end to two
command-line tools, `bsdtar` and `7z`; its own job is to drive them safely and
to decide what may be written to disk.

## Formats

| Format | Read with | Browse / extract / test | Create | Add / delete entries | Password |
|---|---|---|---|---|---|
| ZIP | `7z` | yes | yes | yes | read and write — **AES-256** (WinZip AE-2, `-mem=AES256`). Contents are encrypted; names, sizes and dates stay readable. The legacy ZipCrypto cipher is never written. |
| 7-Zip | `7z` | yes | yes | yes | read and write — **AES-256**, key derived from the password with SHA-256 (7zAES); optionally the file names too (`-mhe=on`). |
| tar, tar.gz, tar.bz2, tar.xz, tar.zst | `bsdtar` | yes | yes | yes, by writing a new archive (see below) | none — the dialog says so |
| CPIO | `bsdtar` | yes | yes (`newc`) | no | none |
| ISO 9660 | `7z` | yes | no | no | — |
| RAR | `bsdtar` | read-only, **untested** (see below) | no | no | encrypted RAR is not supported |

A lone compressed file (`file.txt.gz`) is not an archive and is declined with
that explanation. Formats are recognised by content (magic numbers), not by
file name.

## Command line (used by the file manager)

```
nexus-archive-manager [<archive>]                  browse
nexus-archive-manager --extract <archive>          window with the extraction dialog
nexus-archive-manager --extract <archive> --to <dir>
                                                   extract into <dir> at once, show progress, quit when done;
                                                   stays open if anything needs a decision (password, low disk
                                                   space, suspected bomb) or was skipped for safety
nexus-archive-manager --create <output> <files…>   "New archive" dialog pre-filled (format from the name of
                                                   <output>); quits after the archive is written

Without a window (for scripts and tests):
nexus-archive-manager --headless --extract <archive> [--to <dir>] [--entry <path>]… [--overwrite rename|skip|overwrite] [--force]
nexus-archive-manager --headless --create <output> <files…> [--format <id>] [--level store|fast|normal|best] [--encrypt-names]
nexus-archive-manager --list <archive>             JSON: format, sizes, every entry with its hazards
nexus-archive-manager --test <archive>             integrity test
nexus-archive-manager --add <archive> <files…>     add to the top level
nexus-archive-manager --delete <archive> <entries…>
   … --password-stdin                              read the password (one line) from standard input
nexus-archive-manager --dump formats|tools
```

* `--extract` without `--to` ("extract here"): a single top-level item is
  placed next to the archive, anything else in a folder named after the
  archive. With `--to` the content goes directly into that folder.
* Existing files are never replaced unless `--overwrite overwrite` (or the
  dialog's "Replace") is chosen; the default keeps both (`name (2).ext`).
* Exit codes: 0 success · 1 failure · 2 usage · 3 refused (suspected
  decompression bomb without `--force`, or not enough free space) ·
  4 password needed or wrong.
* There is no option that takes a password as an argument.

## Security model

**Nothing a tool writes goes directly into the user's folder.**

1. The tool unpacks into a private staging directory (mode 0700,
   `.nexus-extract-XXXXXX`) created *inside* the destination, with the tool's
   own protections left on: `bsdtar` without `-P` (refuses `..`, strips a
   leading `/`, will not write through a symbolic link) plus
   `--no-same-owner --no-same-permissions --no-acls --no-xattrs --no-fflags`;
   `7z x` with its default link checks.
2. `safemerge.cpp` then walks what really arrived on disk — `lstat`, never
   following a link — and moves into the destination only:
   * regular files and directories, with set-user-ID, set-group-ID and sticky
     bits removed;
   * symbolic links whose target is relative and stays inside the extraction
     folder.

   Device nodes, FIFOs, sockets, absolute and escaping links are left behind
   and listed in the result. A hard-linked file is copied apart first, so a
   link a tool might have been tricked into creating can never expose or
   modify a file elsewhere. A directory from the archive is merged only into a
   *real* directory of the destination — an existing symbolic link there is
   never extracted through.
3. If the tool reports anything other than the known "entry refused" messages,
   the staging directory is discarded and nothing is added to the destination.

The listing (parsed from tool output) is used for display, for size estimates
and to name the entries to extract — never to decide what is safe.

Other measures:

* **Decompression bombs** — an archive that expands to ≥ 100 MiB *and* ≥ 100×
  its size is flagged in the window; extracting needs an explicit tick (GUI)
  or `--force` (headless). Extraction is refused if the destination lacks the
  space. While a tool runs, the real size of the staging directory is measured
  every second: extraction is killed when it exceeds what the archive declared
  (by more than 25 % + 64 MiB) or when less than 32 MiB of space remains.
  Bombs nested inside an archive (an archive in an archive) are not detected.
* **Passwords** are kept in memory while the archive is open and handed to
  `7z` on its **standard input** (`7z a … -p` asks; `x`/`t`/`l`/`d` ask when
  needed). They never appear in an argument vector or the environment.
  `zip -P`, `7z -pSECRET` and `bsdtar --passphrase`, which would expose them in
  the process list, are not used; that is why encrypted ZIP goes through `7z`.
* **No shell**; every tool is run with an argument vector, `LC_ALL=C.UTF-8`.
  File arguments that could be read as options or as bsdtar's `@archive` syntax
  are passed as `./name`; bsdtar pattern characters in entry names are escaped.
* **Preview** extracts the one file through the same staging procedure into a
  private directory under `$XDG_RUNTIME_DIR` (0700), makes it read-only and
  opens it with `xdg-open` (or `gio open`). Programs, shared libraries and
  `.desktop` launchers are not opened. The directory is removed when the
  archive is closed. The launcher's exit status is checked; "nothing can open
  this" is reported as such.
* **Changing an archive**: ZIP/7z are updated by `7z` (which writes a new file
  and renames it). Tar has no index, so adding or deleting writes a complete
  new archive with `bsdtar … @old` to `.<name>.nexus-part` and renames it over
  the original when it is complete; the original is untouched if anything
  fails. New archives are also written under a temporary name first.
* **gzip integrity**: libarchive does not verify the CRC at the end of a gzip
  stream (found in testing: a corrupted `.tar.gz` extracted "successfully").
  Before testing or extracting a `.tar.gz` the stream is therefore checked
  with `7z t -tgzip`; the result is cached while the file is unchanged.

## Parsed tool output

* `7z l -slt -sccUTF-8` — "Key = Value" blocks (machine-oriented).
* `bsdtar -tv --numeric-owner` — bsdtar offers no machine-readable listing
  (its mtree output rewrites paths and merges entries, hiding exactly the
  dangerous ones). The `ls`-style lines are parsed in the C locale with numeric
  owners, and its `\ooo` escapes are undone. Consequences: modification times
  have the resolution `ls` prints (minutes for recent entries, days for older
  ones), and a crafted name containing ` -> ` or ` link to ` can be *displayed*
  split in the wrong place. Extraction does not depend on it.
* Progress: `7z -bsp1` percentages; for `bsdtar`, the count of `-v` lines
  against the number of entries (creating a tar shows the current file only).

## Drag and drop

Dropping files on the window is implemented (open an archive, add to the open
archive after a confirmation, or start a new archive) but **could not be
tested** — see below. Dragging entries *out* of the window is not implemented:
it would need the files extracted before the drop target is known, and was
left out rather than faked.

## Run-time dependencies (Debian packages)

`libarchive-tools` (bsdtar), `7zip` (provides `7z`; `p7zip-full` is the
transitional name), `xdg-utils` (preview; `gio` from `libglib2.0-bin` is used
if `xdg-open` is missing), `libqt6concurrent6`, `libqt6quick6`,
`qml6-module-qtquick-controls`, `qml6-module-qtquick-layouts`,
`qml6-module-qtcore`, the NEXUS UI library. Optional: `7zip-rar` is *not*
used. `zip`/`unzip` and KArchive are not needed.
A missing tool is reported per format (`--dump tools`, and in the dialogs).

## Limitations

* RAR depends on what libarchive supports (RAR 4 and most RAR 5, no
  encryption, no recovery records). **No RAR file was available to test.**
* ISO: `7z` reads ISO 9660 and Joliet but not Rock Ridge; discs with only
  Rock Ridge names show the short ISO names, symbolic links arrive as small
  files, and characters such as `*` are replaced on extraction.
* A ZIP that stores a symbolic link and then a file "inside" it is rejected as
  a whole by `7z` (nothing is extracted) rather than extracted partially.
* ZIP entries whose names contain `*` or `?` cannot be addressed individually
  with `7z` (they are wildcards there): extracting or deleting such an entry
  may also match other names fitting the pattern.
* Adding always puts items at the top level of the archive. A tar archive
  cannot receive an item whose name already exists at the top level (delete it
  first); for ZIP/7z the existing entry is replaced.
* Rewriting a tar for add/delete recompresses with the default level and
  stores entries in pax format.
* Hard links inside an archive are extracted as independent copies.
* Listing a compressed tar means decompressing it once (no index), a very
  large one takes time; it can be cancelled.
* Multi-volume archives, self-extracting archives and archive comments are not
  handled. No "open with…" for previews, no in-archive rename or move.
* If the application is killed during extraction, a `.nexus-extract-XXXXXX`
  folder may remain in the destination.
* The file chooser is the application's own simple dialog (no bookmarks or
  filters).

## Tested (build chroot, Debian 13, bsdtar 3.7.4, 7-Zip 25.01, as root)

Build: `NEXUS_JOBS=2 scripts/dev.sh build --dir arced --only "archive-manager;editor"`.
Smoke: `tests/desktop/smoke.sh --dir arced archive-manager` — PASS, zero QML warnings.

Headless, with archives built for the purpose (Python `tarfile`/`zipfile`):

* **Traversal and special entries** (`evil.tar.gz`): `../x`, `a/../../../x`,
  an absolute path, a symlink to an outside directory followed by a file
  "inside" it, a relative symlink leaving the root, a hard link to
  `/etc/passwd`, a set-user-ID file, a character device, a FIFO, names with
  newline / non-ASCII. Result: nothing outside the destination; the 8
  dangerous entries listed by name; set-user-ID bit gone; inner relative
  symlink kept; `--list` flags 9 entries.
* `evil.zip` (same ideas): rejected as a whole, destination empty.
  ZIP with `..\..\x` and `dir/../../x`: both stay inside.
* Symlink loop (`loop -> .`), `up -> ..` with a file through it, hard-linked
  pair: nothing escapes; hard links arrive as two files with link count 1.
* Destination that already contains a symlink named like an archive folder:
  not written through, target directory stays empty.
* Bombs: 200 MiB of zeros in a 200 KiB ZIP and 300 MiB in a 46 KiB tar.xz →
  exit 3; with `--force` extracted. A ZIP whose index declares 1 KiB for a
  400 MiB entry → 7z reports a data error, nothing extracted. (The run-time
  size guard itself never had to fire: no test archive got past the tools.)
* Round trip of a tree (300 KiB random file, 50 000-line text, names with
  spaces, `*`, `[1]`, a leading `-`, a symlink) through create → list → test →
  extract for zip, 7z, tar, tar.gz, tar.bz2, tar.xz, tar.zst, cpio: checksums
  identical for all eight. Compression levels change the size as expected.
* `--entry` selections containing pattern characters, spaces and a symlink
  (zip, tar.gz, 7z).
* ISO made with `bsdtar --format=iso9660`: list, extract, test.
* Damaged archives: `.tar.gz` and `.zip` with 16 bytes overwritten → test and
  extraction fail, nothing extracted; truncated `.tar.xz` → clear error.
* Encryption: ZIP created with a password is AES-256 (`7z l -slt`); extract
  without password → exit 4, wrong password → exit 4, right password →
  content identical; 7z with encrypted names: listing needs the password and
  the names are not readable in the file; adding to an encrypted ZIP keeps the
  new entry encrypted; tar + password is refused.
  The argument vector and environment of the running `7z` were read from
  `/proc` during a long encrypted creation: password in neither.
* Add and delete (folders, pattern characters) for zip, 7z, tar.gz, tar, each
  followed by list and test; duplicate name in a tar refused; CPIO refused as
  read-only; no temporary files left behind.
* Conflict policies skip / rename / overwrite; "extract here" with one and
  with several top-level items.

In the nested KWin Wayland session, driven with `xdotool` and checked on
screenshots and on disk: browsing into a folder; "Extract all" through the
dialog; selecting two entries and extracting them relative to the current
folder; integrity test dialog; `--create` batch dialog with password, repeat
and "encrypt file names" → archive verified headless, process exits;
opening it → password dialog, wrong password → retry, right password →
listing with lock icons; `--extract --to` batch run → files present, process
exits; bomb warning in the extraction dialog; delete with confirmation (tar.gz
rewritten); add through the file chooser; preview of a text file opening in
NEXUS Text Editor through `gio open`; preview with no application registered →
error dialog. Light and dark screenshots.

## Not tested

* RAR archives (none available, no tool to make one).
* Drag and drop in either direction (no drag source could be driven in the
  test session).
* Cancelling a running job from the GUI (the code path is the same `kill` used
  by the guards, but the button itself was not pressed in a test).
* Archives larger than a few hundred MiB; non-root users (the tests ran as
  root, which is the harder case for set-user-ID bits and device nodes).
* Launch through the `.desktop` `MimeType=` association from a file manager.
