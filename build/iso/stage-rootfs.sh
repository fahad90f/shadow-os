#!/bin/bash
# Stage 2-4 of the image build. Invoked by build.sh inside a private mount
# namespace (unshare --mount), so the overlay mount below disappears when this
# script exits, successfully or not.
#
# Inputs (environment, exported by build.sh):
#   ARCH VARIANT COMP IMG BASE STAGING REPO_DIR SHADOW_PKGS ISO_NAME + SHADOW_*

. "$(dirname "${BASH_SOURCE[0]}")/../lib/common.sh"

[ -n "${BASE:-}" ] && [ -n "${IMG:-}" ] && [ -n "${STAGING:-}" ] || die "stage-rootfs.sh must be started by build.sh"

MERGED="$IMG/merged"
mount -t overlay shadow-rootfs \
    -o "lowerdir=$BASE,upperdir=$IMG/upper,workdir=$IMG/ovl-work" "$MERGED"
rm -f "$MERGED/.shadow-base-complete"

# Environment handed to every command run inside the image.
IMG_ENV=(
    "SHADOW_NAME=$SHADOW_NAME" "SHADOW_ID=$SHADOW_ID" "SHADOW_VERSION=$SHADOW_VERSION"
    "SHADOW_CODENAME=$SHADOW_CODENAME" "SHADOW_CHANNEL=$SHADOW_CHANNEL"
    "SHADOW_DESKTOP=$SHADOW_DESKTOP" "SHADOW_VENDOR=$SHADOW_VENDOR"
    "SHADOW_DEBIAN_SUITE=$SHADOW_DEBIAN_SUITE" "SHADOW_DEBIAN_VERSION=$SHADOW_DEBIAN_VERSION"
    "SHADOW_HOME_URL=$SHADOW_HOME_URL" "SHADOW_BUG_URL=$SHADOW_BUG_URL"
    "SHADOW_REPO_URL=$SHADOW_REPO_URL" "SHADOW_ARCH=$ARCH" "SHADOW_VARIANT=$VARIANT"
)
run() { in_chroot "$MERGED" "$@"; }

# Never start daemons while configuring the image.
printf '#!/bin/sh\nexit 101\n' > "$MERGED/usr/sbin/policy-rc.d"
chmod 755 "$MERGED/usr/sbin/policy-rc.d"

# All package operations in this stage use the build-time mirror that the base
# layer was bootstrapped from; the shipped APT configuration is written by the
# 80-apt-sources hook afterwards.
[ -s "$MERGED/etc/apt/sources.list" ] || die "base layer has no build-time sources.list"

# ---- SHADOW packages from the locally built, signed repository --------------
if [ -z "${SHADOW_PKGS:-}" ]; then
    run -- apt-get -q update
else
    log "installing SHADOW packages: $SHADOW_PKGS"
    install -D -m 644 "$REPO_DIR/shadow-archive-keyring.gpg" "$MERGED/etc/apt/keyrings/shadow-image-build.gpg"
    cat > "$MERGED/etc/apt/sources.list.d/shadow-image-build.list" <<EOF
deb [signed-by=/etc/apt/keyrings/shadow-image-build.gpg] file:///run/shadow-repo $SHADOW_CODENAME core security community
EOF
    run --bind "$REPO_DIR:/run/shadow-repo:ro" -- apt-get -q update
    # Install resiliently. The full arsenal configures hundreds of packages in
    # one transaction; a maintainer script that creates a system user can fail
    # transiently ("useradd: cannot open /etc/shadow") under that load, leaving
    # a few packages unconfigured. A clean "dpkg --configure -a" pass then
    # completes them, so we retry and only give up (failing the build) if a
    # package is still broken afterwards. The open-file limit is raised for the
    # big transaction and inode/space usage is logged for diagnosis.
    # shellcheck disable=SC2086
    run --bind "$REPO_DIR:/run/shadow-repo:ro" -- bash -eu -c '
        ulimit -n 1048576 2>/dev/null || ulimit -n 65536 2>/dev/null || true
        echo "space/inodes before SHADOW install:"; df -h / || true; df -i / || true
        opts="-q -y --no-install-recommends -o Dpkg::Options::=--force-confnew -o Dpkg::Use-Pty=0"
        ok=
        for a in 1 2 3; do
            echo "=== SHADOW package install: attempt $a ==="
            if apt-get $opts install "$@"; then ok=1; break; fi
            echo "=== attempt $a left packages unconfigured; recovering ==="
            dpkg --configure -a || true
            apt-get $opts --fix-broken install || true
        done
        if [ -z "${ok:-}" ]; then
            echo "=== SHADOW install FAILED after retries ==="
            echo "open-file limit now: $(ulimit -n)"
            echo "space/inodes now:"; df -h / || true; df -i / || true
            echo "packages still in a bad state:"; dpkg --audit || true
            exit 1
        fi
    ' shadow-install $SHADOW_PKGS
    rm -f "$MERGED/etc/apt/sources.list.d/shadow-image-build.list" \
          "$MERGED/etc/apt/keyrings/shadow-image-build.gpg"

    # The SHADOW packages of this release also ship on the system as a signed
    # local APT repository. It is what makes the shadow-arsenal-* profile
    # metapackages installable (and SHADOW packages re-installable) on a
    # machine that has no SHADOW network repository configured. Debug-symbol
    # packages are left out.
    if [ -x "$MERGED/usr/bin/horizon-shell" ]; then
        log "adding the local SHADOW package repository"
        LOCAL_REPO="$MERGED/usr/share/shadow/repository"
        mkdir -p "$LOCAL_REPO"
        rsync -a --exclude '*-dbgsym_*' "$REPO_DIR/dists" "$REPO_DIR/pool" "$LOCAL_REPO/"
        {
            echo "# Shadow OS $SHADOW_VERSION packages that came with this installation"
            echo "# (/usr/share/shadow/repository), authenticated with the SHADOW archive key."
            echo "Types: deb"
            echo "URIs: file:///usr/share/shadow/repository"
            echo "Suites: $SHADOW_CODENAME"
            echo "Components: core security community"
            echo "Signed-By: /usr/share/keyrings/shadow-archive-keyring.gpg"
        } > "$MERGED/etc/apt/sources.list.d/shadow-local.sources"
    fi
fi

# ---- static rootfs overlay ---------------------------------------------------
if [ -d "$SHADOW_ROOT/distro/base/rootfs" ]; then
    log "applying rootfs overlay"
    (cd "$SHADOW_ROOT/distro/base/rootfs" && find . -mindepth 1 ! -name .gitkeep -print0 \
        | cpio --null --quiet -pdm --owner=0:0 "$MERGED")
fi

# ---- configuration hooks -----------------------------------------------------
for hook in "$SHADOW_ROOT"/distro/base/hooks/[0-9]*.sh; do
    [ -f "$hook" ] || continue
    log "hook $(basename "$hook")"
    run --bind "$SHADOW_ROOT/distro/base/hooks:/run/shadow-hooks:ro" -- \
        env "${IMG_ENV[@]}" /bin/bash -eu "/run/shadow-hooks/$(basename "$hook")"
done

# ---- package manifest --------------------------------------------------------
mkdir -p "$SHADOW_OUT/iso"
run -- dpkg-query -W -f='${binary:Package}\t${Version}\t${Architecture}\n' \
    | sort > "$SHADOW_OUT/iso/$ISO_NAME.manifest"
log "image contains $(wc -l < "$SHADOW_OUT/iso/$ISO_NAME.manifest") packages"

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
if grep -rIl -e "$SHADOW_BUILD_MIRROR" "$MERGED/etc/apt" 2>/dev/null | grep -q .; then
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
    printf '# Written by the Shadow OS image build (equivalent to systemd-update-done).\nTIMESTAMP_NSEC=%s\n' \
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
    -noappend -no-progress -processors "$SHADOW_JOBS" "${COMP_OPTS[@]}" \
    -wildcards -e 'boot/initrd.img-*' -e 'proc/*' -e 'sys/*' -e 'run/*' -e 'tmp/*' -e 'dev/*' \
    >/dev/null
du -sb "$MERGED" 2>/dev/null | cut -f1 > "$STAGING/live/filesystem.size"
log "squashfs: $(du -h "$STAGING/live/filesystem.squashfs" | cut -f1) (from $(numfmt --to=iec "$(cat "$STAGING/live/filesystem.size")") installed)"
