# SPDX-License-Identifier: GPL-3.0-or-later
# Inside the chroot, as root: prepare a throw-away home, let the scenario
# create its root-owned fixture, then run it as nobody under Xvfb.
scn="$1"; dir="$2"
export SCN="$scn"
export HOME="$(mktemp -d /tmp/files-test-home.XXXXXX)"
export XDG_RUNTIME_DIR="$HOME/run"; mkdir -m 700 -p "$XDG_RUNTIME_DIR"
export SHOT_DIR="$HOME/shots" NEXUS_LOGS="$HOME/logs"; mkdir -p "$SHOT_DIR" "$NEXUS_LOGS" "$HOME/.config/nexus"
export QML_IMPORT_PATH="/build/$dir/qml" LD_LIBRARY_PATH="/build/$dir/lib" PATH="/build/$dir/bin:$PATH"
export QT_QPA_PLATFORM=xcb LIBGL_ALWAYS_SOFTWARE=1 QT_QUICK_CONTROLS_STYLE=org.nexus.style
export QT_LOGGING_RULES="qt.qpa.*=false;kf.*=false" NEXUS_WINDOW_SIZE=1240x800
FIXTURE_ONLY=1 . "/src/applications/files/tests/ui/$scn.sh"
chown -R nobody:nogroup "$HOME"
setpriv --reuid nobody --regid nogroup --clear-groups \
    xvfb-run -a -s "-screen 0 1600x900x24 -nolisten tcp" \
    dbus-run-session -- timeout --signal=KILL "${NEXUS_RUN_TIMEOUT:-300}" \
    bash -c '. /src/applications/files/tests/ui/lib.sh; . "/src/applications/files/tests/ui/$SCN.sh"' 2>&1 | grep -v "fd limit"
cp "$SHOT_DIR"/*.png /shots/ 2>/dev/null
mkdir -p "/shots/nested-logs/$dir"; cp "$NEXUS_LOGS"/* "/shots/nested-logs/$dir/" 2>/dev/null
type cleanup >/dev/null 2>&1 && cleanup
rm -rf "$HOME"
