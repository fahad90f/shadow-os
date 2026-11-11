# SPDX-License-Identifier: GPL-3.0-or-later
#
# NEXUS Screenshot in the nested Wayland session. Sourced by the session
# shell (it uses the session's "shot" and "xdo" functions):
#
#   scripts/dev.sh nested --dir disks --wait 1 -- '. /src/applications/screenshot/tests/nested-test.sh'
#
# KWin in this session composites with QPainter and therefore has no
# org.kde.KWin.ScreenShot2; the pixels come from tests/fakekwin.cpp, which
# grabs the X server that displays the session and speaks KWin's protocol.
OUT="$NEXUS_LOGS/screenshot"; rm -rf "$OUT"; mkdir -p "$OUT"
pass=0; fail=0
ok() { echo "PASS  $1"; pass=$((pass+1)); }
bad() { echo "FAIL  $1"; fail=$((fail+1)); }
expect_rc() { if [ "$2" = "$3" ]; then ok "$1 (exit $2)"; else bad "$1 (exit $2, expected $3)"; fi; }
# same <name> <a> <b> [max differing pixels]
same() {
    local n; n="$(compare -metric AE "$2" "$3" null: 2>&1)"; n="${n%% *}"
    if [ "${n:-x}" -le "${4:-0}" ] 2>/dev/null; then ok "$1 (differing pixels: $n)"; else bad "$1 (differing pixels: $n)"; fi
}

echo "--- without a capture back-end (real KWin here: QPainter compositing)"
nexus-screenshot --dump backends
nexus-screenshot --full --output "$OUT/none.png" 2>"$OUT/none.err"; expect_rc "no back-end: capture fails honestly" $? 1
cat "$OUT/none.err"

DISPLAY="$NEXUS_NESTED_X" QT_QPA_PLATFORM=xcb nexus-screenshot-fakekwin >"$OUT/fakekwin.log" 2>&1 &
export NEXUS_SCREENSHOT_KWIN_SERVICE=org.nexus.test.FakeKWin
nexus-sysinfo >/dev/null 2>&1 &
sleep 6
nexus-screenshot --dump backends | grep -E "kwin|clipboard"
nexus-screenshot --list-monitors
SCREEN="$(nexus-screenshot --list-monitors | cut -f1 | head -n1)"
echo "screen: $SCREEN"

echo "--- command line"
shot disks-ref.png
nexus-screenshot --full --output "$OUT/full.png"; expect_rc "--full --output png" $? 0
same "full.png equals the X screenshot" "$OUT/full.png" /shots/disks-ref.png
nexus-screenshot --monitor "$SCREEN" --output "$OUT/monitor.png"; expect_rc "--monitor --output" $? 0
same "monitor.png equals the X screenshot" "$OUT/monitor.png" /shots/disks-ref.png
nexus-screenshot --full --output "$OUT/full.jpg"; expect_rc "--output jpg" $? 0
nexus-screenshot --full --output "$OUT/full.webp"; expect_rc "--output webp" $? 0
file "$OUT/full.png" "$OUT/full.jpg" "$OUT/full.webp" | sed "s|$OUT/||"
start=$(date +%s); nexus-screenshot --full --delay 3 --output "$OUT/delay.png"; rc=$?; took=$(( $(date +%s) - start ))
expect_rc "--delay 3" $rc 0; [ "$took" -ge 3 ] && ok "--delay 3 waited ${took}s" || bad "--delay 3 returned after ${took}s"
nexus-screenshot --window --output "$OUT/window.png" 2>"$OUT/window.err"; expect_rc "--window (test double has no windows: honest failure)" $? 1
cat "$OUT/window.err"
nexus-screenshot --full --output /proc/nexus-no-such-dir/x.png 2>/dev/null; expect_rc "unwritable --output" $? 4
nexus-screenshot --full --output "$OUT/x.bmp" 2>/dev/null; expect_rc "unsupported file type" $? 2
nexus-screenshot --full --region --output "$OUT/x.png" 2>/dev/null; expect_rc "conflicting modes" $? 2
nexus-screenshot --delay 99 --output "$OUT/x.png" 2>/dev/null; expect_rc "bad --delay" $? 2
nexus-screenshot --monitor NOPE --output "$OUT/x.png" 2>/dev/null; expect_rc "unknown monitor" $? 2

echo "--- region selection"
nexus-screenshot --region --output "$OUT/region.png" & region=$!
sleep 4
shot disks-region-1-overlay.png
xdo mousemove 300 200 mousedown 1 mousemove 400 260 mousemove 700 420 mousemove 900 560
sleep 1
shot disks-region-2-dragging.png
xdo mouseup 1
wait $region; expect_rc "--region --output" $? 0
convert /shots/disks-ref.png -crop 600x360+300+200 +repage "$OUT/region-ref.png"
identify -format "region.png is %wx%h\n" "$OUT/region.png"
same "region.png equals the same area of the X screenshot" "$OUT/region.png" "$OUT/region-ref.png"
nexus-screenshot --region --output "$OUT/cancelled.png" & region=$!
sleep 4; xdo key Escape
wait $region; expect_rc "--region cancelled with Escape" $? 3
[ -e "$OUT/cancelled.png" ] && bad "cancelled region wrote a file" || ok "cancelled region wrote no file"

echo "--- clipboard"
nexus-screenshot --full --clipboard; expect_rc "--clipboard returns" $? 0
sleep 1
pgrep -f "nexus-screenshot --full --clipboard" >/dev/null && ok "background process keeps the clipboard" || bad "no process holds the clipboard"
nexus-screenshot-cliptest read "$OUT/pasted.png"; expect_rc "another client pastes an image" $? 0
same "pasted image equals the X screenshot" "$OUT/pasted.png" /shots/disks-ref.png 2500
nexus-screenshot-cliptest text "replaced"; sleep 2
pgrep -f "nexus-screenshot --full --clipboard" >/dev/null && bad "holder still running after the clipboard was replaced" || ok "holder exits once the clipboard is replaced"

echo "screenshot tests: $pass passed, $fail failed"
