#!/bin/bash
# Build the NEXUS kernel packages.
#
#   kernel/build.sh [--arch amd64|arm64] [--configure-only] [--jobs N]
#
# The NEXUS kernel is Debian's kernel source for the suite's longterm series
# (package linux-source-<series>), configured with Debian's configuration for
# the architecture plus the NEXUS fragments in kernel/config, kernel/x86_64 and
# kernel/aarch64, and with the patches listed in kernel/patches/series applied.
# kernel.org is not contacted: the source is the one Debian's security team
# maintains for the stable release.
#
# Steps:
#   1. dedicated build chroot (amd64 host; arm64 is cross-compiled)
#   2. unpack linux-source, apply the patch series
#   3. Debian config + NEXUS fragments → .config; verify config/required.list
#   4. make bindeb-pkg  → linux-image / linux-headers packages
#   5. meta package linux-image-nexus-<arch>
#
# Output: $NEXUS_OUT/packages/linux-*.deb and $NEXUS_OUT/kernel/config-<arch>
#
# Environment:
#   NEXUS_KERNEL_DEBUGINFO=btf|none   (default: btf)
#       btf   keep Debian's debug-info settings; BTF type information is built
#             in (needed by CO-RE eBPF tools). Needs ~25 GB of disk and more time.
#       none  no debug information and no BTF: ~6 GB and much faster. For
#             machines that cannot afford the full build.
#   NEXUS_SB_KEY / NEXUS_SB_CERT      PEM key and certificate; when both are
#       set, the kernel image is signed for UEFI Secure Boot with sbsign.
#       Without them the image is unsigned and boots under Secure Boot only if
#       the user enrols its hash or key (docs/security/secure-boot.md).

. "$(dirname "${BASH_SOURCE[0]}")/../build/lib/common.sh"

ARCH="$(host_arch)"
CONFIGURE_ONLY=0
JOBS="$NEXUS_JOBS"
while [ $# -gt 0 ]; do
    case "$1" in
        --arch) ARCH="$2"; shift 2 ;;
        --configure-only) CONFIGURE_ONLY=1; shift ;;
        --jobs) JOBS="$2"; shift 2 ;;
        -h|--help) sed -n '2,33p' "$0"; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done
need_root
need_cmd mmdebstrap
[ "$(host_arch)" = "amd64" ] || die "kernel builds run on an amd64 host (arm64 is cross-compiled)"
KARCH="$(arch_info "$ARCH" kernel)"
: "${NEXUS_KERNEL_DEBUGINFO:=btf}"
case "$NEXUS_KERNEL_DEBUGINFO" in btf|none) ;; *) die "NEXUS_KERNEL_DEBUGINFO must be 'btf' or 'none'" ;; esac

SERIES="$NEXUS_KERNEL_SERIES"
CHROOT="$NEXUS_WORK/chroot-kernel"
FLAVOUR="nexus-$ARCH"
mkdir -p "$NEXUS_OUT/packages" "$NEXUS_OUT/kernel"

# ---- 1. build chroot ------------------------------------------------------------
DEPS="build-essential,bc,bison,flex,libssl-dev,libelf-dev,libdw-dev,dwarves,cpio,kmod,rsync,python3,debhelper,zstd,xz-utils,quilt,ca-certificates,sbsigntool,linux-source-$SERIES,linux-config-$SERIES"
[ "$ARCH" = "arm64" ] && DEPS="$DEPS,gcc-aarch64-linux-gnu,libc6-dev-arm64-cross"
if [ ! -e "$CHROOT/.nexus-bootstrap-complete" ]; then
    [ -d "$CHROOT" ] && safe_rmtree "$CHROOT"
    log "bootstrapping kernel build chroot"
    mmdebstrap --variant=buildd --architectures=amd64 --keyring="$(debian_keyring)" \
        --include="$DEPS" --aptopt='APT::Install-Recommends "false"' --aptopt='Acquire::Retries "5"' \
        --dpkgopt='force-unsafe-io' \
        "$NEXUS_DEBIAN_SUITE" "$CHROOT" - < <(build_sources)
    date -u +%Y-%m-%dT%H:%M:%SZ > "$CHROOT/.nexus-bootstrap-complete"
else
    in_chroot "$CHROOT" -- apt-get -q update >/dev/null
    # shellcheck disable=SC2086
    in_chroot "$CHROOT" -- apt-get -q -y --no-install-recommends install ${DEPS//,/ } >/dev/null
fi

# ---- 2-5. inside the chroot -------------------------------------------------------
SB_BINDS=()
if [ -n "${NEXUS_SB_KEY:-}" ] && [ -n "${NEXUS_SB_CERT:-}" ]; then
    [ -r "$NEXUS_SB_KEY" ] && [ -r "$NEXUS_SB_CERT" ] || die "NEXUS_SB_KEY/NEXUS_SB_CERT are not readable"
    SB_BINDS=(--bind "$NEXUS_SB_KEY:/run/nexus-sb/key.pem:ro" --bind "$NEXUS_SB_CERT:/run/nexus-sb/cert.pem:ro")
fi

in_chroot "$CHROOT" --bind "$NEXUS_ROOT/kernel:/nexus-kernel:ro" "${SB_BINDS[@]}" -- /bin/bash -euo pipefail -c '
ARCH="$1"; KARCH="$2"; SERIES="$3"; FLAVOUR="$4"; JOBS="$5"; DEBUGINFO="$6"; CONFIGURE_ONLY="$7"; NEXUS_VERSION="$8"
say() { printf "\033[1;36m[nexus-kernel]\033[0m %s\n" "$*" >&2; }

CROSS=""
[ "$ARCH" = "arm64" ] && CROSS="aarch64-linux-gnu-"
MAKE=(make ARCH="$KARCH" ${CROSS:+CROSS_COMPILE=$CROSS})

BUILD="/build/kernel-$ARCH"
TARBALL="/usr/src/linux-source-$SERIES.tar.xz"
[ -f "$TARBALL" ] || { echo "missing $TARBALL" >&2; exit 1; }
SRC="$BUILD/linux-source-$SERIES"
if [ ! -f "$SRC/Makefile" ]; then
    say "unpacking $TARBALL"
    rm -rf "$BUILD"; mkdir -p "$BUILD"
    tar -C "$BUILD" -xf "$TARBALL"
    # Patch series (quilt format): applied once, right after unpacking.
    mapfile -t PATCHES < <(sed -e "s/#.*//" -e "/^[[:space:]]*\$/d" /nexus-kernel/patches/series)
    for p in "${PATCHES[@]}"; do
        say "applying patch $p"
        patch -d "$SRC" -p1 --no-backup-if-mismatch < "/nexus-kernel/patches/$p"
    done
    printf "%s\n" "${PATCHES[@]:-}" > "$BUILD/applied-patches"
fi
cd "$SRC"
KVER="$("${MAKE[@]}" -s kernelversion)"
say "Linux $KVER for $ARCH (flavour $FLAVOUR)"

# ---- configuration ----------------------------------------------------------
case "$ARCH" in
    amd64) DEBCFG="/usr/src/linux-config-$SERIES/config.amd64_none_amd64.xz"; ARCHFRAG=/nexus-kernel/x86_64/nexus.config ;;
    arm64) DEBCFG="/usr/src/linux-config-$SERIES/config.arm64_none_arm64.xz"; ARCHFRAG=/nexus-kernel/aarch64/nexus.config ;;
esac
[ -f "$DEBCFG" ] || { echo "missing Debian configuration $DEBCFG" >&2; ls /usr/src/linux-config-$SERIES >&2; exit 1; }
xzcat "$DEBCFG" > .config
cp .config "$BUILD/config.debian"

# Keys that only exist on Debian build machines.
scripts/config --set-str SYSTEM_TRUSTED_KEYS "" --set-str SYSTEM_REVOCATION_KEYS "" \
               --set-str MODULE_SIG_KEY "certs/signing_key.pem"
scripts/config --set-str LOCALVERSION "" --disable LOCALVERSION_AUTO
if [ "$DEBUGINFO" = "none" ]; then
    scripts/config --disable DEBUG_INFO_BTF --disable DEBUG_INFO_BTF_MODULES \
                   --disable DEBUG_INFO_DWARF5 --disable DEBUG_INFO_DWARF4 \
                   --disable DEBUG_INFO_DWARF_TOOLCHAIN_DEFAULT --enable DEBUG_INFO_NONE
fi

KCONFIG_CONFIG=.config ARCH="$KARCH" ${CROSS:+CROSS_COMPILE=$CROSS} \
    scripts/kconfig/merge_config.sh -m .config /nexus-kernel/config/nexus-common.config "$ARCHFRAG" > "$BUILD/merge.log" 2>&1
"${MAKE[@]}" olddefconfig > "$BUILD/olddefconfig.log" 2>&1

# ---- verification -----------------------------------------------------------
fail=0
while IFS= read -r line; do
    line="${line%%#*}"
    [ -z "${line// }" ] && continue
    if [[ "$line" =~ \[([a-z0-9 ]+)\][[:space:]]*$ ]]; then
        archs="${BASH_REMATCH[1]}"
        line="${line%[*}"
        [[ " $archs " == *" $ARCH "* ]] || continue
    fi
    line="$(echo "$line" | sed -e "s/[[:space:]]*\$//")"
    if [[ "$line" == *" is not set" ]]; then
        opt="${line% is not set}"
        if grep -q "^$opt=" .config; then
            echo "  REQUIRED: $line   — but found: $(grep "^$opt=" .config)" >&2; fail=1
        fi
    else
        if ! grep -qxF "$line" .config; then
            opt="${line%%=*}"
            echo "  REQUIRED: $line   — but found: $(grep -E "^(# )?$opt[= ]" .config || echo "(absent)")" >&2; fail=1
        fi
    fi
done < /nexus-kernel/config/required.list
if [ "$fail" -ne 0 ]; then
    echo "kernel configuration does not satisfy kernel/config/required.list" >&2
    exit 1
fi
say "configuration verified against required.list"
cp .config "$BUILD/config.nexus"
# What NEXUS changes relative to Debian, for the documentation and for review.
scripts/diffconfig "$BUILD/config.debian" .config > "$BUILD/config.diff" || true
say "$(grep -c . "$BUILD/config.diff") options differ from the Debian configuration (see config.diff)"

[ "$CONFIGURE_ONLY" = "1" ] && exit 0

# ---- build --------------------------------------------------------------------
PKGVER="${KVER}-1+nexus${NEXUS_VERSION//./}"
say "building packages (version $PKGVER, $JOBS jobs, debuginfo=$DEBUGINFO)"
rm -f "$BUILD"/*.deb "$BUILD"/*.buildinfo "$BUILD"/*.changes
"${MAKE[@]}" -j"$JOBS" bindeb-pkg LOCALVERSION="-$FLAVOUR" KDEB_PKGVERSION="$PKGVER" \
    KBUILD_BUILD_USER=nexus KBUILD_BUILD_HOST=nexus-build KDEB_COMPRESS=xz > "$BUILD/build.log" 2>&1 \
    || { tail -n 40 "$BUILD/build.log" >&2; exit 1; }
RELEASE="$KVER-$FLAVOUR"
ls "$BUILD"/linux-image-"$RELEASE"_*.deb >/dev/null

# ---- optional Secure Boot signature ---------------------------------------------
IMAGE_DEB="$(ls "$BUILD"/linux-image-"$RELEASE"_*.deb | grep -v dbg | head -n1)"
if [ -r /run/nexus-sb/key.pem ] && [ -r /run/nexus-sb/cert.pem ]; then
    say "signing the kernel image for Secure Boot"
    tmp="$(mktemp -d)"
    dpkg-deb -R "$IMAGE_DEB" "$tmp"
    sbsign --key /run/nexus-sb/key.pem --cert /run/nexus-sb/cert.pem \
        --output "$tmp/boot/vmlinuz-$RELEASE.signed" "$tmp/boot/vmlinuz-$RELEASE"
    mv "$tmp/boot/vmlinuz-$RELEASE.signed" "$tmp/boot/vmlinuz-$RELEASE"
    (cd "$tmp" && find . -type f ! -path "./DEBIAN/*" -printf "%P\0" | xargs -0 md5sum > DEBIAN/md5sums)
    dpkg-deb --root-owner-group -Zxz -b "$tmp" "$IMAGE_DEB" >/dev/null
    rm -rf "$tmp"
    echo signed > "$BUILD/secure-boot-state"
else
    echo unsigned > "$BUILD/secure-boot-state"
fi

# ---- meta package -----------------------------------------------------------------
meta="$(mktemp -d)"
mkdir -p "$meta/DEBIAN" "$meta/usr/share/doc/linux-image-nexus-$ARCH"
cat > "$meta/DEBIAN/control" <<CTRL
Package: linux-image-nexus-$ARCH
Version: $PKGVER
Architecture: $ARCH
Maintainer: NEXUS OS Developers <nexus-os@users.noreply.github.com>
Section: kernel
Priority: optional
Depends: linux-image-$RELEASE (= $PKGVER)
Provides: linux-image-nexus
Description: NEXUS kernel for $ARCH (meta package)
 Depends on the current NEXUS kernel image, so that installing this package
 keeps the system on the newest NEXUS kernel across updates.
 .
 The NEXUS kernel is Debian s Linux $SERIES source built with Debian s
 configuration plus the NEXUS hardening and desktop options.
CTRL
sed -i "s/Debian s/Debian'"'"'s/g" "$meta/DEBIAN/control"
printf "The NEXUS kernel is built from Debian linux-source-%s. See /usr/share/doc/linux-image-%s/.\n" "$SERIES" "$RELEASE" \
    > "$meta/usr/share/doc/linux-image-nexus-$ARCH/README"
dpkg-deb --root-owner-group -Zxz -b "$meta" "$BUILD/linux-image-nexus-${ARCH}_${PKGVER}_${ARCH}.deb" >/dev/null
rm -rf "$meta"
say "done: $(ls "$BUILD"/*.deb | wc -l) packages, Secure Boot state: $(cat "$BUILD/secure-boot-state")"
' kernel-build "$ARCH" "$KARCH" "$SERIES" "$FLAVOUR" "$JOBS" "$NEXUS_KERNEL_DEBUGINFO" "$CONFIGURE_ONLY" "$NEXUS_VERSION"

BUILD="$CHROOT/build/kernel-$ARCH"
cp "$BUILD/config.nexus" "$NEXUS_OUT/kernel/config-$ARCH"
cp "$BUILD/config.diff" "$NEXUS_OUT/kernel/config-$ARCH.diff-from-debian"
if [ "$CONFIGURE_ONLY" -eq 1 ]; then
    log "configuration written to $NEXUS_OUT/kernel/config-$ARCH (no build requested)"
    exit 0
fi
for deb in "$BUILD"/*.deb; do
    case "$(basename "$deb")" in
        linux-image-*-dbg_*|linux-libc-dev_*) continue ;;      # not shipped
    esac
    cp "$deb" "$NEXUS_OUT/packages/"
done
log "kernel packages → $NEXUS_OUT/packages:"
ls -1 "$NEXUS_OUT"/packages/linux-*"${ARCH}".deb | sed 's|.*/|  |' >&2
