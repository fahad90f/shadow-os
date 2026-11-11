#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The look of a third-party Qt Widgets application under the SHADOW defaults,
# in the nested KWin Wayland session. Run from the repository root:
#
#   scripts/dev.sh exec --dir themes -- sh -c 'cmake -S /src/desktop/themes/tests -B /build/themes/tests -G Ninja && cmake --build /build/themes/tests'
#   scripts/dev.sh exec --dir themes -- sh -c 'cmake -S /src -B /build/themes-data -DSHADOW_DATA_ONLY=ON -DCMAKE_INSTALL_PREFIX=/usr && rm -rf /tmp/themes-root && DESTDIR=/tmp/themes-root cmake --install /build/themes-data --component shadow-themes'
#   scripts/dev.sh nested --dir themes -- '. /src/desktop/themes/tests/qt-test.sh'
#
# The staged install (/tmp/themes-root) is made visible the way the package
# would be: kdeglobals as system default (XDG_CONFIG_DIRS), colour schemes
# and icon themes through XDG_DATA_DIRS; KWin, which is already running, is
# given the same defaults through the session's own kdeglobals.
# Screenshots: themes-qt-*.png; reports of what the application received:
# $SHADOW_LOGS/qt-report-*.log.
root=/tmp/themes-root
demo=/build/themes/tests/bin/shadow-themes-widgets-demo
export XDG_CONFIG_DIRS="$root/etc/xdg:/etc/xdg" XDG_DATA_DIRS="$root/usr/share:/usr/local/share:/usr/share"
export QT_QPA_PLATFORMTHEME=kde
mkdir -p "$HOME/.local/share"
cp -r "$root/usr/share/color-schemes" "$root/usr/share/icons" "$HOME/.local/share/"
cp "$root/etc/xdg/kdeglobals" "$HOME/.config/kdeglobals"
dbus-send --session --type=method_call --dest=org.kde.KWin /KWin org.kde.KWin.reconfigure
sleep 1

"$demo" --report "$SHADOW_LOGS/qt-report-dark.log" > "$SHADOW_LOGS/qt-demo.log" 2>&1 &
demo_pid=$!
sleep 4
shot themes-qt-dark-controls.png
xdo key alt+f; sleep 1; xdo key Down Down; sleep 0.5
shot themes-qt-dark-menu.png
xdo key Escape; xdo key Escape; sleep 0.3
kill $demo_pid; wait $demo_pid 2>/dev/null

"$demo" --page 1 >> "$SHADOW_LOGS/qt-demo.log" 2>&1 &
demo_pid=$!
sleep 3
shot themes-qt-dark-views.png
kill $demo_pid; wait $demo_pid 2>/dev/null

# Live switch to the light appearance with another accent, while the
# application runs.
"$demo" --page 1 --report "$SHADOW_LOGS/qt-report-start.log" >> "$SHADOW_LOGS/qt-demo.log" 2>&1 &
demo_pid=$!
sleep 3
sh "$root/usr/bin/shadow-apply-appearance" --mode light --accent violet > "$SHADOW_LOGS/qt-apply.log" 2>&1
sleep 3
shot themes-qt-light-violet-live.png
kill $demo_pid; wait $demo_pid 2>/dev/null
"$demo" --report "$SHADOW_LOGS/qt-report-light-violet.log" >> "$SHADOW_LOGS/qt-demo.log" 2>&1 &
demo_pid=$!
sleep 3
shot themes-qt-light-violet-controls.png
kill $demo_pid; wait $demo_pid 2>/dev/null
cp "$HOME/.config/kdeglobals" "$SHADOW_LOGS/qt-kdeglobals-after-apply.txt"
