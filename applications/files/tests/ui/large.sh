# SPDX-License-Identifier: GPL-3.0-or-later
# A large copy (one 900 MiB file and 3 072 small ones): progress indicator,
# a usable window while it runs, integrity of the result, and cancelling.
T=/tmp/files-test-large; rm -rf "$T"; mkdir -p "$T/src/many" "$T/dst" "$T/dst2"
head -c 900M /dev/urandom > "$T/src/big.bin"
head -c 3M /dev/urandom | split -b 1k -a 5 - "$T/src/many/f"
sum="$(sha256sum < "$T/src/big.bin")"
count() { echo "$(find "$1" -type f | wc -l) files, $(du -sm "$1" | cut -f1) MiB"; }
echo "0 source        : $(count $T/src)"
app_start "$T/src"
key ctrl+a ctrl+c; goto "$T/dst"; key ctrl+v; sleep 2.5
snap 1-pill
echo "1 while copying : $(count $T/dst)"
click 1150 26; sleep 0.8       # operations indicator
snap 2-popup
key Escape
# the window stays usable during the copy: switch to the details view and back, open a tab
t0=$(date +%s.%N); key ctrl+2; sleep 0.5; snap 3-list-during-copy; key ctrl+1 ctrl+t; sleep 0.5; key ctrl+w
echo "  still copying after the view changes: $(count $T/dst)"
for i in $(seq 240); do [ "$(find "$T/dst" -type f | wc -l)" = 3073 ] && break; sleep 1; done
sleep 3
echo "2 finished      : $(count $T/dst)"
[ "$(sha256sum < "$T/dst/big.bin")" = "$sum" ] && echo "  big.bin checksum: identical" || echo "  big.bin checksum: DIFFERENT"
diff -rq "$T/src" "$T/dst" > /dev/null && echo "  diff -r src dst: identical" || echo "  diff -r src dst: DIFFERENT"
snap 4-finished
# second copy, cancelled from the operations list
goto "$T/src"; key ctrl+a ctrl+c; goto "$T/dst2"; key ctrl+v; sleep 3
click 1150 26; sleep 0.8
echo "3 while copying : $(count $T/dst2)"
click 1164 95; sleep 2.5        # cancel button of the operation
a="$(count $T/dst2)"; sleep 3; b="$(count $T/dst2)"
echo "4 after cancel  : $a; 3 s later: $b"
snap 5-cancelled
app_stop
rm -rf "$T"
