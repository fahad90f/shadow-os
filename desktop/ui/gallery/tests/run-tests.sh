#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# NexusUI tests that need real pointer events (Qt Quick Test), run inside the
# build chroot against the libraries of one build directory:
#
#   scripts/dev.sh exec --dir <DIR> -- /src/desktop/ui/gallery/tests/run-tests.sh [software]
#
# With "software" the scene is rendered by Qt Quick's software backend.
# Exit status 0 when every test function passed.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
export HOME="$(mktemp -d /tmp/nexus-uitest-home.XXXXXX)"
export XDG_RUNTIME_DIR="$HOME/run"; mkdir -m 700 -p "$XDG_RUNTIME_DIR"
export QT_QUICK_CONTROLS_STYLE=org.nexus.style LIBGL_ALWAYS_SOFTWARE=1
[ "${1:-}" = software ] && export QT_QUICK_BACKEND=software
rc=0
for t in "$HERE"/tst_*.qml; do
    xvfb-run -a -s "-screen 0 1920x1200x24 -nolisten tcp" \
        /usr/lib/qt6/bin/qmltestrunner -input "$t" 2>&1 | grep -av "^PASS\|fd limit\|Invalid image provider: image://nxicon" || true
    [ "${PIPESTATUS[0]}" -eq 0 ] || rc=1
done
rm -rf "$HOME"
exit $rc
