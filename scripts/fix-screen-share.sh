#!/usr/bin/env bash
# fix-screen-share.sh - make screen sharing work in a running Shadow Horizon
# session (Discord, Vesktop, Chrome, Firefox, OBS), without a reboot.
#
#     bash scripts/fix-screen-share.sh
#
# Run it as your normal user (not with sudo) from a terminal inside the Shadow
# desktop. It is safe to run again; run it once more after your next login to
# see that the fix stayed. Once the updated shadow-horizon package is
# installed, the package carries the same fix and a run removes the per-user
# copies this script made.
#
# What was wrong, and what the script does about it:
#  1. xdg-desktop-portal-kde creates its ScreenCast, Screenshot, RemoteDesktop,
#     GlobalShortcuts, Background and InputCapture back-ends only when
#     XDG_CURRENT_DESKTOP is exactly "KDE" (src/desktopportal.cpp). The Shadow
#     session is "SHADOW:KDE", so the KDE back-end never offered screen sharing.
#     -> A systemd drop-in gives only that back-end XDG_CURRENT_DESKTOP=KDE.
#  2. KWin's own start-up starts the portal front-end before
#     horizon-session-leader has published the session environment. That
#     front-end finds no shadow-portals.conf, exports no
#     org.freedesktop.portal.ScreenCast ("No such interface") and stays so for
#     the whole session; it also reads the back-end's screen-cast capabilities
#     only once, when it starts.
#     -> The portals are restarted in the right order now, and a small user
#        unit restarts them at every login, after the environment is published.
#  3. The KDE portal's "Screen Sharing" picker is QML that needs modules
#     from plasma-workspace; without them every share is silently cancelled.
#     -> plasma-workspace is installed (no recommends) if it is missing, and
#        packaged portal files that an earlier attempt changed are reinstalled
#        (same question).
#  4. In KDE mode the back-end also offers GlobalShortcuts (it opens a System
#     Settings page that Shadow does not have on every bind, so apps such as
#     Discord would pop up an error), RemoteDesktop and InputCapture (hand an
#     app the keyboard and pointer after one click). Screen sharing needs none
#     of them. -> They stay off, as in the updated shadow-portals.conf, through
#        ~/.config/xdg-desktop-portal/shadow-portals.conf until the updated
#        shadow-horizon package is installed.
#  5. KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 (added during earlier attempts) is
#     not needed: KWin grants the portal screen capture through the portal's
#     own desktop entry. The switch lets every program, sandboxed ones
#     included, type keystrokes, record the screen without asking and put a
#     window above the lock screen.
#     -> The script offers to remove it. The running session keeps working
#        (KWin reads it only when it starts); after your next login KWin's
#        per-program check is on again. The script never turns it back on.
#  6. plasma-workspace brings the KDE activity manager, which KWin starts at
#     login and which by default keeps a history of the files and programs
#     you use. -> Set to keep none, unless you chose a setting yourself.
#
# apt (step 1) and removing the switch from files outside your home (end)
# ask for your password, each only after you said yes. Everything else is
# written under ~/.config. Files from earlier attempts that would get in the
# way are moved or copied to
# ~/.local/state/shadow-fix-screen-share/backup-<date>/, never deleted.

if [ -z "${BASH_VERSION:-}" ]; then exec bash "$0" "$@"; fi
set -u
export SYSTEMD_PAGER=cat

BIN_KDE=/usr/lib/x86_64-linux-gnu/libexec/xdg-desktop-portal-kde
BIN_FE=/usr/libexec/xdg-desktop-portal
BIN_GTK=/usr/libexec/xdg-desktop-portal-gtk
UNIT_KDE=plasma-xdg-desktop-portal-kde.service
UNIT_FE=xdg-desktop-portal.service
UNIT_GTK=xdg-desktop-portal-gtk.service
NAME_KDE=org.freedesktop.impl.portal.desktop.kde
NAME_FE=org.freedesktop.portal.Desktop
NAME_GTK=org.freedesktop.impl.portal.desktop.gtk
OBJ=/org/freedesktop/portal/desktop
KDE_DBUS=/usr/share/dbus-1/services/$NAME_KDE.service
FE_DBUS=/usr/share/dbus-1/services/$NAME_FE.service
SESSION_TARGET=shadow-horizon-session.target
REFRESH_UNIT=shadow-fix-screen-share.service
PKG_DROPIN=/usr/lib/systemd/user/$UNIT_KDE.d/50-shadow.conf
PORTAL_DESKTOP=/usr/share/applications/org.freedesktop.impl.portal.desktop.kde.desktop
QML=/usr/lib/x86_64-linux-gnu/qt6/qml
SYSTEMDIALOG=/usr/share/plasma/look-and-feel/org.kde.breeze.desktop/contents/systemdialog/SystemDialog.qml
CONF_HOME=${XDG_CONFIG_HOME:-$HOME/.config}
DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
STATE_DIR=${XDG_STATE_HOME:-$HOME/.local/state}/shadow-fix-screen-share
UNIT_DIR=$CONF_HOME/systemd/user
DROPIN=$UNIT_DIR/$UNIT_KDE.d/99-shadow-screen-share.conf
# Portal routing: a shadow-portals.conf in ~/.config replaces the packaged one.
SYS_PORTALS=/usr/share/xdg-desktop-portal/shadow-portals.conf
USER_PORTALS=$CONF_HOME/xdg-desktop-portal/shadow-portals.conf
# First line of every file this script writes.
MARK='# Written by Shadow OS scripts/fix-screen-share.sh.'
# Packaged files the fix relies on: how D-Bus starts the portals, the binary and
# desktop entry KWin's permission check matches, the units and kde.portal.
CRITICAL="$BIN_KDE $BIN_FE $PORTAL_DESKTOP $KDE_DBUS $FE_DBUS /usr/lib/systemd/user/$UNIT_KDE"
CRITICAL="$CRITICAL /usr/lib/systemd/user/$UNIT_FE /usr/share/xdg-desktop-portal/portals/kde.portal"
# What horizon-session-leader publishes at login (minus the two forced below).
SESSION_VARS="WAYLAND_DISPLAY DISPLAY XAUTHORITY XDG_SESSION_DESKTOP XDG_SESSION_TYPE KDE_FULL_SESSION
KDE_SESSION_VERSION QT_QPA_PLATFORM QT_QPA_PLATFORMTHEME GDK_BACKEND SDL_VIDEODRIVER CLUTTER_BACKEND
MOZ_ENABLE_WAYLAND ELECTRON_OZONE_PLATFORM_HINT _JAVA_AWT_WM_NONREPARENTING XCURSOR_THEME XCURSOR_SIZE"
# A line that switches KWin's per-program permission checks off: shell and
# environment.d syntax (NAME=value, export NAME=value) and pam_env syntax
# (NAME DEFAULT=value / NAME OVERRIDE=value).
BYPASS_RE='^[[:space:]]*(export[[:space:]]+)?KWIN_(WAYLAND|SCREENSHOT)_NO_PERMISSION_CHECKS([[:space:]]*=|[[:space:]]+(DEFAULT|OVERRIDE)=)'
KAMD_GROUP=Plugin-org.kde.ActivityManager.Resources.Scoring
START_TS=$(date +%s)
FAILS=0
WARNS=0
BACKUP=""
PICKER_OK=1
REFRESH_OK=0
BYPASS_LEFT=0
EXE_OK=0
probe=2
pkgs=""
repair=""
changed=""
TEE_PID=""

step() { printf '\n== %s\n' "$*"; }
ok()   { printf '  [ OK ] %s\n' "$*"; }
warn() { printf '  [WARN] %s\n' "$*"; WARNS=$((WARNS + 1)); }
fail() { printf '  [FAIL] %s\n' "$*"; FAILS=$((FAILS + 1)); }
note() { printf '         %s\n' "$*"; }
die()  { printf '\nERROR: %s\n' "$*"; exit 1; }

wait_until() {   # seconds command... : poll every 0.25 s
    local n=$(($1 * 4))
    shift
    while [ "$n" -gt 0 ]; do
        "$@" && return 0
        sleep 0.25
        n=$((n - 1))
    done
    return 1
}

# D-Bus helpers; only introspect_has can start a service.
has_owner() {   # 0 = owned, 1 = free, 2 = could not ask
    local out
    out=$(gdbus call --session --timeout 5 --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
        --method org.freedesktop.DBus.NameHasOwner "$1" 2>/dev/null) || return 2
    case "$out" in
        "(true,)") return 0 ;;
        "(false,)") return 1 ;;
        *) return 2 ;;
    esac
}
owner_pid() {
    gdbus call --session --timeout 5 --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
        --method org.freedesktop.DBus.GetConnectionUnixProcessID "$1" 2>/dev/null \
        | sed -n 's/^(uint32 \([0-9][0-9]*\),)$/\1/p'
}
introspect_has() {   # bus-name interface
    timeout --foreground 30 gdbus introspect --session --dest "$1" --object-path "$OBJ" 2>/dev/null \
        | grep -qF "interface $2 {"
}
fe_prop() {   # property of org.freedesktop.portal.ScreenCast on the front-end -> number
    gdbus call --session --timeout 10 --dest "$NAME_FE" --object-path "$OBJ" \
        --method org.freedesktop.DBus.Properties.Get org.freedesktop.portal.ScreenCast "$1" 2>/dev/null \
        | sed -n 's/^(<uint32 \([0-9][0-9]*\)>,)$/\1/p'
}
unit_pid() { systemctl --user show -p MainPID --value "$1" 2>/dev/null; }
proc_env() {   # pid variable -> its value in that process
    { tr '\0' '\n' < "/proc/$1/environ"; } 2>/dev/null | sed -n "s/^$2=//p" | head -n 1
}
# This user's processes whose executable is exactly one of the given files.
# /proc/PID/exe, not the process name (cut to 15 characters by the kernel)
# nor the command line, so this can never match the script itself.
own_pids_of() {
    local p exe want out=""
    for p in /proc/[0-9]*; do
        [ -O "$p" ] || continue
        exe=$(readlink "$p/exe" 2>/dev/null) || continue
        exe=${exe% (deleted)}
        for want in "$@"; do
            if [ "$exe" = "$want" ]; then out="$out ${p#/proc/}"; fi
        done
    done
    printf '%s' "${out# }"
}
is_ours() {   # file: written by this script (and not a link)
    [ -f "$1" ] && [ ! -L "$1" ] && [ "$(head -n 1 "$1" 2>/dev/null)" = "$MARK" ]
}
pids_gone() {
    local p
    for p in "$@"; do kill -0 "$p" 2>/dev/null && return 1; done
    return 0
}
# The systemd unit a process runs in: the last part of its cgroup path. A copy
# that systemd started for a portal unit runs in that unit's cgroup; a copy
# started from a terminal, or forked by D-Bus without systemd, does not.
unit_of_pid() {
    local cg
    cg=$(sed -n -e 's/^0:://p' -e 's/^[0-9]*:name=systemd://p' "/proc/$1/cgroup" 2>/dev/null | head -n 1)
    printf '%s' "${cg##*/}"
}
names_released() {   # 0 once no stopped stray still owns a portal bus name
    local n o
    for n in "$NAME_FE" "$NAME_KDE" "$NAME_GTK"; do
        o=$(owner_pid "$n")
        case " $strays " in *" ${o:-none} "*) return 1 ;; esac
    done
    return 0
}

# Ask the KDE back-end for a screen-cast session (shows nothing) and close it
# again. It answers 0 only when KWin has given it the screen-capture protocol
# zkde_screencast_unstable_v1, and 2 when not.
PROBE_OUT=""
probe_screencast() {
    local req="$OBJ/request/shadow_fix/r$$_$1" ses="$OBJ/session/shadow_fix/s$$_$1"
    PROBE_OUT=$(gdbus call --session --timeout 10 --dest "$NAME_KDE" --object-path "$OBJ" \
        --method org.freedesktop.impl.portal.ScreenCast.CreateSession \
        "$req" "$ses" "'shadow-fix-screen-share'" "@a{sv} {}" 2>&1)
    gdbus call --session --timeout 5 --dest "$NAME_KDE" --object-path "$ses" \
        --method org.freedesktop.impl.portal.Session.Close >/dev/null 2>&1
    case "$PROBE_OUT" in
        "(uint32 0,"*) return 0 ;;
        "(uint32 2,"*) return 1 ;;
        *) return 2 ;;
    esac
}

backup_dir() {
    if [ -z "$BACKUP" ]; then
        BACKUP=$STATE_DIR/backup-$(date +%Y%m%d-%H%M%S)
        mkdir -p "$BACKUP" || { fail "cannot create $BACKUP"; BACKUP=""; return 1; }
    fi
}
backup_move() {   # move a leftover of an earlier attempt out of the way
    local f=$1 rel
    backup_dir || return 1
    rel=${f#/}
    if mkdir -p "$BACKUP/${rel%/*}" && mv -f "$f" "$BACKUP/$rel"; then
        warn "moved aside (left from an earlier attempt): $f"
    else
        fail "could not move $f out of the way; move or delete it yourself"
    fi
}
backup_copy() {   # keep a copy of a file before a line is removed from it
    local f=$1 rel
    backup_dir || return 1
    rel=${f#/}
    mkdir -p "$BACKUP/${rel%/*}" && cp -pL "$f" "$BACKUP/$rel"
}

# Files that switch KWin's permission checks off for the Shadow session: read
# by pam_env at login (/etc/environment also reaches every systemd --user
# service through /usr/lib/environment.d/99-environment.conf), by the login
# shell that starts the session, or the session script itself.
bypass_files() {
    local f
    for f in /etc/environment /etc/environment.d/*.conf /etc/security/pam_env.conf \
        /etc/profile /etc/profile.d/*.sh /etc/bash.bashrc /usr/bin/shadow-horizon-session \
        "$CONF_HOME"/environment.d/*.conf "$HOME/.pam_environment" "$HOME/.profile" \
        "$HOME/.bash_profile" "$HOME/.bash_login" "$HOME/.bashrc" \
        "$HOME/.zprofile" "$HOME/.zshenv" "$HOME/.zshrc"; do
        if [ -f "$f" ] && grep -qsE "$BYPASS_RE" "$f"; then printf '%s\n' "$f"; fi
    done
}
SUDO_OK=0
remove_bypass() {   # file... : delete only the lines that set the switches
    local f rc=0
    for f in "$@"; do
        if ! { [ -O "$f" ] && [ -w "$f" ]; } && [ "$SUDO_OK" != 1 ]; then
            rc=1
            continue
        fi
        if ! backup_copy "$f"; then
            fail "could not keep a copy of $f, so it was not changed"
            rc=1
            continue
        fi
        if [ -O "$f" ] && [ -w "$f" ]; then
            sed -i --follow-symlinks -E "/$BYPASS_RE/d" "$f"
        else
            sudo sed -i --follow-symlinks -E "/$BYPASS_RE/d" "$f"
        fi
        if grep -qsE "$BYPASS_RE" "$f"; then
            fail "still set in $f"
            rc=1
        else
            ok "removed from $f (copy in $BACKUP)"
        fi
    done
    return "$rc"
}

need() { case " $pkgs " in *" $1 "*) ;; *) pkgs="${pkgs:+$pkgs }$1" ;; esac; }
check_qml() {
    pkgs=""
    [ -f "$QML/org/kde/kirigami/qmldir" ] || need qml6-module-org-kde-kirigami
    [ -f "$QML/org/kde/pipewire/qmldir" ] || need qml6-module-org-kde-pipewire
    for m in org/kde/plasma/workspace/dialogs org/kde/taskmanager org/kde/plasma/lookandfeel; do
        [ -f "$QML/$m/qmldir" ] || need plasma-workspace
    done
    [ -f "$SYSTEMDIALOG" ] || need plasma-workspace-data
}
add_repair() {   # package path
    case " $repair " in *" $1 "*) ;; *) repair="${repair:+$repair }$1" ;; esac
    case " $changed " in *" $2 "*) ;; *) changed="$changed $2" ;; esac
}
# Packaged portal files that an earlier attempt edited, replaced or removed. A
# D-Bus file without SystemdService= starts the back-end outside systemd, so
# the drop-in below would stop working at the next login.
check_pkg_files() {
    local pkg path
    repair=""
    changed=""
    grep -qsxF "SystemdService=$UNIT_KDE" "$KDE_DBUS" || add_repair xdg-desktop-portal-kde "$KDE_DBUS"
    grep -qsxF "SystemdService=$UNIT_FE" "$FE_DBUS" || add_repair xdg-desktop-portal "$FE_DBUS"
    command -v dpkg >/dev/null 2>&1 || return 0
    for pkg in xdg-desktop-portal-kde xdg-desktop-portal; do
        for path in $(LC_ALL=C dpkg --verify "$pkg" 2>/dev/null | sed 's/.*[[:space:]]//'); do
            case " $CRITICAL " in *" $path "*) add_repair "$pkg" "$path" ;; esac
        done
    done
}
# apt without questions: no debconf dialogs, changed configuration files are
# kept, and nothing reads the terminal (so it cannot swallow what you type).
apt_get() {
    sudo env DEBIAN_FRONTEND=noninteractive apt-get -o DPkg::Lock::Timeout=120 \
        -o Dpkg::Options::=--force-confdef -o Dpkg::Options::=--force-confold "$@" < /dev/null
}
install_packages() {
    note "(sudo may also print 'unable to resolve host ...'; that is harmless)"
    sudo -v || return 1
    if [ -n "$pkgs" ]; then
        # --no-remove: apt stops instead of removing anything.
        # shellcheck disable=SC2086
        if ! apt_get install -y --no-install-recommends --no-remove $pkgs; then
            note "Refreshing the package lists and trying once more..."
            apt_get update || true
            # shellcheck disable=SC2086
            apt_get install -y --no-install-recommends --no-remove $pkgs || true
        fi
    fi
    if [ -n "$repair" ]; then
        # shellcheck disable=SC2086
        apt_get install -y --reinstall --no-install-recommends --no-remove $repair || true
    fi
}
# Hand the rest of the log to the terminal before the script ends, also after
# die; never wait for it for more than two seconds.
close_log() {
    [ -n "$TEE_PID" ] || return 0
    exec >&- 2>&-
    local n=40
    while [ "$n" -gt 0 ] && kill -0 "$TEE_PID" 2>/dev/null; do
        sleep 0.05
        n=$((n - 1))
    done
}

# ---------------------------------------------------------------------------
[ "$(id -u)" -ne 0 ] || die "Run this as your normal user, not with sudo or as root: it fixes your own desktop session."

LOG=""
if mkdir -p "$STATE_DIR" 2>/dev/null && : 2>/dev/null > "$STATE_DIR/last-run.log"; then
    LOG=$STATE_DIR/last-run.log
    exec > >(tee -a "$LOG") 2>&1
    TEE_PID=$!
    trap close_log EXIT
fi
printf 'Shadow OS: fix screen sharing (%s)\n' "$(date)"

for c in systemctl gdbus grep sed tr readlink timeout journalctl dbus-update-activation-environment; do
    command -v "$c" >/dev/null 2>&1 || die "The command '$c' is missing."
done
SYSTEMCTL=$(command -v systemctl)
if [ -z "${XDG_RUNTIME_DIR:-}" ] || [ ! -d "$XDG_RUNTIME_DIR" ]; then
    die "XDG_RUNTIME_DIR is not set. Run this from a terminal inside your Shadow desktop."
fi
case "${WAYLAND_DISPLAY:-}" in
    "") die "WAYLAND_DISPLAY is not set. Run this from a terminal inside your Shadow desktop (not over SSH or on a text console)." ;;
    /*) wl_sock=$WAYLAND_DISPLAY ;;
    *) wl_sock=$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY ;;
esac
[ -S "$wl_sock" ] || die "There is no Wayland display at $wl_sock. Run this from a terminal inside your Shadow desktop."
systemctl --user show-environment >/dev/null 2>&1 || die "Cannot reach your systemd user manager (systemctl --user)."
user_bus="unix:path=$XDG_RUNTIME_DIR/bus"
[ -S "$XDG_RUNTIME_DIR/bus" ] || die "There is no D-Bus session bus at $XDG_RUNTIME_DIR/bus (package dbus-user-session)."
case "${DBUS_SESSION_BUS_ADDRESS:-}" in
    "") export DBUS_SESSION_BUS_ADDRESS="$user_bus" ;;
    "$user_bus" | "$user_bus,"*) ;;
    *) die "This terminal uses another D-Bus bus ($DBUS_SESSION_BUS_ADDRESS) than your desktop ($user_bus). Open a new terminal from the dock and run the script there." ;;
esac
[ -x "$BIN_FE" ] || die "$BIN_FE is missing. Run: sudo apt install xdg-desktop-portal"
[ -x "$BIN_KDE" ] || die "$BIN_KDE is missing. Run: sudo apt install xdg-desktop-portal-kde"
[ "$(systemctl --user show -p LoadState --value "$UNIT_KDE" 2>/dev/null)" != not-found ] \
    || die "The user unit $UNIT_KDE is missing. Run: sudo apt install --reinstall xdg-desktop-portal-kde"
printf 'Open file dialogs will close, and running screen shares stop for a moment.\n'

# ---------------------------------------------------------------------------
step "1/7  Packages the screen picker needs, and the packaged portal files"
check_qml
check_pkg_files
had_pkgs=$pkgs
if [ -z "$pkgs" ] && [ -z "$repair" ]; then
    ok "present: plasma-workspace dialogs, task manager, PipeWire and Kirigami QML modules"
    ok "the packaged portal files are as shipped"
else
    if [ -n "$pkgs" ]; then
        note "Missing: $pkgs"
        note "The KDE portal draws its 'Screen Sharing' picker with QML from these"
        note "packages; without them every share is silently cancelled. This does not"
        note "switch you to Plasma: the Shadow desktop stays as it is."
    fi
    if [ -n "$repair" ]; then
        note "Changed or removed, probably by an earlier attempt:$changed"
        note "Reinstalling $repair puts the original files back."
    fi
    if [ -t 0 ]; then
        printf '         Do this now (apt, without recommended extras)? [Y/n] '
        read -r answer || answer=n
        case "$answer" in
            [Nn]*) ;;
            *) install_packages ;;
        esac
        check_qml
        check_pkg_files
    fi
    if [ -z "$pkgs" ]; then
        if [ -n "$had_pkgs" ]; then
            ok "installed; the screen picker can open now"
        else
            ok "present: plasma-workspace dialogs, task manager, PipeWire and Kirigami QML modules"
        fi
    else
        PICKER_OK=0
        fail "still missing: $pkgs (the screen picker cannot open)"
    fi
    if [ -z "$repair" ]; then
        ok "the packaged portal files are as shipped"
    else
        fail "packaged portal files differ from the packages:$changed"
    fi
fi

# ---------------------------------------------------------------------------
step "2/7  Leftovers of earlier attempts"
before=$((WARNS + FAILS))
name_re='(org\.freedesktop\.impl\.portal\.desktop\.(kde|gtk)|org\.freedesktop\.portal\.Desktop)'
# D-Bus activation files. One without SystemdService= starts a portal outside
# systemd, so without the drop-in below.
reload=0
for d in "$XDG_RUNTIME_DIR/dbus-1/services" "$DATA_HOME/dbus-1/services"; do
    [ -d "$d" ] || continue
    for f in "$d"/*.service; do
        [ -f "$f" ] || continue
        if grep -qE "^[[:space:]]*Name[[:space:]]*=[[:space:]]*${name_re}[[:space:]]*$" "$f"; then
            backup_move "$f"
            reload=1
        fi
    done
done
if [ "$reload" = 1 ]; then
    gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus \
        --method org.freedesktop.DBus.ReloadConfig >/dev/null 2>&1
fi
for f in /usr/local/share/dbus-1/services/*.service; do
    [ -f "$f" ] || continue
    grep -qE "^[[:space:]]*Name[[:space:]]*=[[:space:]]*${name_re}[[:space:]]*$" "$f" || continue
    if grep -q '^SystemdService=' "$f"; then
        warn "$f replaces a portal's D-Bus file; if sharing still fails: sudo rm '$f'"
    else
        fail "$f starts a portal outside systemd; remove it with: sudo rm '$f'"
    fi
done
# A second file for the same bus name next to the packaged one: D-Bus then
# picks one of the two at random.
# shellcheck disable=SC2013
for f in $(grep -lsE "^[[:space:]]*Name[[:space:]]*=[[:space:]]*${name_re}[[:space:]]*$" /usr/share/dbus-1/services/*.service); do
    case "$f" in "$KDE_DBUS" | "$FE_DBUS" | */"$NAME_GTK.service") continue ;; esac
    fail "$f claims a portal's D-Bus name too; remove it with: sudo rm '$f'"
done
# systemd: masks, whole-unit replacements and other drop-ins of the portal units.
for u in "$UNIT_KDE" "$UNIT_FE" "$UNIT_GTK"; do
    state=$(systemctl --user show -p LoadState --value "$u" 2>/dev/null)
    [ "$state" = not-found ] && continue
    if [ "$state" = masked ]; then
        systemctl --user unmask "$u" >/dev/null 2>&1
        if [ "$(systemctl --user show -p LoadState --value "$u" 2>/dev/null)" = masked ]; then
            fail "$u is masked system-wide; undo with: sudo systemctl --global unmask $u"
        else
            warn "unmasked $u (it had been masked)"
        fi
    fi
    frag=$(systemctl --user show -p FragmentPath --value "$u" 2>/dev/null)
    case "$frag" in
        "$HOME"/* | "$XDG_RUNTIME_DIR"/*) backup_move "$frag" ;;
        /etc/*) warn "$u is replaced by $frag; if sharing still fails: sudo rm '$frag'" ;;
    esac
    # shellcheck disable=SC2013
    for f in $(systemctl --user show -p DropInPaths --value "$u" 2>/dev/null); do
        case "$f" in */"$UNIT_KDE.d/${DROPIN##*/}") continue ;; esac
        case "$f" in
            "$HOME"/*/"$u.d"/* | "$XDG_RUNTIME_DIR"/*/"$u.d"/*) backup_move "$f" ;;
            "$HOME"/* | "$XDG_RUNTIME_DIR"/*) warn "$f also changes $u; check it if sharing still fails" ;;
            /etc/*) warn "$f also changes $u; if sharing still fails: sudo rm '$f'" ;;
        esac
    done
done
# Portal settings: a file here wins over /usr/share/xdg-desktop-portal/shadow-portals.conf.
for d in "$CONF_HOME/xdg-desktop-portal" "$DATA_HOME/xdg-desktop-portal"; do
    for f in "$d/shadow-portals.conf" "$d/kde-portals.conf" "$d/portals.conf"; do
        # This script's own copy (step 3) is not a leftover.
        if [ "$f" = "$USER_PORTALS" ] && is_ours "$f"; then continue; fi
        if [ -e "$f" ] || [ -L "$f" ]; then backup_move "$f"; fi
    done
done
for d in /etc/xdg/xdg-desktop-portal /etc/xdg-desktop-portal /usr/local/share/xdg-desktop-portal; do
    for f in "$d/shadow-portals.conf" "$d/kde-portals.conf" "$d/portals.conf"; do
        if [ -e "$f" ]; then warn "$f overrides Shadow's portal settings; if sharing still fails: sudo rm '$f'"; fi
    done
done
if systemctl --user show-environment 2>/dev/null | grep -q '^XDG_DESKTOP_PORTAL_DIR='; then
    systemctl --user unset-environment XDG_DESKTOP_PORTAL_DIR
    warn "removed XDG_DESKTOP_PORTAL_DIR from the service environment"
fi
# Desktop entries: one that hides the portal's own entry makes KWin refuse it
# the screen-capture protocol; an autostart entry that starts a portal by hand
# runs it outside systemd with the wrong environment.
for d in "$DATA_HOME/applications" "$CONF_HOME/autostart" /usr/local/share/applications; do
    [ -d "$d" ] || continue
    for f in "$d"/*.desktop; do
        [ -f "$f" ] || continue
        if [ "${f##*/}" = "${PORTAL_DESKTOP##*/}" ] || grep -qE '^Exec=.*xdg-desktop-portal' "$f"; then
            case "$f" in
                "$HOME"/*) backup_move "$f" ;;
                *) warn "$f stands in for the portal's own desktop entry; remove it with: sudo rm '$f'" ;;
            esac
        fi
    done
done
[ $((WARNS + FAILS)) -eq "$before" ] && ok "nothing in the way"
[ -n "$BACKUP" ] && note "moved files are in $BACKUP"

# ---------------------------------------------------------------------------
step "3/7  XDG_CURRENT_DESKTOP=KDE for the KDE back-end only, also after login"
# An earlier "sudo" command may have left these owned by root.
for f in "$CONF_HOME" "$UNIT_DIR" "${DROPIN%/*}" "$DROPIN" "$UNIT_DIR/$REFRESH_UNIT" \
    "${USER_PORTALS%/*}" "$USER_PORTALS"; do
    if [ -e "$f" ] && [ ! -O "$f" ]; then
        die "$f does not belong to you (left by a command run with sudo?). Fix it with: sudo chown -R $(id -un): $f"
    fi
done
# The updated shadow-horizon package carries the same drop-in, and its session
# leader restarts the portals at login: then the per-user copies are not needed.
PKG_FIX=0
[ -f "$PKG_DROPIN" ] && PKG_FIX=1
if [ "$PKG_FIX" = 0 ]; then
    mkdir -p "${DROPIN%/*}" "$UNIT_DIR" \
        || die "cannot create ${DROPIN%/*} (if ~/.config belongs to root: sudo chown -R $(id -un): $CONF_HOME)"
fi
if [ "$PICKER_OK" = 1 ] && [ "$PKG_FIX" = 1 ]; then
    if is_ours "$DROPIN"; then
        rm -f "$DROPIN" && ok "the installed shadow-horizon package carries the drop-in; removed the copy in ~/.config"
    fi
elif [ "$PICKER_OK" = 1 ]; then
    printf '%s\n' \
        "$MARK" \
        '# xdg-desktop-portal-kde creates its ScreenCast, Screenshot, RemoteDesktop,' \
        '# GlobalShortcuts, Background and InputCapture back-ends only when' \
        '# XDG_CURRENT_DESKTOP is exactly "KDE"; the Shadow session is "SHADOW:KDE".' \
        '# Only this process is told "KDE"; the portal front-end keeps SHADOW:KDE.' \
        '[Service]' \
        'Environment=XDG_CURRENT_DESKTOP=KDE' > "$DROPIN" || die "cannot write $DROPIN"
else
    # Without the dialog modules the KDE mode gains nothing (every share is
    # cancelled) and a single screenshot request crashes the whole back-end,
    # file dialogs included. Stay out of it until the packages are there.
    warn "the KDE back-end stays out of KDE mode until the packages above are installed"
    if [ -e "$DROPIN" ]; then backup_move "$DROPIN"; fi
fi
# In KDE mode the back-end would also offer GlobalShortcuts (it opens a System
# Settings page on every bind, which Shadow does not have: an error pop-up in
# Discord and other apps), RemoteDesktop and InputCapture (keyboard and pointer
# for an app after one click). Screen sharing needs none of them; the updated
# shadow-horizon package switches them off in its shadow-portals.conf. Until
# that is installed, a copy in ~/.config (which replaces it) does the same.
if grep -qsx 'org.freedesktop.impl.portal.GlobalShortcuts=none' "$SYS_PORTALS"; then
    if is_ours "$USER_PORTALS"; then
        rm -f "$USER_PORTALS" && ok "the installed $SYS_PORTALS is current; removed the copy in ~/.config"
    fi
else
    mkdir -p "${USER_PORTALS%/*}" || die "cannot create ${USER_PORTALS%/*}"
    printf '%s\n' \
        "$MARK" \
        '# Same as the shadow-portals.conf of the updated shadow-horizon package;' \
        '# this script deletes it again once that package is installed.' \
        '[preferred]' \
        'default=kde;gtk' \
        'org.freedesktop.impl.portal.ScreenCast=kde' \
        'org.freedesktop.impl.portal.Screenshot=kde' \
        'org.freedesktop.impl.portal.GlobalShortcuts=none' \
        'org.freedesktop.impl.portal.RemoteDesktop=none' \
        'org.freedesktop.impl.portal.InputCapture=none' > "$USER_PORTALS" || die "cannot write $USER_PORTALS"
    ok "portals screen sharing does not need stay off (GlobalShortcuts, RemoteDesktop, InputCapture)"
fi
NEED_ENABLE=0
if [ "$PKG_FIX" = 1 ]; then
    if is_ours "$UNIT_DIR/$REFRESH_UNIT"; then
        systemctl --user disable "$REFRESH_UNIT" >/dev/null 2>&1
        rm -f "$UNIT_DIR/$REFRESH_UNIT"
    fi
    REFRESH_OK=1
    ok "the installed shadow-horizon package restarts the portals at every login"
elif [ "$(systemctl --user show -p LoadState --value "$SESSION_TARGET" 2>/dev/null)" = loaded ]; then
    printf '%s\n' \
        "$MARK" \
        '# KWin starts the portal front-end before horizon-session-leader has' \
        '# published XDG_CURRENT_DESKTOP and WAYLAND_DISPLAY to systemd; that' \
        '# front-end offers no ScreenCast for the whole session. The session target' \
        '# is started after the publish, so restart the portals then. This does' \
        '# nothing once the shadow-horizon package ships the same fix.' \
        '[Unit]' \
        'Description=Restart the desktop portals with the Shadow session environment' \
        "ConditionPathExists=!$PKG_DROPIN" \
        'Before=xdg-desktop-autostart.target' \
        '' \
        '[Service]' \
        'Type=oneshot' \
        'TimeoutStartSec=60' \
        "ExecStart=-$SYSTEMCTL --user stop $UNIT_FE $UNIT_KDE $UNIT_GTK" \
        "ExecStart=-$SYSTEMCTL --user reset-failed $UNIT_FE $UNIT_KDE $UNIT_GTK" \
        "ExecStart=-$SYSTEMCTL --user --no-block start $UNIT_KDE $UNIT_GTK $UNIT_FE" \
        '' \
        '[Install]' \
        "WantedBy=$SESSION_TARGET" > "$UNIT_DIR/$REFRESH_UNIT" || die "cannot write $UNIT_DIR/$REFRESH_UNIT"
    REFRESH_OK=1
    NEED_ENABLE=1
else
    warn "$SESSION_TARGET does not exist, so the restart at login cannot be set up;"
    note "after each login run: systemctl --user restart $UNIT_FE"
fi
systemctl --user daemon-reload || fail "systemctl --user daemon-reload failed"
if [ "$NEED_ENABLE" = 1 ]; then
    systemctl --user enable "$REFRESH_UNIT" >/dev/null 2>&1
    if [ "$(systemctl --user is-enabled "$REFRESH_UNIT" 2>/dev/null)" = enabled ]; then
        ok "$REFRESH_UNIT restarts the portals at every login"
    else
        REFRESH_OK=0
        fail "could not enable $REFRESH_UNIT"
    fi
fi
if [ "$PICKER_OK" = 1 ]; then
    case " $(systemctl --user show -p DropInPaths --value "$UNIT_KDE" 2>/dev/null) " in
        *"/$UNIT_KDE.d/${DROPIN##*/} "* | *" $PKG_DROPIN "*) ;;
        *) fail "systemd reads neither $DROPIN nor $PKG_DROPIN" ;;
    esac
    kenv=$(systemctl --user show -p Environment --value "$UNIT_KDE" 2>/dev/null)
    case " $kenv " in
        *" XDG_CURRENT_DESKTOP=KDE "*) ok "$UNIT_KDE gets XDG_CURRENT_DESKTOP=KDE" ;;
        *) fail "$UNIT_KDE does not get XDG_CURRENT_DESKTOP=KDE (its Environment: $kenv)" ;;
    esac
fi

# ---------------------------------------------------------------------------
step "4/7  Session environment that systemd hands to the portals"
menv=$(systemctl --user show-environment 2>/dev/null)
mget() { printf '%s\n' "$menv" | sed -n "s/^$1=//p" | head -n 1; }
if [ "$(mget XDG_CURRENT_DESKTOP)" != "SHADOW:KDE" ] || [ "$(mget WAYLAND_DISPLAY)" != "$WAYLAND_DISPLAY" ] \
    || [ "$(mget XDG_MENU_PREFIX)" != "shadow-" ]; then
    names=""
    for v in $SESSION_VARS; do
        if [ -n "${!v+x}" ]; then names="$names $v"; fi
    done
    # shellcheck disable=SC2086
    if dbus-update-activation-environment --systemd $names XDG_CURRENT_DESKTOP=SHADOW:KDE XDG_MENU_PREFIX=shadow-; then
        warn "published this session's environment to systemd again (it was incomplete)"
    else
        fail "could not publish the session environment (dbus-update-activation-environment)"
    fi
    menv=$(systemctl --user show-environment 2>/dev/null)
fi
if [ "$(mget XDG_CURRENT_DESKTOP)" = "SHADOW:KDE" ] && [ -n "$(mget WAYLAND_DISPLAY)" ]; then
    ok "XDG_CURRENT_DESKTOP=SHADOW:KDE WAYLAND_DISPLAY=$(mget WAYLAND_DISPLAY) XDG_MENU_PREFIX=$(mget XDG_MENU_PREFIX)"
else
    fail "systemd still lacks XDG_CURRENT_DESKTOP=SHADOW:KDE or WAYLAND_DISPLAY"
fi
# KWin finds the portal's permission (its desktop entry) in the application
# database; rebuild it with the Shadow menu.
if command -v kbuildsycoca6 >/dev/null 2>&1; then
    if XDG_MENU_PREFIX=shadow- kbuildsycoca6 >/dev/null 2>&1; then
        ok "application database rebuilt"
    else
        warn "kbuildsycoca6 failed"
    fi
fi

# ---------------------------------------------------------------------------
step "5/7  Restart the portals"
systemctl --user stop "$UNIT_FE" "$UNIT_KDE" "$UNIT_GTK" >/dev/null 2>&1
# Copies started by hand (or forked by D-Bus without systemd) keep the bus
# names with their old environment. A copy inside its own unit was started by
# systemd after the stop above (an application can ask for a portal at any
# moment), so it already has the fixed environment: leave it alone.
strays=""
for p in $(own_pids_of "$BIN_FE" "$BIN_KDE" "$BIN_GTK"); do
    case "$(unit_of_pid "$p")" in
        "$UNIT_FE" | "$UNIT_KDE" | "$UNIT_GTK") ;;
        *) strays="$strays $p" ;;
    esac
done
if [ -n "$strays" ]; then
    warn "stopping portal processes that run outside systemd:$strays"
    # shellcheck disable=SC2086
    kill -TERM $strays 2>/dev/null
    # shellcheck disable=SC2086
    if ! wait_until 5 pids_gone $strays; then
        # shellcheck disable=SC2086
        kill -KILL $strays 2>/dev/null
        # shellcheck disable=SC2086
        wait_until 3 pids_gone $strays || fail "could not stop:$strays"
    fi
    # The bus frees their names a moment after they exit; a back-end started
    # before that gives up at once (it does not queue for the name).
    wait_until 5 names_released || warn "a stopped portal still holds its D-Bus name"
fi
systemctl --user reset-failed "$UNIT_FE" "$UNIT_KDE" "$UNIT_GTK" >/dev/null 2>&1
# The back-end first, so that the front-end reads its finished ScreenCast interface.
if timeout --foreground 60 systemctl --user start "$UNIT_KDE"; then
    ok "KDE back-end started by systemd"
else
    fail "systemd could not start $UNIT_KDE"
fi
# Start the GTK back-end explicitly as well: when an earlier start of a portal
# was interrupted, D-Bus keeps that request pending for up to two minutes and
# the front-end would wait for it; a start through systemd answers it at once.
if [ "$(systemctl --user show -p LoadState --value "$UNIT_GTK" 2>/dev/null)" = loaded ]; then
    timeout --foreground 30 systemctl --user start "$UNIT_GTK" >/dev/null 2>&1 \
        || warn "systemd could not start $UNIT_GTK (only some file dialogs use it)"
fi

# ---------------------------------------------------------------------------
step "6/7  Check the KDE back-end"
wait_until 10 has_owner "$NAME_KDE"
kpid=$(owner_pid "$NAME_KDE")
if [ -z "$kpid" ]; then
    fail "nothing owns $NAME_KDE"
else
    upid=$(unit_pid "$UNIT_KDE")
    if [ "$kpid" = "$upid" ]; then
        ok "$NAME_KDE is served by $UNIT_KDE (PID $kpid)"
    else
        fail "$NAME_KDE is held by PID $kpid, not by $UNIT_KDE (PID ${upid:-none})"
    fi
    v=$(proc_env "$kpid" XDG_CURRENT_DESKTOP)
    if [ "$PICKER_OK" = 0 ]; then
        :   # kept out of KDE mode on purpose (step 3)
    elif [ "$v" = KDE ]; then
        ok "its XDG_CURRENT_DESKTOP is KDE"
    else
        fail "its XDG_CURRENT_DESKTOP is '$v' (must be KDE)"
    fi
    v=$(proc_env "$kpid" WAYLAND_DISPLAY)
    if [ -n "$v" ]; then ok "its WAYLAND_DISPLAY is $v"; else fail "it has no WAYLAND_DISPLAY"; fi
    v=$(readlink "/proc/$kpid/exe" 2>/dev/null)
    if [ "$v" = "$BIN_KDE" ]; then
        EXE_OK=1
        ok "it runs $BIN_KDE, the path KWin's permission check expects"
    else
        warn "it runs '$v'; KWin grants screen capture only to $BIN_KDE"
    fi
fi
if [ "$PICKER_OK" = 0 ]; then
    :   # no ScreenCast without KDE mode; the missing packages are the problem
elif introspect_has "$NAME_KDE" org.freedesktop.impl.portal.ScreenCast; then
    ok "it serves org.freedesktop.impl.portal.ScreenCast"
else
    fail "it does not serve org.freedesktop.impl.portal.ScreenCast"
fi
probe_loop() {   # tag
    local n=1
    probe=2
    while [ "$n" -le 16 ]; do
        probe_screencast "$1$n"
        probe=$?
        [ "$probe" -eq 1 ] || break
        sleep 0.5
        n=$((n + 1))
    done
}
probe=3
[ "$PICKER_OK" = 1 ] && probe_loop a
if [ "$probe" -eq 1 ]; then
    # KWin looks the portal's permission up in its application database. Build
    # it again from the Shadow menu and give the back-end a new connection.
    note "KWin refused the screen-capture protocol; rebuilding the application database and retrying"
    if command -v kbuildsycoca6 >/dev/null 2>&1; then
        XDG_MENU_PREFIX=shadow- kbuildsycoca6 --noincremental >/dev/null 2>&1
    fi
    systemctl --user stop "$UNIT_KDE" >/dev/null 2>&1
    systemctl --user reset-failed "$UNIT_KDE" >/dev/null 2>&1
    timeout --foreground 60 systemctl --user start "$UNIT_KDE" >/dev/null 2>&1
    wait_until 10 has_owner "$NAME_KDE"
    probe_loop b
fi
case "$probe" in
    0) ok "KWin gives it the screen-capture protocol (zkde_screencast_unstable_v1)" ;;
    1) fail "KWin does not give it the screen-capture protocol (zkde_screencast_unstable_v1)"
       note "KWin grants it only to $BIN_KDE, through $PORTAL_DESKTOP"
       note "(X-KDE-Wayland-Interfaces). Is kwin-common installed (KWin's screencast plug-in)?"
       note "Is that desktop file there and unchanged (sudo apt install --reinstall xdg-desktop-portal-kde)?"
       note "Do not switch KWin's permission checks off for every program: send the log instead." ;;
    3) note "(screen-capture check skipped: the picker packages are missing)" ;;
    *) fail "could not ask the back-end for a screen-cast session: $PROBE_OUT" ;;
esac

# ---------------------------------------------------------------------------
step "7/7  Start and check the portal front-end"
fe_up() { timeout --foreground 60 systemctl --user start "$UNIT_FE"; }
if fe_up; then ok "front-end started by systemd"; else fail "systemd could not start $UNIT_FE"; fi
st=$(fe_prop AvailableSourceTypes)
cm=$(fe_prop AvailableCursorModes)
if [ "${st:-0}" = 0 ] || [ "${cm:-0}" = 0 ]; then
    # It read the back-end before its ScreenCast existed: start it again.
    systemctl --user stop "$UNIT_FE" >/dev/null 2>&1
    systemctl --user reset-failed "$UNIT_FE" >/dev/null 2>&1
    fe_up >/dev/null 2>&1
    st=$(fe_prop AvailableSourceTypes)
    cm=$(fe_prop AvailableCursorModes)
fi
fpid=$(owner_pid "$NAME_FE")
if [ -n "$fpid" ] && [ "$fpid" = "$(unit_pid "$UNIT_FE")" ]; then
    ok "$NAME_FE is served by $UNIT_FE (PID $fpid)"
    v=$(proc_env "$fpid" XDG_CURRENT_DESKTOP)
    case ":$(printf '%s' "$v" | tr '[:upper:]' '[:lower:]'):" in
        *:shadow:* | *:kde:*) ok "its XDG_CURRENT_DESKTOP is $v, so it reads shadow-portals.conf" ;;
        *) fail "its XDG_CURRENT_DESKTOP is '$v' (needs SHADOW:KDE)" ;;
    esac
else
    fail "$NAME_FE is held by PID ${fpid:-none}, not by $UNIT_FE"
fi
if introspect_has "$NAME_FE" org.freedesktop.portal.ScreenCast; then
    ok "applications now find org.freedesktop.portal.ScreenCast"
else
    fail "the front-end does not offer org.freedesktop.portal.ScreenCast"
fi
if [ "${st:-0}" != 0 ] && [ "${cm:-0}" != 0 ]; then
    ok "screens and windows can be shared (source types $st, cursor modes $cm)"
else
    fail "the front-end reports nothing to share (source types '$st', cursor modes '$cm')"
fi
# Only this boot and this run: an older process with the same PID may have
# logged the same words.
if [ -n "$fpid" ] && journalctl --user -b --since "@$START_TS" _PID="$fpid" --no-pager -o cat 2>/dev/null \
    | grep -qE 'last-resort fallback|Failed to create screen cast proxy'; then
    fail "the front-end logged a configuration problem (see the diagnostics)"
fi
# PipeWire carries the picture; KWin captures only with OpenGL compositing.
if systemctl --user is-active --quiet pipewire.socket || systemctl --user is-active --quiet pipewire.service; then
    ok "PipeWire is running"
else
    systemctl --user start pipewire.socket pipewire.service >/dev/null 2>&1
    if systemctl --user is-active --quiet pipewire.service; then warn "started PipeWire"; else fail "PipeWire is not running"; fi
fi
if systemctl --user is-active --quiet wireplumber.service; then
    ok "WirePlumber is running"
else
    systemctl --user start wireplumber.service >/dev/null 2>&1
    if systemctl --user is-active --quiet wireplumber.service; then warn "started WirePlumber"; else fail "WirePlumber is not running"; fi
fi
comp=$(gdbus call --session --timeout 10 --dest org.kde.KWin --object-path /KWin \
    --method org.kde.KWin.supportInformation 2>/dev/null | grep -o 'Compositing Type: [A-Za-z]*' | head -n 1)
case "$comp" in
    *OpenGL*) ok "KWin composites with OpenGL" ;;
    *QPainter*) fail "KWin draws in software (QPainter) mode; it can only screen-cast with OpenGL (GPU driver missing?)" ;;
    *) note "(could not ask KWin which renderer it uses)" ;;
esac

# ---------------------------------------------------------------------------
step "Security: KWin's per-program permission check"
# KWin reads KWIN_WAYLAND_NO_PERMISSION_CHECKS once, when it starts. This
# terminal descends from KWin, so its environment shows how KWin was started.
SESSION_BYPASS=0
if [ "${KWIN_WAYLAND_NO_PERMISSION_CHECKS:-}" = 1 ] || [ "${KWIN_SCREENSHOT_NO_PERMISSION_CHECKS:-}" = 1 ]; then
    SESSION_BYPASS=1
fi
mapfile -t bypass < <(bypass_files)
ready=1
grep -qs '^X-KDE-Wayland-Interfaces=.*zkde_screencast_unstable_v1' "$PORTAL_DESKTOP" || ready=0
grep -qsxF "Exec=$BIN_KDE" "$PORTAL_DESKTOP" || ready=0
[ -f /etc/xdg/menus/shadow-applications.menu ] || ready=0
# KWin matches the running back-end's own executable against that entry, and
# packaged files that differ must be put back first.
if [ "$EXE_OK" != 1 ] || [ -n "$repair" ]; then ready=0; fi
if [ "${#bypass[@]}" -gt 0 ]; then
    warn "KWIN_WAYLAND_NO_PERMISSION_CHECKS / KWIN_SCREENSHOT_NO_PERMISSION_CHECKS is set in: ${bypass[*]}"
    note "Screen sharing does NOT need it: KWin grants the portal screen capture through"
    note "$PORTAL_DESKTOP."
    note "It lets EVERY program, sandboxed ones included, type keystrokes into other windows,"
    note "record the screen without asking, read all window titles and put a window above"
    note "the lock screen that receives what you type there."
    answer=n
    if [ "$ready" = 0 ]; then
        note "Not offered yet: KWin could not recognise the portal by its desktop entry"
        note "($PORTAL_DESKTOP, /etc/xdg/menus/shadow-applications.menu"
        note "or the program file the back-end runs). Fix the problems above first, e.g.:"
        note "sudo apt install --reinstall xdg-desktop-portal-kde"
    elif [ "$PICKER_OK" = 0 ] || [ "$probe" -ne 0 ] || [ "$FAILS" -ne 0 ]; then
        note "Not offered yet: screen sharing must pass first. Run this script again then."
    elif [ -t 0 ]; then
        note "Removing it changes nothing in this session (KWin read it when it started);"
        note "after your next login KWin's per-program check is on again."
        printf '         Remove it now (copies of the changed files are kept)? [Y/n] '
        read -r answer || answer=n
    fi
    case "$answer" in
        [Nn]*) ;;
        *)
            SUDO_OK=1
            for f in "${bypass[@]}"; do
                if ! { [ -O "$f" ] && [ -w "$f" ]; }; then SUDO_OK=0; fi
            done
            if [ "$SUDO_OK" = 0 ]; then
                note "(sudo may also print 'unable to resolve host ...'; that is harmless)"
                if sudo -v; then SUDO_OK=1; else note "sudo failed: only files in your home are changed"; fi
            fi
            remove_bypass "${bypass[@]}"
            ;;
    esac
    mapfile -t bypass < <(bypass_files)
fi
if [ "${#bypass[@]}" -eq 0 ]; then
    # /etc/environment also reached the systemd user manager (environment.d).
    # unset-environment cannot drop a value that came from there; a reload,
    # which reads the files again, does.
    if systemctl --user show-environment 2>/dev/null | grep -qE '^KWIN_(WAYLAND|SCREENSHOT)_NO_PERMISSION_CHECKS='; then
        systemctl --user unset-environment KWIN_WAYLAND_NO_PERMISSION_CHECKS KWIN_SCREENSHOT_NO_PERMISSION_CHECKS
        systemctl --user daemon-reload
    fi
    if [ "$SESSION_BYPASS" = 1 ]; then
        BYPASS_LEFT=1
        ok "no file sets it any more; KWin keeps it until you log out"
    else
        ok "not set: KWin decides per program which ones may capture the screen or inject input"
    fi
else
    BYPASS_LEFT=2
    warn "still set in: ${bypass[*]}"
    for f in "${bypass[@]}"; do
        case "$f" in
            "$HOME"/*) note "    sed -i -E '/$BYPASS_RE/d' $f" ;;
            *) note "    sudo sed -i -E '/$BYPASS_RE/d' $f" ;;
        esac
    done
fi

# ---------------------------------------------------------------------------
# plasma-workspace brings the KDE activity manager, which KWin starts at
# login; by default it keeps a history of the files and applications you use.
if [ -x /usr/lib/x86_64-linux-gnu/libexec/kactivitymanagerd ] && command -v kwriteconfig6 >/dev/null 2>&1 \
    && command -v kreadconfig6 >/dev/null 2>&1; then
    step "Privacy: activity history"
    cur=$(kreadconfig6 --file kactivitymanagerd-pluginsrc --group "$KAMD_GROUP" --key what-to-remember 2>/dev/null)
    if [ -z "$cur" ]; then
        if kwriteconfig6 --file kactivitymanagerd-pluginsrc --group "$KAMD_GROUP" --key what-to-remember 2; then
            ok "the KDE activity manager keeps no history of opened files and applications"
            note "(undo: kwriteconfig6 --file kactivitymanagerd-pluginsrc --group $KAMD_GROUP --key what-to-remember --delete)"
        else
            warn "could not write ~/.config/kactivitymanagerd-pluginsrc"
        fi
    else
        ok "activity history setting already chosen (what-to-remember=$cur); left as it is"
    fi
fi

# ---------------------------------------------------------------------------
printf '\n'
if [ "$FAILS" -eq 0 ]; then
    printf '%s\n' \
        'PASS: screen sharing is fixed.' \
        '' \
        '  1. Quit Discord, Vesktop, Chrome and OBS completely (Discord: tray icon, Quit' \
        '     Discord) and start them again.' \
        '  2. Share your screen: a "Screen Sharing" window lists your screens and' \
        '     windows (tabs Screens / Windows); pick one and press Share.'
    if [ "$REFRESH_OK" = 1 ]; then
        printf '%s\n' '  3. Nothing to redo after a reboot: the fix applies itself at every login.'
    else
        printf '%s\n' "  3. After each login run: systemctl --user restart $UNIT_FE"
    fi
    case "$BYPASS_LEFT" in
        0) printf '%s\n' '  KWin granted the portal screen capture through its normal per-program check.' ;;
        1) printf '%s\n' '  4. After your next login, run this script once more. It must say PASS again;' \
               '     that run checks screen sharing with KWin'"'"'s per-program check switched on.' ;;
        2) printf '%s\n' '  4. KWin'"'"'s permission checks are still switched off for every program; remove' \
               '     the lines listed under "Security", log out and in, and run this script again.' ;;
    esac
    case " $had_pkgs " in
        *" plasma-workspace "*)
            printf '%s\n' '  The login screen may now also list "Plasma" sessions (they come with' \
                '  plasma-workspace); keep choosing Shadow Horizon.' ;;
    esac
else
    printf 'FAIL: %s problem(s), marked [FAIL] above.\n' "$FAILS"
    if [ "$PICKER_OK" = 0 ]; then
        printf '%s\n' "  The screen picker cannot open until you install: sudo apt install --no-install-recommends $pkgs" \
            "  Then run this script again."
    fi
    if [ -n "$repair" ]; then
        printf '%s\n' "  Packaged portal files were changed; put them back with: sudo apt install --reinstall $repair" \
            "  Then run this script again."
    fi
    printf '\n--- diagnostics ---\n'
    for pair in "$UNIT_KDE=$NAME_KDE" "$UNIT_FE=$NAME_FE"; do
        u=${pair%%=*}
        n=${pair#*=}
        printf '\n# %s (unit PID %s, bus owner PID %s)\n' "$u" "$(unit_pid "$u")" "$(owner_pid "$n")"
        p=$(owner_pid "$n")
        if [ -n "$p" ]; then
            printf '  exe: %s\n' "$(readlink "/proc/$p/exe" 2>/dev/null)"
            { tr '\0' '\n' < "/proc/$p/environ"; } 2>/dev/null \
                | grep -E '^(XDG_CURRENT_DESKTOP|WAYLAND_DISPLAY|DISPLAY|QT_QPA_PLATFORM|XDG_MENU_PREFIX)=' | sed 's/^/  /'
        fi
        jl=$(journalctl --user -u "$u" --since "@$START_TS" --no-pager -o short 2>&1 | tail -n 40)
        if [ -n "$jl" ]; then
            printf '%s\n' "$jl" | sed 's/^/  /'
        else
            printf '  (no journal lines; as root: journalctl -b _SYSTEMD_USER_UNIT=%s)\n' "$u"
        fi
    done
    printf '\n# systemd environment\n'
    systemctl --user show-environment 2>/dev/null \
        | grep -E '^(XDG_CURRENT_DESKTOP|WAYLAND_DISPLAY|DISPLAY|XDG_MENU_PREFIX|XDG_SESSION_TYPE|KWIN_[A-Z_]*)=' | sed 's/^/  /'
    printf '\n# systemctl --user cat %s\n' "$UNIT_KDE"
    systemctl --user cat "$UNIT_KDE" 2>&1 | sed 's/^/  /'
fi
[ -n "$BACKUP" ] && printf '\nFiles moved aside or copied: %s\n' "$BACKUP"
[ -n "$LOG" ] && printf 'Log of this run: %s\n' "$LOG"

# Exit status 0 = PASS, 1 = FAIL; close_log (EXIT trap) flushes the log.
[ "$FAILS" -eq 0 ]
