# SPDX-License-Identifier: GPL-3.0-or-later
#
# Result-window steps for nested-test.sh (GUI_STEPS=/src/applications/screenshot/tests/gui-steps.sh):
# annotate with every tool, undo/redo, crop, Save, Save as, Copy, close while
# holding the clipboard, region capture from the window, Open folder.
# Coordinates are for the 1600x900 nested session, where KWin centres the
# 1120x780 window with its content starting at (403,105).
echo "--- result window"
PICS="$HOME/Pictures/Screenshots"
nexus-screenshot 2>"$OUT/gui.err" & gui=$!
sleep 7
shot disks-gui-1-result.png
tool() { xdo mousemove "$1" 805 click 1; sleep 0.6; }
drag() { xdo mousemove "$1" "$2" mousedown 1 mousemove $(( ($1+$3)/2 )) $(( ($2+$4)/2 )) mousemove "$3" "$4" mouseup 1; sleep 0.8; }
save_as() { xdo mousemove 1463 852 click 1; sleep 1.5; cp "$(ls -t "$PICS"/* | head -n1)" "$OUT/$1"; }

tool 626; drag 700 300 1215 520                 # rectangle (red)
xdo mousemove 982 805 click 1; sleep 0.4        # blue
tool 535; drag 1380 650 1225 530                # arrow
tool 717; xdo mousemove 470 230 click 1; sleep 1; xdo type --delay 30 "Kernel version"; sleep 0.5
shot disks-gui-2-text-popup.png
xdo key Return; sleep 0.8
tool 802; drag 1090 347 1200 362                # pixelate the kernel version
xdo mousemove 900 790; sleep 0.5
shot disks-gui-3-annotated.png
save_as gui-annotated.png
same_not() { local n; n="$(compare -metric AE "$2" "$3" null: 2>&1)"; n="${n%% *}"; if [ "${n:-0}" -gt "$4" ] 2>/dev/null; then ok "$1 (differing pixels: $n)"; else bad "$1 (differing pixels: $n)"; fi; }
same_not "annotations are in the saved file" "$OUT/gui-annotated.png" /shots/disks-ref.png 2000
for _ in 1 2 3 4; do xdo mousemove 1071 805 click 1; sleep 0.4; done
save_as gui-undone.png
same "four undo steps restore the capture exactly" "$OUT/gui-undone.png" /shots/disks-ref.png
for _ in 1 2 3 4; do xdo mousemove 1116 805 click 1; sleep 0.4; done
save_as gui-redone.png
same "four redo steps bring the annotations back" "$OUT/gui-redone.png" "$OUT/gui-annotated.png"
tool 455; drag 683 213 1241 728                 # crop to the System Information window
shot disks-gui-4-cropped.png
save_as gui-cropped.png
identify -format "      cropped file: %wx%h %m\n" "$OUT/gui-cropped.png"
w="$(identify -format %w "$OUT/gui-cropped.png")"
[ "$w" -gt 800 ] && [ "$w" -lt 900 ] && ok "crop reduced the picture to the selection" || bad "unexpected crop width $w"
ls "$PICS" | sed 's/^/      /'
ls "$PICS" | grep -Eq '^Screenshot_[0-9]{4}-[0-9]{2}-[0-9]{2}_[0-9]{2}-[0-9]{2}-[0-9]{2}(_[0-9]+)?\.png$' && ok "Save uses ~/Pictures/Screenshots with a timestamp name" || bad "unexpected file names"

xdo mousemove 1267 852 click 1; sleep 1.2       # Save as…
xdo key ctrl+a; xdo type --delay 15 "$OUT/gui-saved-as.webp"; sleep 0.5
shot disks-gui-5-save-as.png
xdo key Return; sleep 1.5
file "$OUT/gui-saved-as.webp" | grep -q "Web/P" && ok "Save as… wrote a WebP file" || bad "Save as… did not write WebP"

xdo mousemove 1366 852 click 1; sleep 1         # Copy
shot disks-gui-6-copied.png
nexus-screenshot-cliptest read "$OUT/gui-pasted.png"; expect_rc "Copy: another client pastes the picture" $? 0
same "pasted picture equals the saved, cropped file" "$OUT/gui-pasted.png" "$OUT/gui-cropped.png"

xdo mousemove 857 138 click 1; sleep 0.5        # Region mode
xdo mousemove 1453 138 click 1; sleep 3.5       # Capture: window hides, overlay appears
shot disks-gui-7-region-overlay.png
xdo mousemove 420 190 mousedown 1 mousemove 600 300 mousemove 1180 520 mouseup 1; sleep 2.5
shot disks-gui-8-region-result.png
save_as gui-region.png
convert /shots/disks-ref.png -crop 760x330+420+190 +repage "$OUT/gui-region-ref.png"
same "region captured from the window equals that area of the screen (own window not in it)" "$OUT/gui-region.png" "$OUT/gui-region-ref.png"

xdo mousemove 1366 852 click 1; sleep 1         # Copy again, then close the window
xdo mousemove 1505 87 click 1; sleep 2
kill -0 $gui 2>/dev/null && ok "closing the window keeps the process while it holds the clipboard" || bad "process exited although it holds the clipboard"
nexus-screenshot-cliptest read "$OUT/gui-pasted-after-close.png"; expect_rc "paste still works after the window was closed" $? 0
nexus-screenshot-cliptest text "bye" >/dev/null & replacer=$!
sleep 1.5; xdo mousemove 800 450 click 1; wait $replacer
for _ in 1 2 3 4 5 6; do kill -0 $gui 2>/dev/null || break; sleep 1; done
kill -0 $gui 2>/dev/null && bad "process still running after the clipboard was replaced" || ok "process ends once the clipboard is replaced"
grep -v "^$" "$OUT/gui.err" | head -5
