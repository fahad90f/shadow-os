#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Start a built GUI binary under its own virtual X server and drive it with
# xdotool. Runs inside the build chroot:
#
#   scripts/dev.sh exec --dir <DIR> -- /src/desktop/ui/gallery/tests/xrun.sh \
#       [-s WxH] [-e VAR=value]... <binary> [args...] -- '<commands>'
#
# <commands> is shell code executed while the application runs, with:
#   out            file collecting the application's stdout+stderr
#   wait_for PAT   wait (≤ 20 s) until a line matching PAT appears in $out
#   geometry NAME  print "x y" from the page's "CASE-GEOMETRY NAME x y" line
#   click X Y      move the pointer there and click the left button
#   count PAT      number of lines in $out matching PAT
#   shot NAME      screenshot of the whole virtual screen → /shots/NAME
# The application is killed afterwards; the exit status is that of <commands>.
set -u
SIZE=900x640; ENVS=()
while [ $# -gt 0 ]; do
    case "$1" in
        -s) SIZE="$2"; shift 2 ;;
        -e) ENVS+=("$2"); shift 2 ;;
        *) break ;;
    esac
done
ARGS=()
while [ $# -gt 0 ] && [ "$1" != "--" ]; do ARGS+=("$1"); shift; done
shift; COMMANDS="${1:-true}"

export HOME="$(mktemp -d /tmp/nexus-xrun-home.XXXXXX)"
export XDG_RUNTIME_DIR="$HOME/run"; mkdir -m 700 -p "$XDG_RUNTIME_DIR"
export QT_QPA_PLATFORM=xcb LIBGL_ALWAYS_SOFTWARE=1 QT_QUICK_CONTROLS_STYLE=org.nexus.style
export QT_LOGGING_RULES="qt.qpa.*=false" NEXUS_WINDOW_SIZE="$SIZE"
out="$HOME/app.log"; : > "$out"
display=":$(( 300 + RANDOM % 600 ))"
Xvfb "$display" -screen 0 1920x1200x24 -nolisten tcp >"$HOME/xvfb.log" 2>&1 &
xvfb=$!
sleep 1
export DISPLAY="$display"
env "${ENVS[@]}" dbus-run-session -- timeout --signal=KILL "${NEXUS_RUN_TIMEOUT:-120}" "${ARGS[@]}" >"$out" 2>&1 &
app=$!

wait_for() { for _ in $(seq 100); do grep -qE "$1" "$out" && return 0; sleep 0.2; done; echo "xrun: timed out waiting for $1" >&2; return 1; }
geometry() { grep -E "CASE-GEOMETRY $1 " "$out" | tail -n1 | awk '{print $(NF-1), $NF}'; }
click() { xdotool mousemove "$1" "$2"; sleep 0.15; xdotool click 1; sleep 0.35; }
count() { grep -cE "$1" "$out"; }
shot() { import -window root "/shots/$1"; }

( eval "$COMMANDS" ); rc=$?
kill "$app" 2>/dev/null; sleep 0.3
pkill -P "$app" 2>/dev/null
kill "$xvfb" 2>/dev/null
if [ "${XRUN_SHOW_LOG:-0}" = 1 ] || [ $rc -ne 0 ]; then sed 's/^/    | /' "$out"; fi
rm -rf "$HOME"
exit $rc
