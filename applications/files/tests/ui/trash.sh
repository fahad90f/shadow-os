# SPDX-License-Identifier: GPL-3.0-or-later
# Trash (freedesktop.org trash specification): trash, view, restore, empty.
T=/tmp/files-test-trash; rm -rf "$T"; mkdir -p "$T/work/dir"
echo one > "$T/work/one.txt"; echo two > "$T/work/two.txt"; echo in > "$T/work/dir/inner.txt"
TR="$HOME/.local/share/Trash"
app_start "$T/work"
key ctrl+a Delete; sleep 2
echo "1 trashed   : work=[$(tree $T/work)] files=[$(ls $TR/files | tr '\n' ' ')] info=[$(ls $TR/info | tr '\n' ' ')]"
echo "  one.txt.trashinfo: $(tr '\n' '|' < $TR/info/one.txt.trashinfo)"
click 68 88; sleep 1.5          # sidebar: Trash
snap 1-view
key o; sleep 0.3; park; key Menu; sleep 0.6; snap 2-menu; key Down Return; sleep 2
echo "2 restored one.txt: work=[$(tree $T/work)] files=[$(ls $TR/files | tr '\n' ' ')] info=[$(ls $TR/info | tr '\n' ' ')]"
click 68 88 3; sleep 0.5; key Down Down Down Return; sleep 0.8   # Trash > Empty the Trash…
snap 3-empty-confirm
click "${EMPTY_X:-767}" "${EMPTY_Y:-439}"; sleep 2.5
echo "3 emptied   : files=[$(ls $TR/files | tr '\n' ' ')] info=[$(ls $TR/info | tr '\n' ' ')]"
snap 4-emptied
app_stop
