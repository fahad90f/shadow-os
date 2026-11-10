#!/bin/bash
# Stage 2-4 of the image build. Invoked by build.sh inside a private mount
# namespace (unshare --mount), so the overlay mount below disappears when this
# script exits, successfully or not.
#
# Inputs (environment, exported by build.sh):
#   ARCH VARIANT COMP IMG BASE STAGING REPO_DIR NEXUS_PKGS ISO_NAME + NEXUS_*

. "$(dirname "${BASH_SOURCE[0]}")/../lib/common.sh"

[ -n "${BASE:-}" ] && [ -n "${IMG:-}" ] && [ -n "${STAGING:-}" ] || die "stage-rootfs.sh must be started by build.sh"

MERGED="$IMG/merged"
mount -t overlay nexus-rootfs \
    -o "lowerdir=$BASE,upperdir=$IMG/upper,workdir=$IMG/ovl-work" "$MERGED"
rm -f "$MERGED/.nexus-base-complete"

# Environment handed to every command run inside the image.
IMG_ENV=(
    "NEXUS_NAME=$NEXUS_NAME" "NEXUS_ID=$NEXUS_ID" "NEXUS_VERSION=$NEXUS_VERSION"
    "NEXUS_CODENAME=$NEXUS_CODENAME" "NEXUS_CHANNEL=$NEXUS_CHANNEL"
    "NEXUS_DESKTOP=$NEXUS_DESKTOP" "NEXUS_VENDOR=$NEXUS_VENDOR"
    "NEXUS_DEBIAN_SUITE=$NEXUS_DEBIAN_SUITE" "NEXUS_DEBIAN_VERSION=$NEXUS_DEBIAN_VERSION"
    "NEXUS_HOME_URL=$NEXUS_HOME_URL" "NEXUS_BUG_URL=$NEXUS_BUG_URL"
    "NEXUS_REPO_URL=$NEXUS_REPO_URL" "NEXUS_ARCH=$ARCH" "NEXUS_VARIANT=$VARIANT"
)
run() { in_chroot "$MERGED" "$@"; }

# Never start daemons while configuring the image.
printf '#!/bin/sh\nexit 101\n' > "$MERGED/usr/sbin/policy-rc.d"
chmod 755 "$MERGED/usr/sbin/policy-rc.d"

# All package operations in this stage use the build-time mirror that the base
# layer was bootstrapped from; the shipped APT configuration is written by the
# 80-apt-sources hook afterwards.
[ -s "$MERGED/etc/apt/sources.list" ] || die "base layer has no build-time sources.list"

# ---- NEXUS packages from the locally built, signed repository --------------
if [ -z "${NEXUS_PKGS:-}" ]; then
    run -- apt-get -q update
else
    log "installing NEXUS packages: $NEXUS_PKGS"
    install -D -m 644 "$REPO_DIR/nexus-archive-keyring.gpg" "$MERGED/etc/apt/keyrings/nexus-image-build.gpg"
    cat > "$MERGED/etc/apt/sources.list.d/nexus-image-build.list" <<EOF
deb [signed-by=/etc/apt/keyrings/nexus-image-build.gpg] file:///run/nexus-repo $NEXUS_CODENAME core security community
EOF
    run --bind "$REPO_DIR:/run/nexus-repo:ro" -- apt-get -q update
    # shellcheck disable=SC2086
    run --bind "$REPO_DIR:/run/nexus-repo:ro" -- apt-get -q -y --no-install-recommends \
        -o Dpkg::Options::=--force-confnew install $NEXUS_PKGS
    rm -f "$MERGED/etc/apt/sources.list.d/nexus-image-build.list" \
          "$MERGED/etc/apt/keyrings/nexus-image-build.gpg"
fi

# ---- on-media package pool (bootloader front-ends for the installer) --------
POOL_LIST="$NEXUS_ROOT/build/iso/pool.list"
if [ -f "$POOL_LIST" ]; then
    mapfile -t POOL_PKGS < <(awk -v arch="$ARCH" '
        { sub(/#.*/, "") } NF == 0 { next }
        { pkg = $1
          if (match($0, /\[[^]]*\]/)) { spec = substr($0, RSTART+1, RLENGTH-2); n = split(spec, a, /[ \t]+/); ok = 0
              for (i = 1; i <= n; i++) if (a[i] == arch) ok = 1; if (!ok) next }
          print pkg }' "$POOL_LIST")
    if [ "${#POOL_PKGS[@]}" -gt 0 ]; then
        log "downloading installer pool: ${POOL_PKGS[*]}"
        mkdir -p "$STAGING/pool"
        run --bind "$STAGING/pool:/run/nexus-pool" -- /bin/sh -ec \
            'cd /run/nexus-pool && apt-get -q download "$@"' sh "${POOL_PKGS[@]}"
        (cd "$STAGING/pool" && sha256sum -- *.deb > SHA256SUMS)
    fi
fi

# ---- static rootfs overlay ---------------------------------------------------
if [ -d "$NEXUS_ROOT/distro/base/rootfs" ]; then
    log "applying rootfs overlay"
    (cd "$NEXUS_ROOT/distro/base/rootfs" && find . -mindepth 1 ! -name .gitkeep -print0 \
        | cpio --null --quiet -pdm --owner=0:0 "$MERGED")
fi

# ---- configuration hooks -----------------------------------------------------
for hook in "$NEXUS_ROOT"/distro/base/hooks/[0-9]*.sh; do
    [ -f "$hook" ] || continue
    log "hook $(basename "$hook")"
    run --bind "$NEXUS_ROOT/distro/base/hooks:/run/nexus-hooks:ro" -- \
        env "${IMG_ENV[@]}" /bin/bash -eu "/run/nexus-hooks/$(basename "$hook")"
done

# ---- package manifest --------------------------------------------------------
mkdir -p "$NEXUS_OUT/iso"
run -- dpkg-query -W -f='${binary:Package}\t${Version}\t${Architecture}\n' \
    | sort > "$NEXUS_OUT/iso/$ISO_NAME.manifest"
log "image contains $(wc -l < "$NEXUS_OUT/iso/$ISO_NAME.manifest") packages"

# ---- boot loader binaries (taken from the image itself, so they always match
#      the GRUB version that the installed system will run) ---------------------
BOOTSTAGE="$STAGING/.bootfiles"
mkdir -p "$BOOTSTAGE/efi" "$STAGING/boot/grub/fonts"
EFI="$(arch_info "$ARCH" efi)"
GRUB_EFI_PLATFORM="$(arch_info "$ARCH" grub)"
cp "$MERGED/usr/lib/shim/shim$EFI.efi.signed" "$BOOTSTAGE/efi/boot$EFI.efi"
cp "$MERGED/usr/lib/shim/mm$EFI.efi.signed"   "$BOOTSTAGE/efi/mm$EFI.efi"
cp "$MERGED/usr/lib/grub/$GRUB_EFI_PLATFORM-signed/gcd$EFI.efi.signed" "$BOOTSTAGE/efi/grub$EFI.efi"
cp "$MERGED/usr/share/grub/unicode.pf2" "$STAGING/boot/grub/fonts/unicode.pf2"

if [ "$ARCH" = "amd64" ]; then
    # Legacy BIOS: El Torito core image + module directory + hybrid MBR.
    run -- grub-mkimage -O i386-pc-eltorito -d /usr/lib/grub/i386-pc \
        -p /boot/grub -o /tmp/eltorito.img biosdisk iso9660
    mkdir -p "$STAGING/boot/grub/i386-pc"
    mv "$MERGED/tmp/eltorito.img" "$STAGING/boot/grub/i386-pc/eltorito.img"
    cp "$MERGED"/usr/lib/grub/i386-pc/*.mod "$MERGED"/usr/lib/grub/i386-pc/*.lst \
        "$STAGING/boot/grub/i386-pc/"
    cp "$MERGED/usr/lib/grub/i386-pc/boot_hybrid.img" "$BOOTSTAGE/boot_hybrid.img"
fi

# ---- final cleanup (always last) --------------------------------------------
log "cleaning image"
run -- apt-get -q clean
rm -rf "$MERGED"/var/lib/apt/lists/* "$MERGED"/var/cache/apt/*.bin \
       "$MERGED"/var/log/*.log "$MERGED"/var/log/apt/* "$MERGED"/var/log/dpkg.log* \
       "$MERGED"/var/cache/debconf/*-old "$MERGED"/var/lib/dpkg/*-old \
       "$MERGED"/tmp/* "$MERGED"/var/tmp/* "$MERGED"/root/.bash_history \
       "$MERGED"/etc/ssh/ssh_host_*
rm -f "$MERGED/usr/sbin/policy-rc.d"
# A unique machine-id is generated on first boot of every live session/install.
: > "$MERGED/etc/machine-id"
rm -f "$MERGED/var/lib/dbus/machine-id"

# Anything that still references the build host must not ship.
if grep -rIl -e "$NEXUS_BUILD_MIRROR" "$MERGED/etc/apt" 2>/dev/null | grep -q .; then
    die "build-time mirror URL leaked into the image's APT configuration"
fi

# ---- kernel + initramfs for the boot loader ----------------------------------
KERNEL="$(find "$MERGED/boot" -maxdepth 1 -name 'vmlinuz-*' | sort -V | tail -n1)"
[ -n "$KERNEL" ] || die "no kernel found in image /boot"
KVER="${KERNEL##*/vmlinuz-}"
[ -f "$MERGED/boot/initrd.img-$KVER" ] || die "no initramfs for kernel $KVER"
cp "$KERNEL" "$STAGING/live/vmlinuz"
cp "$MERGED/boot/initrd.img-$KVER" "$STAGING/live/initrd.img"
echo "$KVER" > "$STAGING/live/kernel-version"
log "kernel $KVER"

# ---- reproducible timestamps, then squashfs ---------------------------------
find "$MERGED" -xdev -newermt "@$SOURCE_DATE_EPOCH" -print0 \
    | xargs -0r touch --no-dereference --date="@$SOURCE_DATE_EPOCH"

# Tell systemd the image is up to date with its /usr, otherwise every boot of
# the read-only live system re-runs the "first boot after update" jobs
# (ldconfig, hwdb, journal catalog, sysusers) and wastes tens of seconds.
usr_ns="$(stat -c %.9Y "$MERGED/usr" | tr -d .)"
for d in etc var; do
    printf '# Written by the NEXUS OS image build (equivalent to systemd-update-done).\nTIMESTAMP_NSEC=%s\n' \
        "$usr_ns" > "$MERGED/$d/.updated"
    touch -r "$MERGED/usr" "$MERGED/$d/.updated"
done

case "$COMP" in
    zstd) COMP_OPTS=(-comp zstd -Xcompression-level 19 -b 1M) ;;
    xz)   COMP_OPTS=(-comp xz -Xbcj "$([ "$ARCH" = amd64 ] && echo x86 || echo arm)" -b 1M -Xdict-size 100%) ;;
    *) die "unknown compressor '$COMP'" ;;
esac
log "creating squashfs ($COMP)"
mksquashfs "$MERGED" "$STAGING/live/filesystem.squashfs" \
    -noappend -no-progress -processors "$NEXUS_JOBS" "${COMP_OPTS[@]}" \
    -wildcards -e 'boot/initrd.img-*' -e 'proc/*' -e 'sys/*' -e 'run/*' -e 'tmp/*' -e 'dev/*' \
    >/dev/null
du -sb "$MERGED" 2>/dev/null | cut -f1 > "$STAGING/live/filesystem.size"
log "squashfs: $(du -h "$STAGING/live/filesystem.squashfs" | cut -f1) (from $(numfmt --to=iec "$(cat "$STAGING/live/filesystem.size")") installed)"
