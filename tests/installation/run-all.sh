#!/bin/bash
# Run the installer end-to-end scenarios and print a summary table.
#
#   tests/installation/run-all.sh <iso> [arch] [results-dir] [scenario:firmware ...]
#
# Default matrix (amd64): every scenario under UEFI, plus whole-disk installs
# under UEFI Secure Boot (enforcing) and legacy BIOS, and manual mode on BIOS.
set -u
ISO="${1:?usage: run-all.sh <iso> [arch] [results-dir] [scenario:firmware ...]}"
ARCH="${2:-amd64}"
OUT="${3:-$(dirname "$0")/../../build/work/tests/install-matrix-$ARCH}"
shift $(( $# < 3 ? $# : 3 ))
HERE="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "$OUT"

if [ $# -gt 0 ]; then
    COMBOS="$*"
elif [ "$ARCH" = "amd64" ]; then
    COMBOS="erase:uefi erase-luks:uefi erase:uefi-secureboot erase:bios alongside:uefi manual:uefi manual:bios erase-home:uefi refuse:uefi"
else
    COMBOS="erase:uefi erase-luks:uefi"
fi

fail=0
for combo in $COMBOS; do
    scenario="${combo%%:*}"; fw="${combo##*:}"
    dir="$OUT/$scenario-$fw"
    rm -rf "$dir"; mkdir -p "$dir"
    start=$(date +%s)
    if python3 -u "$HERE/test_install.py" --iso "$ISO" --arch "$ARCH" --firmware "$fw" --scenario "$scenario" \
            --workdir "$dir" > "$dir.log" 2>&1; then
        echo "PASS  $ARCH  $fw  $scenario  ($(( $(date +%s) - start ))s)" | tee -a "$OUT/summary.txt"
        rm -f "$dir"/*.qcow2            # keep logs, drop the multi-GB disk images
    else
        echo "FAIL  $ARCH  $fw  $scenario  ($(( $(date +%s) - start ))s)  see $dir.log" | tee -a "$OUT/summary.txt"
        grep -a -E '^FAIL' "$dir.log" | head -5
        fail=1
    fi
done
exit $fail
