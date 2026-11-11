#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs the NEXUS lock screen in the real KScreenLocker greeter
# (kscreenlocker_greet --testing) under Xvfb, with the PAM files of this
# directory, and takes screenshots. To be started inside the build chroot:
#
#   scripts/dev.sh exec --dir themes -- sh /src/desktop/themes/tests/lockscreen-test.sh [options] [-- actions]
#
#   --size WxH        screen size (default 1600x900)
#   --no-wallpaper    do not point the lock screen at the wallpaper in the source tree
#   --via-config      find the shell package through plasmashellrc instead of --shell
#   --prefix NAME     screenshots are /shots/NAME-<label>.png (default themes-lock)
#   actions           shell commands run while the greeter is up; available:
#                     xdo <xdotool args>, shot <label>, $PASSWORD (the right password).
#                     Default: "shot start".
#
# The greeter authenticates the user it runs as (root in the chroot, whose
# password is locked). In a private mount namespace /etc is replaced by a
# copy holding this directory's pam.d/kde* files and a shadow entry with a
# known password, so the real PAM stack (pam_unix through common-auth) is
# exercised and nothing in the chroot is modified.
# Output: /shots/NAME.log (stderr of the greeter), /shots/NAME.out (stdout:
# "Unlocked" after a successful authentication) and the exit status.
set -eu

size=1600x900 wallpaper=1 prefix=themes-lock via_config=0
while [ $# -gt 0 ]; do
    case "$1" in
        --size) size="$2"; shift 2 ;;
        --no-wallpaper) wallpaper=0; shift ;;
        --via-config) via_config=1; shift ;;
        --prefix) prefix="$2"; shift 2 ;;
        --) shift; break ;;
        *) echo "lockscreen-test: unknown option $1" >&2; exit 2 ;;
    esac
done
actions="${*:-shot start}"
PASSWORD='Nexus-test-42'

work="$(mktemp -d /tmp/nexus-lock-test.XXXXXX)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/share/plasma/shells" "$work/home/.config" "$work/xdg"
cp -r /src/desktop/themes/lockscreen/org.nexus.horizon "$work/share/plasma/shells/"
cp /src/desktop/themes/lockscreen/plasmashellrc "$work/xdg/"
if [ "$wallpaper" = 1 ]; then
    printf '[Greeter][LnF][General]\nwallpaper=/src/assets/wallpapers/nexus-horizon.jpg\n' > "$work/home/.config/kscreenlockerrc"
fi

cp -a /etc "$work/etc"
cp /src/desktop/themes/lockscreen/pam.d/kde /src/desktop/themes/lockscreen/pam.d/kde-fingerprint \
   /src/desktop/themes/lockscreen/pam.d/kde-smartcard "$work/etc/pam.d/"
hash="$(openssl passwd -6 "$PASSWORD")"
sed -i "s|^root:[^:]*:|root:$hash:|" "$work/etc/shadow"
# The real name the lock screen shows.
sed -i 's|^root:x:0:0:[^:]*:|root:x:0:0:Ada Lovelace:|' "$work/etc/passwd"

shell_arg="--shell org.nexus.horizon"
[ "$via_config" = 1 ] && shell_arg=""
cat > "$work/inner.sh" <<INNER
xdo() { xdotool "\$@"; }
shot() { import -window root "/shots/$prefix-\$1.png"; }
PASSWORD='$PASSWORD'
/usr/lib/x86_64-linux-gnu/libexec/kscreenlocker_greet --testing $shell_arg > "/shots/$prefix.out" 2> "/shots/$prefix.log" &
greeter=\$!
sleep \${NEXUS_LOCK_WAIT:-6}
$actions
sleep 1
if kill -0 \$greeter 2>/dev/null; then
    echo "greeter still running (not unlocked); terminating it" >> "/shots/$prefix.out"
    kill \$greeter 2>/dev/null
    wait \$greeter 2>/dev/null
else
    wait \$greeter; echo "greeter exit status \$?" >> "/shots/$prefix.out"
fi
true
INNER

export LIBGL_ALWAYS_SOFTWARE=1 QT_QPA_PLATFORM=xcb
export HOME="$work/home" XDG_RUNTIME_DIR="$work/run"; mkdir -m 700 -p "$XDG_RUNTIME_DIR"
export XDG_DATA_DIRS="$work/share:/usr/local/share:/usr/share" XDG_CONFIG_DIRS="$work/xdg:/etc/xdg"
export QT_LOGGING_RULES="kscreenlocker_greet.debug=true;qt.qpa.*=false"
unshare -m --propagation unchanged sh -c "mount --bind '$work/etc' /etc && exec dbus-run-session -- xvfb-run -a -s '-screen 0 ${size}x24' sh '$work/inner.sh'"
echo "---- stdout (/shots/$prefix.out)"; cat "/shots/$prefix.out"
echo "---- stderr (/shots/$prefix.log)"; grep -v '^$' "/shots/$prefix.log" | head -n "${NEXUS_LOCK_LOGLINES:-60}" || true
