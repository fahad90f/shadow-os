# SPDX-License-Identifier: GPL-3.0-or-later
# Permission errors as an unprivileged user (run with tests/run-user.sh).
T=/tmp/files-test-perm
fixture() {
    rm -rf "$T"; mkdir -p "$T/mine" "$T/readonly" "$T/noentry"
    echo own > "$T/mine/own.txt"; echo public > "$T/readonly/public.txt"; echo secret > "$T/readonly/secret.txt"
    echo hidden > "$T/noentry/x.txt"
    chmod 600 "$T/readonly/secret.txt"; chmod 700 "$T/noentry"; chmod 755 "$T" "$T/readonly"
    chown -R nobody:nogroup "$T/mine"
}
cleanup() { rm -rf "$T"; }
if [ -n "$FIXTURE_ONLY" ]; then fixture; return; fi

echo "running as: $(id -un)"
app_start "$T/readonly"
snap 1-readonly-folder
key ctrl+shift+n; sleep 0.8; snap 2-newfolder-not-offered; key Escape
park; key Menu; sleep 0.6; snap 3-background-menu; key Escape
# copy a readable and an unreadable file into my own folder
key ctrl+a ctrl+c; goto "$T/mine"; key ctrl+v; sleep 2.5; snap 4-unreadable-source
click 829 432; sleep 2                                   # "Skip"
echo "1 copy readable + unreadable, skipped: mine=[$(tree $T/mine)]"
# permanent delete, trash and rename inside a folder I cannot write to
goto "$T/readonly"
key p shift+Delete; sleep 1; snap 5a-delete-confirm; click 790 442; sleep 2; snap 5-delete-denied
echo "2 delete in read-only folder : readonly=[$(ls $T/readonly | tr '\n' ' ')]"
sleep 4; key Delete; sleep 2.5; snap 6-trash-denied
echo "3 trash in read-only folder  : readonly=[$(ls $T/readonly | tr '\n' ' ')] trash=[$(ls $HOME/.local/share/Trash/files 2>/dev/null | tr '\n' ' ')]"
sleep 4; key F2; sleep 0.6; key ctrl+a; type_text "renamed.txt"; key Return; sleep 2.5; snap 7-rename-denied
echo "4 rename in read-only folder : readonly=[$(ls $T/readonly | tr '\n' ' ')]"
key Escape
# paste into it: not offered, and Ctrl+V does nothing
goto "$T/mine"; key o ctrl+c; goto "$T/readonly"; key ctrl+v; sleep 2
echo "5 paste into read-only folder: readonly=[$(ls $T/readonly | tr '\n' ' ')]"
# a folder I may not enter
goto "$T/noentry"; sleep 1; snap 8-no-entry
# permissions of a file owned by someone else
goto "$T/readonly"; key p alt+Return; sleep 1.2; click 521 289; sleep 0.8; snap 9-foreign-permissions
key Escape
app_stop
