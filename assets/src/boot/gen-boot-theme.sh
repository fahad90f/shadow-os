#!/bin/bash
# Regenerates the binary parts of the boot branding:
#   OUTDIR/plymouth/shadow/   images of the Plymouth splash (gen-plymouth-theme.py)
#   OUTDIR/grub/shadow/       GRUB menu theme (gen-grub-theme.sh, if it is there)
#
#   gen-boot-theme.sh OUTDIR
#
# This script only calls the two generators; each documents what it needs.
# The splash images need python3, rsvg-convert and the Inter font, all present
# in the build chroot:
#   scripts/dev.sh exec -- /src/assets/src/boot/gen-boot-theme.sh /tmp/boot-theme
# then copy /tmp/boot-theme/plymouth/shadow/*.png over
# assets/boot/plymouth/shadow/ (the script and the .plymouth file there are
# written by hand) and commit them. The same input gives the same files, so
# "git status" shows whether the committed images are current.
# The menu's fonts also need grub-mkfont, which the build chroot does not
# have (see gen-grub-theme.sh). Without it only the menu's images are made
# (--images-only) and the committed .pf2 files stay as they are.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:?output directory}"

mkdir -p "$OUT/plymouth/shadow"
python3 "$HERE/gen-plymouth-theme.py" "$OUT/plymouth/shadow"

if [ -x "$HERE/gen-grub-theme.sh" ]; then
    mkdir -p "$OUT/grub/shadow"
    if command -v grub-mkfont >/dev/null; then
        "$HERE/gen-grub-theme.sh" "$OUT/grub/shadow"
    else
        echo "gen-boot-theme: grub-mkfont not found, GRUB fonts not generated" >&2
        "$HERE/gen-grub-theme.sh" --images-only "$OUT/grub/shadow"
    fi
else
    echo "gen-boot-theme: no gen-grub-theme.sh here, GRUB theme not generated" >&2
fi
echo "generated: $OUT"
