#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs one UI scenario of SHADOW Files as an unprivileged user (nobody) under a
# plain virtual X server in the build chroot — for everything that needs real
# permission errors, which root never gets:
#
#   applications/files/tests/run-user.sh <scenario> [build-dir]
#
# The scenario's fixture function `fixture` runs as root first, the rest as
# nobody. Screenshots go to build/work/shots/files-*.png.
set -e
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
scn="${1:?usage: run-user.sh <scenario> [build-dir]}"
dir="${2:-files}"
[ -f "$root/applications/files/tests/ui/$scn.sh" ] || { echo "no such scenario: $scn" >&2; exit 2; }
exec "$root/scripts/dev.sh" exec --dir "$dir" -- bash /src/applications/files/tests/ui/user-session.sh "$scn" "$dir"
