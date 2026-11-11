#!/bin/bash
# Stage 5 of the image build: turn the staging tree into a bootable hybrid ISO.
#
# Layout (identical on both architectures unless noted):
#   /.disk/info                 media identification; Debian's signed "gcd"
#                               GRUB image locates the boot media by this file
#   /EFI/boot/boot<arch>.efi    shim (Microsoft-signed)
#   /EFI/boot/grub<arch>.efi    GRUB (Debian-signed, "gcd" removable variant)
#   /EFI/boot/mm<arch>.efi      MOK manager
#   /boot/grub/grub.cfg         boot menu (shared by UEFI and BIOS)
#   /boot/grub/i386-pc/         BIOS core image and modules         (amd64)
#   /live/{vmlinuz,initrd.img,filesystem.squashfs}
#
# The image carries a real GPT: partition 1 is the ISO9660 filesystem,
# partition 2 an appended EFI System Partition. Written raw to a USB stick it
# boots through UEFI (with or without Secure Boot) and, on amd64, through
# legacy BIOS via GRUB's hybrid MBR. Burned to optical media it boots through
# El Torito in both modes.

. "$(dirname "${BASH_SOURCE[0]}")/../lib/common.sh"

[ -n "${STAGING:-}" ] && [ -n "${ARCH:-}" ] || die "assemble.sh must be started by build.sh"

EFI="$(arch_info "$ARCH" efi)"
BOOTSTAGE="$STAGING/.bootfiles"
ISO_DIR="$NEXUS_OUT/iso"
ISO="$ISO_DIR/$ISO_NAME.iso"
BUILD_STAMP="$(date -u -d "@$SOURCE_DATE_EPOCH" +%Y%m%d)"
# ISO9660 volume id: max 32 chars, [A-Z0-9_]
VOLID="$(printf 'NEXUS_%s_%s' "$NEXUS_VERSION" "$ARCH" | tr 'a-z.-' 'A-Z__' | cut -c1-32)"

# ---- media identification ----------------------------------------------------
printf '%s %s "%s" - %s %s (%s)\n' "$NEXUS_NAME" "$NEXUS_VERSION" "$NEXUS_CODENAME" \
    "$VARIANT" "$ARCH" "$BUILD_STAMP" > "$STAGING/.disk/info"

# ---- boot menu ----------------------------------------------------------------
GRUB_SRC="$NEXUS_ROOT/system/boot/grub"
sed -e "s|@NEXUS_NAME@|$NEXUS_NAME|g" \
    -e "s|@NEXUS_VERSION@|$NEXUS_VERSION|g" \
    -e "s|@NEXUS_CODENAME@|$NEXUS_CODENAME|g" \
    -e "s|@ARCH@|$ARCH|g" \
    -e "s|@KERNEL_VERSION@|$(cat "$STAGING/live/kernel-version")|g" \
    "$GRUB_SRC/live-grub.cfg.in" > "$STAGING/boot/grub/grub.cfg"
if [ -f "$NEXUS_ROOT/assets/boot/grub/nexus/theme.txt" ]; then
    mkdir -p "$STAGING/boot/grub/themes/nexus"
    cp -r "$NEXUS_ROOT/assets/boot/grub/nexus/." "$STAGING/boot/grub/themes/nexus/"
fi

# ---- EFI System Partition image ----------------------------------------------
mkdir -p "$STAGING/EFI/boot"
cp "$BOOTSTAGE/efi/"*.efi "$STAGING/EFI/boot/"

ESP="$IMG/efi.img"
rm -f "$ESP"
esp_kb=$(( $(du -sk "$BOOTSTAGE/efi" | cut -f1) + 1024 ))
esp_kb=$(( (esp_kb + 1023) / 1024 * 1024 ))            # round up to whole MiB
[ "$esp_kb" -ge 4096 ] || esp_kb=4096
truncate -s "${esp_kb}K" "$ESP"
mformat -i "$ESP" -v NEXUS_ESP -N "$(printf '%08x' "$SOURCE_DATE_EPOCH" | cut -c1-8)" ::
mmd -i "$ESP" ::/EFI ::/EFI/boot
for f in "$BOOTSTAGE/efi/"*.efi; do
    mcopy -i "$ESP" "$f" "::/EFI/boot/$(basename "$f")"
done

# ---- xorriso -------------------------------------------------------------------
BIOS_OPTS=()
if [ "$ARCH" = "amd64" ]; then
    BIOS_OPTS=(
        --grub2-mbr "$BOOTSTAGE/boot_hybrid.img"
        --mbr-force-bootable
        -b /boot/grub/i386-pc/eltorito.img
        -no-emul-boot -boot-load-size 4 -boot-info-table --grub2-boot-info
        -eltorito-alt-boot
    )
fi

# The boot staging directory is build scaffolding, not media content.
BOOT_HYBRID_TMP="$IMG/bootfiles"
safe_rmtree "$BOOT_HYBRID_TMP"
mv "$BOOTSTAGE" "$BOOT_HYBRID_TMP"
[ "$ARCH" = "amd64" ] && BIOS_OPTS[1]="$BOOT_HYBRID_TMP/boot_hybrid.img"

log "writing $ISO"
rm -f "$ISO"
xorriso -as mkisofs \
    -iso-level 3 -rational-rock -joliet -joliet-long -full-iso9660-filenames \
    -volid "$VOLID" \
    -appid "$NEXUS_NAME $NEXUS_VERSION" \
    -publisher "$NEXUS_VENDOR" \
    -preparer "NEXUS OS build system" \
    --modification-date="$(date -u -d "@$SOURCE_DATE_EPOCH" +%Y%m%d%H%M%S00)" \
    -partition_offset 16 \
    -partition_cyl_align off \
    -append_partition 2 28732ac11ff8d211ba4b00a0c93ec93b "$ESP" \
    -appended_part_as_gpt \
    -iso_mbr_part_type a2a0d0ebe5b9334487c068b6b72699c7 \
    -c /boot.catalog \
    "${BIOS_OPTS[@]}" \
    -e '--interval:appended_partition_2:::' -no-emul-boot \
    -o "$ISO" \
    "$STAGING" 2> "$IMG/xorriso.log" || { tail -n 30 "$IMG/xorriso.log" >&2; die "xorriso failed"; }

(cd "$ISO_DIR" && sha256sum "$(basename "$ISO")" > "$(basename "$ISO").sha256")
log "ISO size: $(du -h "$ISO" | cut -f1)   sha256: $(cut -d' ' -f1 "$ISO.sha256")"
