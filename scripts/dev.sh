#!/bin/bash
# Developer loop inside the Debian build chroot (never against host libraries).
#
#   scripts/dev.sh build [--dir NAME] [--only "a;b"] [ninja targets...]
#   scripts/dev.sh run   [--dir NAME] [--shot FILE.png] [--size WxH] [--smoke]
#                        [--delay MS] [--light] [--accent NAME] <binary> [args...]
#   scripts/dev.sh shell [--dir NAME]
#   scripts/dev.sh nested [--dir NAME] [--shot FILE.png] [--size WxH] [--wait SECONDS]
#                         [--light] [--accent NAME] -- '<commands>'
#
#   build   configure (once) and build; --only restricts the configured
#           components, so parallel work on different apps can use separate
#           --dir build directories without touching each other.
#           --modules "a;b" restricts the shell modules built into
#           horizon-shell ("-" for none); --cc-pages "a;b" does the same for
#           Control Center pages
#   run     start a built GUI binary under a virtual X server with software GL;
#           --shot saves a screenshot of its main window into build/work/shots/
#           --smoke exits non-zero if the QML engine reported warnings/errors
#   shell   interactive shell in the chroot with the same environment
#   exec    run one command in the chroot: scripts/dev.sh exec -- apt-cache show nmap
#   nested  a real NEXUS HORIZON session for testing: KWin (Wayland) runs inside
#           a virtual X server and <commands> are executed in it as session
#           clients, e.g.  'horizon-shell & sleep 4; nexus-files /usr &'
#           --shot captures the whole (virtual) screen after --wait seconds.
#           Inside <commands>, "xdo" sends input to the session
#           (xdo mousemove 20 16 click 1; xdo key super), and "shot NAME"
#           takes an additional screenshot. Built binaries of every build
#           directory are on PATH and have launcher entries. Log files
#           written to $HOME/*.log and anything put into $NEXUS_LOGS end up
#           in build/work/shots/nested-logs/<dir>/.
#
# The repository is mounted read-only at /src; build trees live in the chroot
# under /build/<NAME> (default NAME: dev).
. "$(dirname "${BASH_SOURCE[0]}")/../build/lib/common.sh"
need_root

ARCH="${NEXUS_DEV_ARCH:-$(host_arch)}"
CHROOT="$NEXUS_WORK/chroot-build-$ARCH"
[ -e "$CHROOT/.nexus-bootstrap-complete" ] || die "build chroot missing — run build/chroot/create.sh --arch $ARCH"
SHOTS="$NEXUS_WORK/shots"
mkdir -p "$SHOTS" "$CHROOT/build"

cmd="${1:-}"; shift || true
DIR="dev"; ONLY=""; SHOT=""; SIZE=""; SMOKE=0; DELAY=""; LIGHT=0; ACCENT=""; WAIT=8; MODULES=""; CCPAGES=""
while [ $# -gt 0 ]; do
    case "$1" in
        --dir) DIR="$2"; shift 2 ;;
        --only) ONLY="$2"; shift 2 ;;
        --modules) MODULES="$2"; shift 2 ;;
        --cc-pages) CCPAGES="$2"; shift 2 ;;
        --shot) SHOT="$2"; shift 2 ;;
        --size) SIZE="$2"; shift 2 ;;
        --delay) DELAY="$2"; shift 2 ;;
        --wait) WAIT="$2"; shift 2 ;;
        --smoke) SMOKE=1; shift ;;
        --light) LIGHT=1; shift ;;
        --accent) ACCENT="$2"; shift 2 ;;
        --) shift; break ;;
        *) break ;;
    esac
done

B="/build/$DIR"
chr() { in_chroot "$CHROOT" --bind "$NEXUS_ROOT:/src:ro" --bind "$SHOTS:/shots" -- "$@"; }

case "$cmd" in
build)
    if [ ! -f "$CHROOT$B/build.ninja" ] || [ -n "$ONLY$MODULES$CCPAGES" ]; then
        chr cmake -S /src -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
            -DCMAKE_INSTALL_PREFIX=/usr ${ONLY:+-DNEXUS_ONLY="$ONLY"} \
            ${MODULES:+-DHORIZON_MODULES="$MODULES"} ${CCPAGES:+-DNEXUS_CC_PAGES="$CCPAGES"}
    fi
    chr ninja -C "$B" -j "${NEXUS_JOBS}" "$@"
    ;;
run)
    [ $# -ge 1 ] || die "usage: dev.sh run [options] <binary> [args...]"
    bin="$1"; shift
    # A throw-away HOME keeps test runs from picking up stale settings.
    appearance=""
    [ "$LIGHT" -eq 1 ] && appearance="ColorScheme=light"
    [ -n "$ACCENT" ] && appearance="$appearance
Accent=$ACCENT"
    chr /bin/bash -c '
        B="$1"; bin="$2"; shot="$3"; size="$4"; smoke="$5"; delay="$6"; appearance="$7"; shift 7
        export HOME="$(mktemp -d /tmp/nexus-dev-home.XXXXXX)"
        export XDG_RUNTIME_DIR="$HOME/run"; mkdir -m 700 -p "$XDG_RUNTIME_DIR"
        mkdir -p "$HOME/.config/nexus"
        if [ -n "$appearance" ]; then printf "[Appearance]\n%s\n" "$appearance" > "$HOME/.config/nexus/appearance.conf"; fi
        export QML_IMPORT_PATH="$B/qml" LD_LIBRARY_PATH="$B/lib"
        export QT_QPA_PLATFORM=xcb LIBGL_ALWAYS_SOFTWARE=1 QT_QUICK_CONTROLS_STYLE=org.nexus.style
        export QT_LOGGING_RULES="qt.qpa.*=false"
        [ -n "$shot" ] && export NEXUS_SCREENSHOT="/shots/$shot"
        [ -n "$size" ] && export NEXUS_WINDOW_SIZE="$size"
        [ -n "$delay" ] && export NEXUS_SCREENSHOT_DELAY="$delay"
        [ "$smoke" = 1 ] && export NEXUS_SMOKE_TEST=1
        exe="$B/bin/$bin"; [ -x "$exe" ] || exe="$bin"
        exec xvfb-run -a -s "-screen 0 1920x1200x24 -nolisten tcp" \
            dbus-run-session -- timeout --signal=KILL "${NEXUS_RUN_TIMEOUT:-60}" "$exe" "$@"
    ' run "$B" "$bin" "$SHOT" "$SIZE" "$SMOKE" "$DELAY" "$appearance" "$@"
    if [ -n "$SHOT" ] && [ -f "$SHOTS/$SHOT" ]; then log "screenshot: $SHOTS/$SHOT"; fi
    ;;
nested)
    [ $# -ge 1 ] || die "usage: dev.sh nested [options] -- '<commands>'"
    appearance=""
    [ "$LIGHT" -eq 1 ] && appearance="ColorScheme=light"
    [ -n "$ACCENT" ] && appearance="$appearance
Accent=$ACCENT"
    chr /bin/bash -c '
        B="$1"; shot="$2"; size="${3:-1600x900}"; wait="$4"; appearance="$5"; commands="$6"
        W="${size%x*}"; H="${size#*x}"
        export HOME="$(mktemp -d /tmp/nexus-nested-home.XXXXXX)"
        export XDG_RUNTIME_DIR="$HOME/run"; mkdir -m 700 -p "$XDG_RUNTIME_DIR"
        mkdir -p "$HOME/.config/nexus" "$HOME/.config/menus" "$HOME/.local/share/applications"
        cp /src/desktop/horizon-shell/session/nexus-applications.menu "$HOME/.config/menus/"
        cp /src/desktop/horizon-shell/session/kwinrc "$HOME/.config/kwinrc"
        export XDG_MENU_PREFIX=nexus-
        if [ -n "$appearance" ]; then printf "[Appearance]\n%s\n" "$appearance" > "$HOME/.config/nexus/appearance.conf"; fi

        # Launcher entries and PATH for everything that has been built.
        pins=""
        for bindir in /build/*/bin; do
            [ -d "$bindir" ] || continue
            PATH="$PATH:$bindir"
        done
        # This build directory wins over the others.
        PATH="$B/bin:$PATH"
        for desktop in /src/applications/*/org.nexus.*.desktop /src/system/*/org.nexus.*.desktop; do
            [ -f "$desktop" ] || continue
            id="$(basename "$desktop" .desktop)"
            exe="$(sed -n "s/^Exec=\([^ ]*\).*/\1/p" "$desktop" | head -n1)"
            command -v "$exe" >/dev/null 2>&1 || continue
            sed -e "s|^Icon=.*|Icon=/src/assets/icons/apps/$id.svg|" "$desktop" > "$HOME/.local/share/applications/$id.desktop"
        done
        for id in org.nexus.files org.nexus.terminal org.nexus.controlcenter org.nexus.monitor org.nexus.sysinfo; do
            [ -f "$HOME/.local/share/applications/$id.desktop" ] && pins="$pins$id.desktop,"
        done
        printf "[Desktop]\nWallpaper=/src/assets/wallpapers/nexus-horizon.jpg\nWallpaperLight=/src/assets/wallpapers/nexus-daylight.jpg\n[Dock]\nPinned=%s\n" "${pins%,}" \
            > "$HOME/.config/nexus/horizon.conf"

        export LIBGL_ALWAYS_SOFTWARE=1 QT_LOGGING_RULES="*.debug=false;qt.qpa.*=false;kf.*=false"
        export XDG_CURRENT_DESKTOP="NEXUS:KDE" XDG_SESSION_TYPE=wayland
        export KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 KWIN_SCREENSHOT_NO_PERMISSION_CHECKS=1
        export QML_IMPORT_PATH="$B/qml" LD_LIBRARY_PATH="$B/lib"
        display=":$(( 200 + RANDOM % 700 ))"
        Xvfb "$display" -screen 0 "${W}x${H}x24" -nolisten tcp >"$HOME/xvfb.log" 2>&1 &
        xvfb=$!
        sleep 1.5
        export NEXUS_NESTED_X="$display" NEXUS_SHOTS=/shots
        # Where <commands> can leave files that should survive the session.
        export NEXUS_LOGS="/shots/nested-logs/$(basename "$B")"; mkdir -p "$NEXUS_LOGS"
        cat > "$HOME/session.sh" <<SESSION
xdo() { DISPLAY="$display" xdotool "\$@"; }
shot() { DISPLAY="$display" import -window root "/shots/\$1"; }
kwin_wayland --x11-display "$display" --width "$W" --height "$H" --no-lockscreen --socket wayland-nexus >"$HOME/kwin.log" 2>&1 &
kwin=\$!
for i in \$(seq 60); do [ -S "\$XDG_RUNTIME_DIR/wayland-nexus" ] && break; sleep 0.25; done
export WAYLAND_DISPLAY=wayland-nexus QT_QPA_PLATFORM=wayland
kbuildsycoca6 >/dev/null 2>&1
$commands
sleep $wait
[ -n "$shot" ] && shot "$shot"
kill \$(jobs -p) 2>/dev/null
wait 2>/dev/null
SESSION
        timeout --signal=KILL "${NEXUS_RUN_TIMEOUT:-180}" dbus-run-session -- bash "$HOME/session.sh" 2>&1 | grep -v "fd limit"
        kill "$xvfb" 2>/dev/null
        # Logs of the last nested session stay available for debugging.
        logs="/shots/nested-logs/$(basename "$B")"
        mkdir -p "$logs"
        cp "$HOME"/*.log "$logs"/ 2>/dev/null
        rm -rf "$HOME"
        true
    ' nested "$B" "$SHOT" "$SIZE" "$WAIT" "$appearance" "$*"
    if [ -n "$SHOT" ] && [ -f "$SHOTS/$SHOT" ]; then log "screenshot: $SHOTS/$SHOT"; fi
    ;;
shell)
    chr /bin/bash -c "export QML_IMPORT_PATH=$B/qml LD_LIBRARY_PATH=$B/lib; cd $B 2>/dev/null || cd /; exec bash -i"
    ;;
exec)
    # Non-interactive command in the chroot (repository at /src, read-only):
    #   scripts/dev.sh exec -- apt-cache policy nmap
    [ $# -ge 1 ] || die "usage: dev.sh exec [--dir NAME] -- <command> [args...]"
    chr env QML_IMPORT_PATH="$B/qml" LD_LIBRARY_PATH="$B/lib" PATH="$B/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" "$@"
    ;;
*)
    sed -n '2,20p' "$0"; exit 1 ;;
esac
