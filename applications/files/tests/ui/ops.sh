# SPDX-License-Identifier: GPL-3.0-or-later
# Keyboard-driven file operations; the on-disk state is printed after every step.
T=/tmp/files-test-ops; rm -rf "$T"; mkdir -p "$T/src/sub" "$T/dst"
echo hello > "$T/src/a.txt"; echo world > "$T/src/b.txt"; echo deep > "$T/src/sub/c.txt"
echo "0 initial            src: $(tree $T/src)| dst: $(tree $T/dst)"
app_start "$T/src"
key ctrl+a ctrl+c
goto "$T/dst"
key ctrl+v; sleep 2
echo "1 copy+paste         dst: $(tree $T/dst)"
key a F2; sleep 0.5
snap 1-rename
key ctrl+a; type_text "renamed.txt"; key Return; sleep 1.2
echo "2 rename (F2)        dst: $(tree $T/dst)"
key ctrl+shift+n; sleep 0.8
snap 2-newfolder
key ctrl+a; type_text "NewDir"; key Return; sleep 1.2
echo "3 new folder         dst: $(tree $T/dst)"
key b Delete; sleep 1.5
echo "4 trash (Delete)     dst: $(tree $T/dst)| trash: $(ls $HOME/.local/share/Trash/files 2>/dev/null | tr '\n' ' ')"
echo "  trashinfo: $(cat $HOME/.local/share/Trash/info/b.txt.trashinfo 2>/dev/null | tr '\n' ' ')"
key ctrl+z; sleep 2
echo "5 undo (Ctrl+Z)      dst: $(tree $T/dst)| trash: $(ls $HOME/.local/share/Trash/files 2>/dev/null | tr '\n' ' ')"
key s ctrl+x; sleep 1.3; key N Return; sleep 1
key ctrl+v; sleep 2
echo "6 cut+paste          dst: $(tree $T/dst)"
key BackSpace; sleep 1
key r shift+Delete; sleep 1
snap 3-delete-confirm
echo "7 before confirming  dst: $(tree $T/dst)"
key Escape; sleep 0.8
echo "8 Escape = cancel    dst: $(tree $T/dst)"
key shift+Delete; sleep 1
click 781 447        # "Delete"
sleep 1.5
echo "9 confirmed          dst: $(tree $T/dst)| trash: $(ls $HOME/.local/share/Trash/files 2>/dev/null | tr '\n' ' ')"
snap 4-end
app_stop
echo "src untouched: $(tree $T/src)"
