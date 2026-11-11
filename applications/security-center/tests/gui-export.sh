#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# GUI test of navigation and report export in the nested session (read-only:
# the status tool is run directly from the source tree, nothing is changed).
#
#   scripts/dev.sh nested --dir seccenter --size 1280x860 --wait 1 -- \
#       '. /src/applications/security-center/tests/gui-export.sh'
click() { xdo mousemove "$1" "$2" click 1; sleep "${3:-1}"; }
shadow-security-center --tools-dir /src/security > "$SHADOW_LOGS/gui-export-app.log" 2>&1 &
sleep 10
shot seccenter-ex-1-overview.png
# Tile "Firewall" → Firewall page; sidebar back to the overview.
click 420 200 1.5; shot seccenter-ex-2-tile-link.png
click 100 127 1.5
# Copy report → "Copy as text".
click 971 78 1.5; shot seccenter-ex-3-copy-menu.png
click 940 119 1.2; shot seccenter-ex-4-copied-toast.png
# Save… → dialog. Pasting the clipboard into the file-name field shows what
# "Copy as text" put there (the chroot has no wl-paste).
click 1074 78 2; shot seccenter-ex-5-save-dialog.png
xdo key ctrl+a; xdo key ctrl+v; sleep 1; shot seccenter-ex-6-clipboard-pasted.png
click 763 533 1
# Save as JSON, then as text; keep copies of the written files.
click 1074 78 2; click 535 436 1; shot seccenter-ex-7-save-json.png; click 852 533 1.5; shot seccenter-ex-8-saved-toast.png
click 1074 78 2; click 471 436 1; click 852 533 1.5
ls -l "$HOME"/shadow-security-report-* > "$SHADOW_LOGS/gui-export-files.log" 2>&1
cp "$HOME"/shadow-security-report-* "$SHADOW_LOGS/" 2>/dev/null
# Saving again under the same name: the dialog warns before replacing.
click 1074 78 2; shot seccenter-ex-9-save-exists.png; click 763 533 1
# Every page through the sidebar.
n=0
for y in 235 273 311 349 387 457 495 533; do
    n=$((n+1)); click 100 "$y" 1.5; shot "seccenter-ex-page-$n.png"
done
