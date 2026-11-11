# SPDX-License-Identifier: GPL-3.0-or-later
# Helpers for the UI scenarios (sourced inside the nested session, where
# xdo() and shot() exist). Coordinates are relative to the window content:
# KWin places the 1240x800 window at (180,32) on the 1600x900 screen and adds
# a 35 px title bar.
OX=180; OY=67
if ! type xdo >/dev/null 2>&1; then
    # Not the nested session (run-user.sh): a bare X server, the window sits at (0,0).
    OX=0; OY=0
    xdo() { xdotool "$@"; }
    shot() { import -window root -crop 1240x800+0+0 +repage "$SHOT_DIR/$1"; }
fi
APP_LOG="$SHADOW_LOGS/app-$SCN.log"; : > "$APP_LOG"

key() { for k in "$@"; do xdo key "$k"; sleep "${KEY_DELAY:-0.4}"; done; }
type_text() { xdo type --delay 25 "$1"; sleep 0.5; }
move() { xdo mousemove $((OX + $1)) $((OY + $2)); sleep 0.3; }
click() { move "$1" "$2"; xdo click "${3:-1}"; sleep 0.6; }
dclick() { move "$1" "$2"; xdo click --repeat 2 --delay 90 1; sleep 0.8; }
drag() {    # x1 y1 x2 y2 [modifier held during the drop]
    move "$1" "$2"; xdo mousedown 1; sleep 0.3
    xdo mousemove_relative 12 8; sleep 0.2; xdo mousemove_relative 12 8; sleep 0.2
    [ -n "$5" ] && xdo keydown "$5"
    move "$3" "$4"; xdo mousemove_relative 2 2; sleep 0.5
    xdo mouseup 1; sleep 0.3
    [ -n "$5" ] && xdo keyup "$5"
    sleep 1
}
snap() { shot "files-$SCN-$1.png" && echo "shot: files-$SCN-$1.png"; }
tree() { (cd "$1" 2>/dev/null && find . -mindepth 1 | sort | sed "s|^\./||" | tr "\n" " "); }
goto() { key ctrl+l; type_text "$1"; key Return; sleep 1; }
# Open the context menu of the selection with the keyboard and activate its
# n-th enabled entry.
park() { xdo mousemove $((OX + 1235)) $((OY + 795)); sleep 0.2; }   # a resting pointer would hover (and highlight) menu entries
menu_pick() { park; key Menu; sleep 0.4; for i in $(seq "$1"); do xdo key Down; sleep 0.15; done; sleep 0.2; key Return; sleep 0.8; }

app_start() {
    shadow-files "$@" >> "$APP_LOG" 2>&1 &
    APP=$!
    sleep "${START_DELAY:-6}"
    if [ "$OX" = 0 ]; then      # no window manager: give the window the keyboard focus
        xdo windowfocus "$(xdo search --onlyvisible --classname shadow-files | head -1)" 2>/dev/null; sleep 0.4
    fi
}
app_alive() { kill -0 "$APP" 2>/dev/null && echo "app: running" || echo "app: NOT RUNNING"; }
app_stop() {
    app_alive
    kill "$APP" 2>/dev/null; wait "$APP" 2>/dev/null
    local warnings
    warnings="$(grep -a -E "qrc:|QML|ReferenceError|TypeError|Segmentation|ASSERT" "$APP_LOG" | sort | uniq -c)"
    if [ -n "$warnings" ]; then echo "---- APP WARNINGS"; echo "$warnings" | head -40; else echo "---- no QML warnings"; fi
}
