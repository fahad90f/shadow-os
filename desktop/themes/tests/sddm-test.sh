#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs the NEXUS SDDM theme in the real Qt 6 greeter (test mode) under Xvfb
# and takes screenshots. To be started inside the build chroot:
#
#   scripts/dev.sh exec --dir themes -- sh /src/desktop/themes/tests/sddm-test.sh [options] [-- actions]
#
#   --size WxH        screen size (default 1600x900)
#   --users N         number of login users the greeter sees (default: those of the system)
#   --sessions N      number of installed sessions the greeter sees (default: those of the system)
#   --no-wallpaper    leave the wallpaper path of theme.conf alone (missing in the chroot: solid colour)
#   --fake-daemon     test mode has no SDDM daemon, so a login is never answered and the
#                     power actions are reported as unavailable. With this option the
#                     COPY of the theme under test is patched: every login attempt is
#                     answered with the daemon's loginFailed signal after 1.5 s, and
#                     sddm.can{Suspend,Reboot,PowerOff} read as true (the actions
#                     themselves still go to the absent daemon and do nothing).
#   --prefix NAME     screenshots are /shots/NAME-<label>.png (default themes-sddm)
#   actions           shell commands run while the greeter is up; available:
#                     xdo <xdotool args>, shot <label>, DISPLAY. Default: "shot start".
#
# Users and sessions are made visible to the greeter in a private mount
# namespace (a copy of /etc with extra passwd entries and an SDDM
# configuration naming a temporary session directory), so nothing in the
# chroot is modified. The greeter log is written to /shots/NAME.log.
set -eu

size=1600x900 users="" sessions="" wallpaper=1 prefix=themes-sddm fake=0
while [ $# -gt 0 ]; do
    case "$1" in
        --size) size="$2"; shift 2 ;;
        --users) users="$2"; shift 2 ;;
        --sessions) sessions="$2"; shift 2 ;;
        --no-wallpaper) wallpaper=0; shift ;;
        --fake-daemon) fake=1; shift ;;
        --prefix) prefix="$2"; shift 2 ;;
        --) shift; break ;;
        *) echo "sddm-test: unknown option $1" >&2; exit 2 ;;
    esac
done
actions="${*:-shot start}"

work="$(mktemp -d /tmp/nexus-sddm-test.XXXXXX)"
trap 'rm -rf "$work"' EXIT
cp -r /src/desktop/themes/sddm/nexus "$work/theme"
if [ "$wallpaper" = 1 ]; then
    printf '[General]\nbackground=/src/assets/wallpapers/nexus-horizon.jpg\n' > "$work/theme/theme.conf.user"
fi
if [ "$fake" = 1 ]; then
    sed -i -e 's/sddm\.can\(Suspend\|Reboot\|PowerOff\)/true/g' \
           -e 's|^    TextConstants { id: strings }|&\n    Timer { interval: 1500; running: root.busy; onTriggered: sddm.loginFailed() }|' "$work/theme/Main.qml"
    grep -q 'sddm.loginFailed()' "$work/theme/Main.qml" || { echo "sddm-test: could not patch Main.qml" >&2; exit 1; }
fi

cp -a /etc "$work/etc"
mkdir -p "$work/etc/sddm.conf.d" "$work/sessions" "$work/home"
if [ -n "$users" ]; then
    names="Ada Lovelace:ada|Grace Hopper:grace|:operator|Alan Turing:alan|Katherine Johnson:katherine|Linus:linus|Margaret Hamilton:margaret|Dennis Ritchie:dmr|Barbara Liskov:barbara|Ken Thompson:ken|Radia Perlman:radia|Edsger Dijkstra:edsger"
    i=0
    while [ "$i" -lt "$users" ]; do
        entry="$(printf '%s' "$names" | cut -d'|' -f$(( i % 12 + 1 )))"
        login="${entry#*:}"; [ "$i" -ge 12 ] && login="$login$(( i / 12 + 1 ))"
        mkdir -p "$work/home/$login"
        printf '%s:x:%d:%d:%s:%s:/bin/bash\n' "$login" $(( 1000 + i )) $(( 1000 + i )) "${entry%%:*}" "$work/home/$login" >> "$work/etc/passwd"
        i=$(( i + 1 ))
    done
    # One user with a picture, to exercise the avatar.
    if [ -d "$work/home/grace" ]; then
        convert -size 256x192 gradient:'#FFB020-#FF5C8A' -fill white -draw 'circle 128,80 128,40' "png:$work/home/grace/.face.icon" 2>/dev/null || true
    fi
fi
if [ -n "$sessions" ]; then
    i=0
    for name in "NEXUS HORIZON" "NEXUS HORIZON (safe graphics)" "Recovery console" "Kiosk"; do
        [ "$i" -lt "$sessions" ] || break
        printf '[Desktop Entry]\nType=Application\nName=%s\nComment=Test session %d\nExec=/bin/true\n' "$name" "$i" > "$work/sessions/session$i.desktop"
        i=$(( i + 1 ))
    done
    # SDDM lists Wayland sessions only on machines with /dev/dri, which the
    # chroot does not have; the X11 session directory is read unconditionally.
    printf '[Wayland]\nSessionDir=/nonexistent\n[X11]\nSessionDir=%s\n' "$work/sessions" > "$work/etc/sddm.conf.d/test.conf"
fi

cat > "$work/inner.sh" <<INNER
xdo() { xdotool "\$@"; }
shot() { import -window root "/shots/$prefix-\$1.png"; }
sddm-greeter-qt6 --test-mode --theme "$work/theme" > "/shots/$prefix.log" 2>&1 &
greeter=\$!
sleep \${NEXUS_SDDM_WAIT:-6}
$actions
kill \$greeter 2>/dev/null
wait \$greeter 2>/dev/null
true
INNER

export LIBGL_ALWAYS_SOFTWARE=1 QT_QPA_PLATFORM=xcb
export HOME="$work/home" XDG_RUNTIME_DIR="$work/run"; mkdir -m 700 -p "$XDG_RUNTIME_DIR"
unshare -m --propagation unchanged sh -c "mount --bind '$work/etc' /etc && exec xvfb-run -a -s '-screen 0 ${size}x24' sh '$work/inner.sh'"
echo "sddm-test: log /shots/$prefix.log"
grep -v '^$' "/shots/$prefix.log" | head -n 40 || true
