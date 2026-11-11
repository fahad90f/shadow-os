#!/bin/bash
# Regenerates the binary parts of the boot branding from the logo, the
# wallpaper and the UI fonts:
#   assets/boot/grub/nexus/      GRUB menu theme (background, fonts, selection box)
#   assets/boot/plymouth/nexus/  Plymouth splash images
# Needs ImageMagick, rsvg-convert, grub-mkfont and the Inter / JetBrains Mono
# fonts — all present in the build chroot:
#   scripts/dev.sh exec -- /src/assets/src/boot/gen-boot-theme.sh /tmp/boot-theme
# then copy /tmp/boot-theme/{grub,plymouth} over assets/boot/.
set -euo pipefail
SRC="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:?output directory}"
G="$OUT/grub/nexus"; P="$OUT/plymouth/nexus"
mkdir -p "$G" "$P"

BG="#05070D"; ACCENT="#22D3EE"; SURFACE="#0F172A"

# ---- GRUB ---------------------------------------------------------------------
# Dark, calm background: the wallpaper darkened, so menu text stays legible.
convert "$SRC/wallpapers/nexus-horizon.jpg" -resize 1280x720^ -gravity center -extent 1280x720 \
    -fill "$BG" -colorize 55% -depth 8 -strip "$G/background.png"
rsvg-convert -w 96 -h 96 "$SRC/logos/nexus-symbol.svg" -o "$G/logo.png"
# Selected-entry highlight as a 9-slice box (GRUB wants select_{nw,n,ne,w,c,e,sw,s,se}.png).
convert -size 24x24 xc:none -fill "rgba(34,211,238,0.16)" -stroke "$ACCENT" -strokewidth 1 \
    -draw "roundrectangle 0.5,0.5 23,23 6,6" "$OUT/select.png"
i=0
for part in nw n ne w c e sw s se; do
    x=$(( (i % 3) * 8 )); y=$(( (i / 3) * 8 ))
    convert "$OUT/select.png" -crop "8x8+$x+$y" +repage "$G/select_$part.png"
    i=$((i + 1))
done
rm -f "$OUT/select.png"
INTER="$(fc-match -f '%{file}' 'Inter Variable')"
MONO="$(fc-match -f '%{file}' 'JetBrains Mono')"
grub-mkfont -s 16 -n "NEXUS Sans Regular 16" -o "$G/sans-16.pf2" "$INTER"
grub-mkfont -s 24 -n "NEXUS Sans Regular 24" -o "$G/sans-24.pf2" "$INTER"
grub-mkfont -s 14 -n "NEXUS Mono Regular 14" -o "$G/mono-14.pf2" "$MONO"

# ---- Plymouth -----------------------------------------------------------------
rsvg-convert -w 160 -h 160 "$SRC/logos/nexus-symbol.svg" -o "$P/logo.png"
# Progress track and bar.
convert -size 320x4 xc:none -fill "rgba(148,163,184,0.25)" -draw "roundrectangle 0,0 319,3 2,2" "$P/track.png"
convert -size 320x4 xc:none -fill "$ACCENT" -draw "roundrectangle 0,0 319,3 2,2" "$P/bar.png"
# Passphrase prompt: entry box, bullet, lock.
convert -size 360x44 xc:none -fill "$SURFACE" -stroke "rgba(34,211,238,0.7)" -strokewidth 1 \
    -draw "roundrectangle 0.5,0.5 359,43 10,10" "$P/entry.png"
convert -size 12x12 xc:none -fill "#E5E7EB" -draw "circle 6,6 6,1.5" "$P/bullet.png"
echo "generated: $G $P"
