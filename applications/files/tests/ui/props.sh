# SPDX-License-Identifier: GPL-3.0-or-later
# Properties dialog: general page, folder size, viewing and changing permissions.
T=/tmp/files-test-props; rm -rf "$T"; mkdir -p "$T/work/folder/inner"
printf '#!/bin/sh\necho hello\n' > "$T/work/script.sh"; chmod 640 "$T/work/script.sh"
head -c 5000 /dev/urandom > "$T/work/folder/a.bin"; head -c 7000 /dev/urandom > "$T/work/folder/inner/b.bin"
chmod 750 "$T/work/folder" "$T/work/folder/inner"; chmod 600 "$T/work/folder/a.bin" "$T/work/folder/inner/b.bin"
perm() { stat -c '%n=%a' "$@" | sed "s|$T/work/||" | tr '\n' ' '; }
echo "0 initial: $(perm $T/work/script.sh $T/work/folder $T/work/folder/a.bin $T/work/folder/inner $T/work/folder/inner/b.bin)"
app_start "$T/work"
key s alt+Return; sleep 1.2; snap 1-general
click 523 "${TAB_Y:-329}"; sleep 0.8; snap 2-permissions
# script.sh 640 -> tick Owner/execute, Others/read -> 744
click 833 475; click 726 535; sleep 0.4; snap 3-edited
click 702 585; sleep 1.5; snap 4-applied          # "Apply permissions"
echo "1 script.sh after apply: $(perm $T/work/script.sh)"
key Escape; sleep 1.2
# folder 750 -> Others read + enter, also for everything inside
key f alt+Return; sleep 2; snap 5-folder-general
click 523 312; sleep 0.6
click 739 517; click 839 517; sleep 0.3
click "${REC_X:-397}" "${REC_Y:-551}"; sleep 0.4; snap 6-folder-recursive
click "${APPLY_X:-702}" "${APPLY_Y:-601}"; sleep 2; snap 7-folder-applied
echo "2 folder after recursive apply: $(perm $T/work/folder $T/work/folder/a.bin $T/work/folder/inner $T/work/folder/inner/b.bin)"
key Escape
app_stop
