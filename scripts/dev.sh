#!/bin/bash
# Developer loop inside the Debian build chroot (never against host libraries).
#
#   scripts/dev.sh build [--dir NAME] [--only "a;b"] [ninja targets...]
#   scripts/dev.sh run   [--dir NAME] [--shot FILE.png] [--size WxH] [--smoke]
#                        [--delay MS] [--light] [--accent NAME] <binary> [args...]
#   scripts/dev.sh shell [--dir NAME]
#
#   build   configure (once) and build; --only restricts the configured
#           components, so parallel work on different apps can use separate
#           --dir build directories without touching each other
#   run     start a built GUI binary under a virtual X server with software GL;
#           --shot saves a screenshot of its main window into build/work/shots/
#           --smoke exits non-zero if the QML engine reported warnings/errors
#   shell   interactive shell in the chroot with the same environment
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
DIR="dev"; ONLY=""; SHOT=""; SIZE=""; SMOKE=0; DELAY=""; LIGHT=0; ACCENT=""
while [ $# -gt 0 ]; do
    case "$1" in
        --dir) DIR="$2"; shift 2 ;;
        --only) ONLY="$2"; shift 2 ;;
        --shot) SHOT="$2"; shift 2 ;;
        --size) SIZE="$2"; shift 2 ;;
        --delay) DELAY="$2"; shift 2 ;;
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
    if [ ! -f "$CHROOT$B/build.ninja" ] || [ -n "$ONLY" ]; then
        chr cmake -S /src -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
            -DCMAKE_INSTALL_PREFIX=/usr ${ONLY:+-DNEXUS_ONLY="$ONLY"}
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
    [ -n "$SHOT" ] && [ -f "$SHOTS/$SHOT" ] && log "screenshot: $SHOTS/$SHOT"
    ;;
shell)
    chr /bin/bash -c "export QML_IMPORT_PATH=$B/qml LD_LIBRARY_PATH=$B/lib; cd $B 2>/dev/null || cd /; exec bash -i"
    ;;
*)
    sed -n '2,20p' "$0"; exit 1 ;;
esac
