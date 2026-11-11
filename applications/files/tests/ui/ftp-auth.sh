# SPDX-License-Identifier: GPL-3.0-or-later
# A server that requires a password. KIO asks for it through the kpasswdserver
# module of kiod6 (package kio6), which the session bus starts on demand.
T=/tmp/files-test-ftpauth; rm -rf "$T"; mkdir -p "$T/server"
echo "private" > "$T/server/private.txt"
python3 /src/applications/files/tests/ui/ftpd.py "$T/server" 2122 shadow s3cret-pass > "$SHADOW_LOGS/ftpd-auth.log" 2>&1 &
sleep 1
# 1. the password service cannot start (the bus has no display to give it)
app_start "ftp://shadow@127.0.0.1:2122/"
sleep 3; snap 1-no-password-service
app_stop
# 2. as in a full session: the bus activates services with the session's display
dbus-update-activation-environment WAYLAND_DISPLAY QT_QPA_PLATFORM XDG_RUNTIME_DIR XDG_CURRENT_DESKTOP
app_start "ftp://shadow@127.0.0.1:2122/"
sleep 4; snap 2-password-dialog
key Escape; sleep 3; snap 3-cancelled                      # dismissing the prompt is not an empty folder
key F5; sleep 4
type_text "wrong"; key Return; sleep 3; snap 4-wrong-password
click 714 451; sleep 3                                    # "Retry"
type_text "s3cret-pass"; key Return; sleep 4; snap 5-listed
app_stop
echo "server log: $(tr '\n' ' ' < "$SHADOW_LOGS/ftpd-auth.log" | cut -c1-400)"
