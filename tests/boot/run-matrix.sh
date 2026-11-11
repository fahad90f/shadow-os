#!/bin/bash
# Run the live-boot test across every firmware/media combination that applies
# to the image's architecture and print a summary table.
#
#   tests/boot/run-matrix.sh <iso> [amd64|arm64] [results-dir]
set -u
ISO="${1:?usage: run-matrix.sh <iso> [arch] [results-dir]}"
ARCH="${2:-amd64}"
OUT="${3:-$(dirname "$0")/../../build/work/tests/boot-matrix-$ARCH}"
HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$OUT"

case "$ARCH" in
    amd64) COMBOS="uefi:cdrom uefi:usb uefi-secureboot:cdrom uefi-secureboot:usb bios:cdrom bios:usb" ;;
    arm64) COMBOS="uefi:cdrom uefi:usb" ;;
    *) echo "unsupported arch $ARCH" >&2; exit 2 ;;
esac

fail=0
: > "$OUT/summary.txt"
for combo in $COMBOS; do
    fw="${combo%%:*}"; media="${combo##*:}"
    log="$OUT/$fw-$media.log"
    rm -rf "$OUT/$fw-$media"
    if python3 "$HERE/test_live_boot.py" --iso "$ISO" --arch "$ARCH" --firmware "$fw" --media "$media" \
            --workdir "$OUT/$fw-$media" ${SHADOW_TEST_TIMEOUT:+--timeout "$SHADOW_TEST_TIMEOUT"} > "$log" 2>&1; then
        echo "PASS  $ARCH  $fw  $media" | tee -a "$OUT/summary.txt"
    else
        echo "FAIL  $ARCH  $fw  $media  (see $log)" | tee -a "$OUT/summary.txt"
        grep -a -E '^FAIL' "$log" | head -3
        fail=1
    fi
done
exit $fail
