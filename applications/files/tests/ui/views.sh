# SPDX-License-Identifier: GPL-3.0-or-later
# Views: grid/list, hidden files, filter, recursive search, preview panel,
# tabs, split view, properties.
T=/tmp/files-test-views; rm -rf "$T"; mkdir -p "$T/work/sub/deep" "$T/work/photos"
convert -size 640x400 gradient:navy-orange "$T/work/picture.png"
convert -size 300x300 plasma:fractal "$T/work/photos/plasma.jpg"
printf 'First line of the notes\nSecond line\nThird line with umlauts: äöü\n' > "$T/work/notes.txt"
printf '#!/bin/sh\necho hello\n' > "$T/work/script.sh"; chmod 644 "$T/work/script.sh"
echo secret > "$T/work/.hidden-file"; echo found > "$T/work/sub/deep/needle-report.txt"; echo x > "$T/work/sub/needle.log"
head -c 3000 /dev/urandom > "$T/work/data.bin"
app_start "$T/work"
sleep 1; snap 01-grid
key ctrl+h; sleep 0.8; snap 02-hidden
key ctrl+h ctrl+2; sleep 0.8; snap 03-list
key alt+p; sleep 0.5; type_text "pi"; sleep 1.5; snap 04-preview-image
sleep 1; key n; sleep 1.5; snap 05-preview-text
sleep 1; key d; sleep 1.2; snap 06-preview-binary
key ctrl+1 alt+p
key ctrl+f; type_text "no"; sleep 0.6; snap 07-filter
key ctrl+a; type_text "needle"; key Return; sleep 2.5; snap 08-search
key Down Return; sleep 1.2; snap 09-search-opened
key ctrl+t; goto /usr/share; snap 10-tabs
key F3; sleep 1; goto "$T/work/sub"; snap 11-split
key F6; sleep 0.5; key ctrl+w; sleep 0.8; snap 12-tab-closed
app_stop
