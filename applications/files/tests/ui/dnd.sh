# SPDX-License-Identifier: GPL-3.0-or-later
# Drag and drop inside the window: onto a folder, with Ctrl, onto the sidebar
# Trash, between the panes of the split view, and a drop that must do nothing.
T=/tmp/files-test-dnd; rm -rf "$T"; mkdir -p "$T/work/box" "$T/work/crate"
echo one > "$T/work/one.txt"; echo two > "$T/work/two.txt"; echo three > "$T/work/three.txt"
app_start "$T/work"; snap 0-start; echo "0 start: $(tree $T/work)"
# tiles (139 px apart): box 321, crate 460, one.txt 599, three.txt 738, two.txt 877
drag 599 113 321 113
echo "1 drag one.txt -> box (same file system: move)  : $(tree $T/work)"
drag 599 113 460 113 ctrl
echo "2 Ctrl+drag three.txt -> crate (copy)           : $(tree $T/work)"
drag 738 113 68 88
echo "3 drag two.txt -> sidebar Trash                 : $(tree $T/work)| trash: $(ls $HOME/.local/share/Trash/files 2>/dev/null | tr '\n' ' ')"
key F3; sleep 1.2; key b Return; sleep 1; snap 1-split
drag 489 178 1000 500
echo "4 drag crate from the left pane into the right  : $(tree $T/work)"
snap 2-after
click 331 178; drag 331 178 400 520
echo "5 drag box onto its own folder's background     : $(tree $T/work)"
app_stop
