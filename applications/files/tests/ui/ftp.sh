# SPDX-License-Identifier: GPL-3.0-or-later
# Network locations: "Connect to server" against a local FTP server (ftpd.py),
# browsing, download, upload, new folder, rename and delete on the server.
T=/tmp/files-test-ftp; rm -rf "$T"; mkdir -p "$T/server/pub" "$T/local"
echo "served by ftp" > "$T/server/readme.txt"; head -c 300000 /dev/urandom > "$T/server/pub/blob.bin"
echo "from the local disk" > "$T/local/upload.txt"
python3 /src/applications/files/tests/ui/ftpd.py "$T/server" 2121 > "$SHADOW_LOGS/ftpd.log" 2>&1 &
sleep 1
app_start "$T/local"
click 110 283; sleep 1; snap 1-dialog        # sidebar: Connect to server…
type_text "127.0.0.1"; key Tab; sleep 0.2; key ctrl+a; type_text "2121"; sleep 0.5; snap 2-filled
click 407 "${REMEMBER_Y:-492}"; sleep 0.3                   # "Keep this server in the sidebar"
snap 2b-remember; click 791 538; sleep 3; snap 3-connected
key ctrl+2; sleep 1; snap 4-list
# download: copy readme.txt and pub/ to the local folder
key ctrl+a ctrl+c; goto "$T/local"; key ctrl+v; sleep 3
echo "1 download: local=[$(tree $T/local)] readme='$(cat $T/local/readme.txt 2>/dev/null)' blob $(cmp -s $T/server/pub/blob.bin $T/local/pub/blob.bin && echo identical || echo DIFFERENT)"
# upload: copy upload.txt to the server
key u ctrl+c; key alt+Left; sleep 2; key ctrl+v; sleep 3
echo "2 upload  : server=[$(tree $T/server)] upload='$(cat $T/server/upload.txt 2>/dev/null)'"
# new folder, rename and delete on the server
key ctrl+shift+n; sleep 0.8; key ctrl+a; type_text "made-here"; key Return; sleep 2.5
echo "3 mkdir   : server=[$(tree $T/server)]"
sleep 1; key u; sleep 0.3; key F2; sleep 0.6; key ctrl+a; type_text "renamed.txt"; key Return; sleep 2.5
echo "4 rename  : server=[$(tree $T/server)]"
sleep 1; type_text "ren"; key shift+Delete; sleep 1; snap 5-delete-confirm
click 781 447; sleep 2.5
echo "5 delete  : server=[$(tree $T/server)]"
key F5; sleep 2; snap 6-end
app_stop
grep -c . "$SHADOW_LOGS/ftpd.log" | sed 's/^/ftpd.log lines: /'
