#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs one UI scenario of NEXUS Files in the nested NEXUS HORIZON session
# (KWin/Wayland inside Xvfb) of the build chroot:
#
#   applications/files/tests/run-ui.sh <scenario> [build-dir]
#
# Scenarios are tests/ui/<scenario>.sh; they drive the real window with
# xdotool, print the on-disk state after every step and save screenshots to
# build/work/shots/files-*.png. A nested session is ended after 180 seconds,
# so every scenario has to fit into that. Nothing here is installed.
set -e
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
scn="${1:?usage: run-ui.sh <scenario> [build-dir]}"
dir="${2:-files}"
[ -f "$root/applications/files/tests/ui/$scn.sh" ] || { echo "no such scenario: $scn" >&2; exit 2; }
exec "$root/scripts/dev.sh" nested --dir "$dir" --size 1600x900 --wait 1 -- \
    "SCN=$scn; . /src/applications/files/tests/ui/lib.sh; . /src/applications/files/tests/ui/$scn.sh"
