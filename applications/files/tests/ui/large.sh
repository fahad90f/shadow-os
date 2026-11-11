# SPDX-License-Identifier: GPL-3.0-or-later
# A large copy (one 600 MiB file and 2000 small ones): progress indicator,
# a usable window while it runs, integrity of the result.
T=/tmp/files-test-large; rm -rf "$T"; mkdir -p "$T/src/many" "$T/dst"
head -c 600M /dev/urandom > "$T/src/big.bin"
head -c 2000K /dev/urandom | split -b 1k -a 4 - "$T/src/many/f"
sum="$(sha256sum < "$T/src/big.bin")"
count() { echo "$(find "$1" -type f | wc -l) files, $(du -sm "$1" | cut -f1) MiB"; }
echo "0 source        : $(count $T/src)"
app_start "$T/src"
key ctrl+a ctrl+c; goto "$T/dst"; key ctrl+v; sleep 0.8
snap 1-pill
echo "1 while copying : $(count $T/dst)"
click 1150 26; sleep 0.8       # operations indicator
snap 2-popup
key Escape
# the window stays usable during the copy: details view and back, a new tab and closing it
key ctrl+2; sleep 0.5; snap 3-list-during-copy; key ctrl+1 ctrl+t; sleep 0.5; key ctrl+w
echo "  after switching views and tabs: $(count $T/dst)"
for i in $(seq 110); do [ "$(find "$T/dst" -type f | wc -l)" = 2001 ] && [ "$(stat -c %s "$T/dst/big.bin" 2>/dev/null)" = 629145600 ] && break; sleep 1; done
sleep 3
echo "2 finished      : $(count $T/dst)"
[ "$(sha256sum < "$T/dst/big.bin")" = "$sum" ] && echo "  big.bin checksum: identical" || echo "  big.bin checksum: DIFFERENT"
diff -rq "$T/src" "$T/dst" > /dev/null && echo "  diff -r src dst: identical" || echo "  diff -r src dst: DIFFERENT"
snap 4-finished
app_stop
rm -rf "$T"
