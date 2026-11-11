#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# GUI test of navigation and report export in the nested session (read-only:
# the status tool is run directly from the source tree, nothing is changed).
#
#   scripts/dev.sh nested --dir seccenter --size 1280x860 --wait 1 -- \
#       '. /src/applications/security-center/tests/gui-export.sh'
click() { xdo mousemove "$1" "$2" click 1; sleep "${3:-1}"; }
nexus-security-center --tools-dir /src/security > "$NEXUS_LOGS/gui-export-app.log" 2>&1 &
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
