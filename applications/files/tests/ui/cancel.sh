# SPDX-License-Identifier: GPL-3.0-or-later
# Cancelling a running copy from the operations list.
T=/tmp/files-test-cancel; rm -rf "$T"; mkdir -p "$T/src/many" "$T/dst"
head -c 4M /dev/urandom | split -b 1k -a 4 - "$T/src/many/f"
count() { echo "$(find "$1" -type f | wc -l) files"; }
echo "0 source        : $(count $T/src)"
app_start "$T/src"
key ctrl+a ctrl+c; goto "$T/dst"; key ctrl+v; sleep 2.5
click 1150 26; sleep 0.8; snap 1-running
echo "1 while copying : $(count $T/dst)"
click 1164 95; sleep 2.5        # cancel button of the operation
a="$(count $T/dst)"; sleep 4; b="$(count $T/dst)"
echo "2 after cancel  : $a; 4 s later: $b (the copy stopped; what was already copied stays)"
snap 2-cancelled
app_stop
rm -rf "$T"
