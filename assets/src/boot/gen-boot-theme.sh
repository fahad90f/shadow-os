#!/bin/bash
# Regenerates the binary parts of the boot branding from the logo, the
# wallpaper and the UI fonts:
#   assets/boot/grub/shadow/      GRUB menu theme (background, fonts, selection box)
#   assets/boot/plymouth/shadow/  Plymouth splash images
# Needs ImageMagick, rsvg-convert, grub-mkfont and the Inter / JetBrains Mono
# fonts — all present in the build chroot:
#   scripts/dev.sh exec -- /src/assets/src/boot/gen-boot-theme.sh /tmp/boot-theme
# then copy /tmp/boot-theme/{grub,plymouth} over assets/boot/.
set -euo pipefail
SRC="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:?output directory}"
G="$OUT/grub/shadow"; P="$OUT/plymouth/shadow"
mkdir -p "$G" "$P"

BG="#05070D"; ACCENT="#22D3EE"; SURFACE="#0F172A"

# ---- GRUB ---------------------------------------------------------------------
# Dark, calm background: the wallpaper darkened, so menu text stays legible.
convert "$SRC/wallpapers/shadow-horizon.jpg" -resize 1280x720^ -gravity center -extent 1280x720 \
    -fill "$BG" -colorize 55% -depth 8 -strip "$G/background.png"
rsvg-convert -w 96 -h 96 "$SRC/logos/shadow-symbol.svg" -o "$G/logo.png"
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
grub-mkfont -s 16 -n "SHADOW Sans Regular 16" -o "$G/sans-16.pf2" "$INTER"
grub-mkfont -s 24 -n "SHADOW Sans Regular 24" -o "$G/sans-24.pf2" "$INTER"
grub-mkfont -s 14 -n "SHADOW Mono Regular 14" -o "$G/mono-14.pf2" "$MONO"

# ---- Plymouth -----------------------------------------------------------------
# Glowing logo at the centre.
rsvg-convert -w 150 -h 150 "$SRC/logos/shadow-symbol.svg" -o "$P/logo.png"
# Soft cyan halo that pulses behind the logo.
convert -size 460x460 radial-gradient:'rgba(34,211,238,0.42)'-'rgba(34,211,238,0)' "$P/glow.png"
# Spinner ring: a faint full circle with one bright rounded arc. Rotating the
# whole image spins the arc. Circumference 2*pi*104 ~= 653; the arc shows ~130.
rsvg-convert -o "$P/ring.png" /dev/stdin <<RING
<svg xmlns="http://www.w3.org/2000/svg" width="248" height="248" viewBox="0 0 248 248">
  <circle cx="124" cy="124" r="104" fill="none" stroke="rgba(148,163,184,0.16)" stroke-width="5"/>
  <circle cx="124" cy="124" r="104" fill="none" stroke="#22D3EE" stroke-width="5"
          stroke-linecap="round" stroke-dasharray="130 523" transform="rotate(-90 124 124)"/>
  <circle cx="124" cy="20" r="4.5" fill="#CFFAFE"/>
</svg>
RING
# A second, slower inner arc in the violet secondary accent for depth.
rsvg-convert -o "$P/ring2.png" /dev/stdin <<RING2
<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200" viewBox="0 0 200 200">
  <circle cx="100" cy="100" r="80" fill="none" stroke="#8B5CF6" stroke-width="3"
          stroke-linecap="round" stroke-dasharray="60 443" opacity="0.8" transform="rotate(40 100 100)"/>
</svg>
RING2
# Progress: a faint track, the cyan bar, and a blurred copy of the bar as glow.
convert -size 360x4 xc:none -fill "rgba(148,163,184,0.22)" -draw "roundrectangle 0,0 359,3 2,2" "$P/track.png"
convert -size 360x4 xc:none -fill "$ACCENT" -draw "roundrectangle 0,0 359,3 2,2" "$P/bar.png"
convert "$P/bar.png" -blur 0x3 "$P/barglow.png"
# Passphrase prompt: entry box, bullet.
convert -size 360x44 xc:none -fill "$SURFACE" -stroke "rgba(34,211,238,0.7)" -strokewidth 1 \
    -draw "roundrectangle 0.5,0.5 359,43 10,10" "$P/entry.png"
convert -size 12x12 xc:none -fill "#E5E7EB" -draw "circle 6,6 6,1.5" "$P/bullet.png"
echo "generated: $G $P"
