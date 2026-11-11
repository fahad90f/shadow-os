# SPDX-License-Identifier: GPL-3.0-or-later
# Failing operations (as root, so an immutable folder stands in for missing
# permissions): every failure is reported and nothing is lost.
T=/tmp/files-test-errors; chattr -i "$T/locked" 2>/dev/null; rm -rf "$T"; mkdir -p "$T/locked" "$T/src"
echo keep > "$T/locked/keep.txt"; echo new > "$T/src/new.txt"
chattr +i "$T/locked" || echo "chattr +i is not supported here; the steps below prove nothing"
app_start "$T/src"
# the folder is recognised as read-only: pasting is not offered and Ctrl+V does nothing
key ctrl+a ctrl+c; goto "$T/locked"; key ctrl+v; sleep 2.5; snap 1-paste-not-offered
echo "1 paste into the immutable folder : locked=[$(ls $T/locked | tr '\n' ' ')]"
sleep 4; key k Delete; sleep 2.5; snap 2-trash-failed
echo "2 trash                           : locked=[$(ls $T/locked | tr '\n' ' ')] trash=[$(ls $HOME/.local/share/Trash/files 2>/dev/null | tr '\n' ' ')]"
sleep 4; key shift+Delete; sleep 1; snap 3a-confirm; click 781 447; sleep 2.5; snap 3-delete-failed
echo "3 delete permanently              : locked=[$(ls $T/locked | tr '\n' ' ')]"
sleep 4; key F2; sleep 0.6; key ctrl+a; type_text "other.txt"; key Return; sleep 2.5; snap 4-rename-failed
echo "4 rename                          : locked=[$(ls $T/locked | tr '\n' ' ')]"
key Escape
click 1150 26; sleep 0.8; snap 6-failed-operations; key Escape
goto "/tmp/files-test-errors/does-not-exist"; sleep 1; snap 7-missing-folder
# a rename onto an existing name is refused before anything runs
goto "$T/src"; echo other > "$T/src/taken.txt"; sleep 1.5; key n F2; sleep 0.6; key ctrl+a; type_text "taken.txt"; key Return; sleep 2; snap 8-rename-taken
click 410 "${STOP_Y:-493}"; sleep 1.5                     # "Stop"
echo "5 rename onto an existing name    : src=[$(for f in $T/src/*; do printf "%s='%s' " "$(basename $f)" "$(cat $f)"; done)]"
app_stop
chattr -i "$T/locked"; rm -rf "$T"
