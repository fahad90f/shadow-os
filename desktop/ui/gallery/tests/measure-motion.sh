#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# CPU time the gauge/meter page costs over a fixed run, per rendering mode.
# Runs inside the build chroot:
#
#   scripts/dev.sh exec --dir <DIR> -- /src/desktop/ui/gallery/tests/measure-motion.sh [seconds]
#
# Prints one line per mode: user+system CPU seconds of the gallery process
# for <seconds> (default 20) of wall time showing `--case motion` (12 gauges
# and 12 meters that all change once per second). CPU time, not wall time,
# so the figure is comparable on a loaded machine.
set -u
SECS="${1:-20}"
export HOME="$(mktemp -d /tmp/shadow-motion-home.XXXXXX)"
export XDG_RUNTIME_DIR="$HOME/run"; mkdir -m 700 -p "$XDG_RUNTIME_DIR"; mkdir -p "$HOME/.config/shadow"
export QT_QPA_PLATFORM=xcb LIBGL_ALWAYS_SOFTWARE=1 QT_QUICK_CONTROLS_STYLE=org.shadow.style
export QT_LOGGING_RULES="qt.qpa.*=false" SHADOW_WINDOW_SIZE=1100x720
display=":$(( 300 + RANDOM % 600 ))"
Xvfb "$display" -screen 0 1920x1200x24 -nolisten tcp >/dev/null 2>&1 &
xvfb=$!
sleep 1
export DISPLAY="$display"

measure() {     # label, then VAR=value settings
    local label="$1"; shift
    # SHADOW_SMOKE_TEST makes the gallery exit by itself after the delay.
    local out
    out="$( { TIMEFORMAT='%U %S'; time env "$@" SHADOW_SMOKE_TEST=1 SHADOW_SCREENSHOT_DELAY=$((SECS * 1000)) \
              dbus-run-session -- shadow-ui-gallery --case motion >"$HOME/app.log" 2>&1; } 2>&1 )"
    local cpu; cpu="$(awk '{printf "%.2f", $1 + $2}' <<<"$out")"
    local ok; ok="$(grep -c SHADOW-SMOKE-OK "$HOME/app.log")"
    printf '%-44s %6s s CPU in %s s  (%s)\n' "$label" "$cpu" "$SECS" "$([ "$ok" = 1 ] && echo ok || echo FAILED)"
}

measure "software renderer"                      QT_QUICK_BACKEND=software
printf '[Appearance]\nReduceMotion=true\n' > "$HOME/.config/shadow/appearance.conf"
measure "software renderer, reduce motion"       QT_QUICK_BACKEND=software
rm -f "$HOME/.config/shadow/appearance.conf"
measure "OpenGL on llvmpipe (default RHI)"       SHADOW_DUMMY=1
printf '[Appearance]\nReduceMotion=true\n' > "$HOME/.config/shadow/appearance.conf"
measure "OpenGL on llvmpipe, reduce motion"      SHADOW_DUMMY=1
kill "$xvfb" 2>/dev/null
rm -rf "$HOME"
