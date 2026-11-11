# SPDX-License-Identifier: GPL-3.0-or-later
# Name conflict dialog: keep both, replace, skip, rename, stop; "apply to all".
T=/tmp/files-test-conflict
setup() { rm -rf "$T"; mkdir -p "$T/src" "$T/dst"; echo new-content > "$T/src/a.txt"; echo old > "$T/dst/a.txt"; echo extra > "$T/src/b.txt"; [ -n "$1" ] && echo old-b > "$T/dst/b.txt"; sleep 0.5; }
state() { for f in "$T"/dst/*; do printf "%s='%s' " "$(basename "$f")" "$(cat "$f")"; done; }
paste_src_into_dst() { goto "$T/src"; key ctrl+a ctrl+c; goto "$T/dst"; key ctrl+v; sleep 1.8; }
setup; app_start "$T/src"
paste_src_into_dst; snap 1-dialog; click 728 493; sleep 1.5
echo "keep both      : $(state)"
setup; paste_src_into_dst; click 825 493; sleep 1.5
echo "replace        : $(state)"
setup; paste_src_into_dst; click 533 493; sleep 1.5
echo "skip           : $(state)"
setup; paste_src_into_dst; click 624 493; sleep 0.8; snap 2-rename
key ctrl+a; type_text "mine.txt"; key Return; sleep 1.5
echo "rename         : $(state)"
setup; paste_src_into_dst; click 410 493; sleep 1.5; snap 2b-after-stop
echo "stop           : $(state)"
setup both; paste_src_into_dst; click 387 447; click 825 493; sleep 1.5; snap 3-after-replace-all
echo "replace, all   : $(state)"
# with the box ticked "Rename…" is hidden and "Skip" moves right
setup both; paste_src_into_dst; click 387 447; click 636 493; sleep 1.5
echo "skip, all      : $(state)"
# a folder merged into an existing folder of the same name
rm -rf "$T"; mkdir -p "$T/src/box" "$T/dst/box"; echo one > "$T/src/box/one.txt"; echo kept > "$T/dst/box/kept.txt"; sleep 0.5
paste_src_into_dst; snap 4-folder-conflict
app_stop
