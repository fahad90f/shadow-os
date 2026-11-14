#!/bin/bash
# The launcher must show a newly installed application without a new login.
# Builds test.cpp against the real Applications model in the build chroot and
# runs it with an isolated XDG tree and the session's own application menu.
#
#   sudo desktop/horizon-shell/tests/app-refresh/run.sh
. "$(dirname "${BASH_SOURCE[0]}")/../../../../build/lib/common.sh"
need_root
CHROOT="$SHADOW_WORK/chroot-build-$(host_arch)"
[ -e "$CHROOT/.shadow-bootstrap-complete" ] || die "build chroot missing — run build/chroot/create.sh"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
in_chroot "$CHROOT" --bind "$SHADOW_ROOT:/src:ro" --bind "$WORK:/work" -- /bin/bash -euc '
    cmake -S /src/desktop/horizon-shell/tests/app-refresh -B /work/build -G Ninja >/work/cmake.log 2>&1 || { tail -20 /work/cmake.log; exit 1; }
    cmake --build /work/build >/work/build.log 2>&1 || { grep -E "error" /work/build.log | head -20; exit 1; }
    R=/work/run
    mkdir -p $R/sys/applications $R/home/.local/share $R/cache $R/config $R/runtime $R/xdg/menus
    chmod 700 $R/runtime
    cp /src/desktop/horizon-shell/session/shadow-applications.menu $R/xdg/menus/
    printf "[Desktop Entry]\nType=Application\nName=Existing App\nExec=true\n" > $R/sys/applications/org.test.existing.desktop
    export HOME=$R/home XDG_DATA_HOME=$R/home/.local/share XDG_DATA_DIRS=$R/sys XDG_CACHE_HOME=$R/cache \
           XDG_CONFIG_HOME=$R/config XDG_CONFIG_DIRS=$R/xdg XDG_MENU_PREFIX=shadow- XDG_RUNTIME_DIR=$R/runtime \
           QT_QPA_PLATFORM=offscreen TEST_APPDIR=$R/sys/applications
    /work/build/apptest 2>/dev/null
'
