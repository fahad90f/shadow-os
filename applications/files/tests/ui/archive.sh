# SPDX-License-Identifier: GPL-3.0-or-later
# Create archives (tar.gz, zip) and extract them again with bsdtar.
T=/tmp/files-test-archive; rm -rf "$T"; mkdir -p "$T/work/docs"
echo alpha > "$T/work/docs/a.txt"; echo beta > "$T/work/docs/-dash.txt"; echo note > "$T/work/note.txt"
cp -r "$T/work" "$T/orig"
app_start "$T/work"
# both items > Compress… (7th entry of the multi-selection menu), default tar.gz
key ctrl+a; menu_pick 7; snap 1-dialog
key ctrl+a; type_text "bundle"; key Return; sleep 3
echo "1 tar.gz : work=[$(ls -A $T/work | tr '\n' ' ')] members=[$(bsdtar -tf $T/work/bundle.tar.gz 2>&1 | tr '\n' ' ')]"
# the folder alone as zip: 9th entry of a folder's menu; Tab to the format box, 3 down
sleep 1.2; key d; menu_pick 9
key ctrl+a; type_text "zipped"; key Tab Down Down Down; snap 2-zip
key shift+Tab Return; sleep 3
echo "2 zip    : work=[$(ls -A $T/work | tr '\n' ' ')] members=[$(bsdtar -tf $T/work/zipped.zip 2>&1 | tr '\n' ' ')] type=[$(file -b $T/work/zipped.zip | cut -c1-30)]"
# an existing name is refused in the dialog
sleep 1.2; key n; menu_pick 9
key ctrl+a; type_text "bundle"; sleep 0.5; snap 3-name-taken; key Escape
# Extract here (9th entry of an archive's menu)
sleep 1.2; key b; sleep 0.3; park; key Menu; sleep 0.6; snap 4-archive-menu; key Escape; menu_pick 9; sleep 3
echo "3 extract tar.gz (2 top-level entries -> folder): work=[$(ls -A $T/work | tr '\n' ' ')]"
diff -r "$T/orig" "$T/work/bundle" && echo "  bundle/ identical to the original tree"
sleep 1.2; key z; menu_pick 9; sleep 3
echo "4 extract zip (1 top-level entry, name taken -> docs (1)): work=[$(ls -A $T/work | tr '\n' ' ')]"
diff -r "$T/orig/docs" "$T/work/docs (1)" && echo "  'docs (1)' identical to docs"
# a file that is not an archive at all
printf 'this is not an archive' > "$T/work/broken.tar.gz"; sleep 1.5
key Escape; sleep 1.2; key b r; menu_pick 9; sleep 2.5
snap 5-broken
echo "5 broken archive: work=[$(ls -A $T/work | tr '\n' ' ')]"
app_stop
