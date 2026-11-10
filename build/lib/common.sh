#!/bin/bash
# NEXUS OS build system — shared configuration and helpers.
# Sourced by every script under build/. Never executed directly.
#
# All knobs are environment-overridable so CI and developers can relocate
# work/output directories or point at a local mirror without editing files.

set -euo pipefail

NEXUS_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
export NEXUS_ROOT

# shellcheck source=../../distro/base/nexus.conf
. "$NEXUS_ROOT/distro/base/nexus.conf"

: "${NEXUS_WORK:=$NEXUS_ROOT/build/work}"
: "${NEXUS_OUT:=$NEXUS_ROOT/build/out}"
: "${NEXUS_CACHE:=$NEXUS_WORK/cache}"
: "${NEXUS_JOBS:=$(nproc)}"
export NEXUS_WORK NEXUS_OUT NEXUS_CACHE NEXUS_JOBS

# Build-time mirrors. Plain HTTP is deliberate: APT authenticates content with
# the signed Release file, and build hosts behind TLS-intercepting proxies do
# not have their private CA inside a pristine chroot. The *installed* system
# is configured with HTTPS mirrors (see distro/base/apt/).
: "${NEXUS_BUILD_MIRROR:=http://deb.debian.org/debian}"
: "${NEXUS_BUILD_SECURITY_MIRROR:=http://deb.debian.org/debian-security}"
export NEXUS_BUILD_MIRROR NEXUS_BUILD_SECURITY_MIRROR

log()  { printf '\033[1;36m[nexus]\033[0m %s\n' "$*" >&2; }
warn() { printf '\033[1;33m[nexus] WARNING:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[nexus] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

need_root() {
    [ "$(id -u)" -eq 0 ] || die "this step must run as root (it creates chroots and device nodes)"
}

need_cmd() {
    local c
    for c in "$@"; do
        command -v "$c" >/dev/null 2>&1 || die "required command '$c' not found — see docs/architecture/build-environment.md"
    done
}

# Map a Debian architecture name to the names other tools use.
#   arch_info <debian-arch> <field>
arch_info() {
    case "$1:$2" in
        amd64:gnu)    echo x86_64-linux-gnu ;;
        amd64:kernel) echo x86 ;;
        amd64:uname)  echo x86_64 ;;
        amd64:efi)    echo x64 ;;
        amd64:grub)   echo x86_64-efi ;;
        amd64:qemu)   echo qemu-system-x86_64 ;;
        arm64:gnu)    echo aarch64-linux-gnu ;;
        arm64:kernel) echo arm64 ;;
        arm64:uname)  echo aarch64 ;;
        arm64:efi)    echo aa64 ;;
        arm64:grub)   echo arm64-efi ;;
        arm64:qemu)   echo qemu-system-aarch64 ;;
        *) die "unsupported architecture/field: $1/$2 (supported: amd64, arm64)" ;;
    esac
}

host_arch() { dpkg --print-architecture; }

# Foreign-architecture chroots need qemu-user registered with binfmt_misc.
ensure_foreign_arch() {
    local arch="$1"
    [ "$arch" = "$(host_arch)" ] && return 0
    need_cmd arch-test
    if ! arch-test "$arch" >/dev/null 2>&1; then
        die "cannot execute $arch binaries on this host. Install qemu-user-static and register binfmt_misc:
       mount -t binfmt_misc binfmt_misc /proc/sys/fs/binfmt_misc
       cat /usr/lib/binfmt.d/qemu-$(arch_info "$arch" uname).conf > /proc/sys/fs/binfmt_misc/register"
    fi
}

# APT sources lines used while *building* (chroots, image rootfs).
build_sources() {
    local comps="main contrib non-free-firmware"
    printf 'deb %s %s %s\n' "$NEXUS_BUILD_MIRROR" "$NEXUS_DEBIAN_SUITE" "$comps"
    printf 'deb %s %s-updates %s\n' "$NEXUS_BUILD_MIRROR" "$NEXUS_DEBIAN_SUITE" "$comps"
    printf 'deb %s %s-security %s\n' "$NEXUS_BUILD_SECURITY_MIRROR" "$NEXUS_DEBIAN_SUITE" "$comps"
}

# Run a command inside a chroot with the kernel filesystems it needs.
#
# Everything is mounted inside a private mount namespace, so the mounts vanish
# when the command exits — there is no cleanup step that can be skipped and no
# way for a later "rm -rf" on the work directory to descend into the host /dev.
#
#   in_chroot <rootdir> [--bind <src>:<dst>[:ro]]... -- <command> [args...]
in_chroot() {
    local root="$1"; shift
    local binds=()
    while [ "${1:-}" = "--bind" ]; do binds+=("$2"); shift 2; done
    [ "${1:-}" = "--" ] && shift
    [ -d "$root" ] || die "chroot '$root' does not exist"

    unshare --mount --propagation private -- /bin/bash -euc '
        root="$1"; shift
        nbinds="$1"; shift
        mount -t proc proc "$root/proc"
        mount -t sysfs -o ro,nosuid,nodev,noexec sysfs "$root/sys"
        mount --rbind /dev "$root/dev"
        mount -t tmpfs -o mode=755,nosuid,nodev tmpfs "$root/run"
        i=0
        while [ "$i" -lt "$nbinds" ]; do
            spec="$1"; shift; i=$((i+1))
            src="${spec%%:*}"; rest="${spec#*:}"; dst="${rest%%:*}"; opt=""
            [ "$rest" != "$dst" ] && opt="${rest#*:}"
            mkdir -p "$root$dst"
            mount --bind "$src" "$root$dst"
            [ "$opt" = "ro" ] && mount -o remount,bind,ro "$root$dst"
        done
        exec env -i \
            PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
            HOME=/root TERM="${TERM:-dumb}" LANG=C.UTF-8 LC_ALL=C.UTF-8 \
            DEBIAN_FRONTEND=noninteractive \
            SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-}" \
            NEXUS_JOBS="${NEXUS_JOBS:-1}" \
            chroot "$root" "$@"
    ' nexus-chroot "$root" "${#binds[@]}" "${binds[@]}" "$@"
}

# Keyring used to authenticate the Debian archive during bootstrap. On a
# Debian host apt already trusts it; on Ubuntu and derivatives it is only
# available as a file from the debian-archive-keyring package.
debian_keyring() {
    local k
    for k in /usr/share/keyrings/debian-archive-keyring.pgp \
             /usr/share/keyrings/debian-archive-keyring.gpg; do
        [ -s "$k" ] && { echo "$k"; return 0; }
    done
    die "Debian archive keyring not found — install the 'debian-archive-keyring' package"
}

# Refuse to delete a tree that still has something mounted in it.
safe_rmtree() {
    local dir="$1"
    [ -e "$dir" ] || return 0
    case "$(realpath "$dir")" in
        "$(realpath "$NEXUS_WORK")"/*|"$(realpath "$NEXUS_OUT")"/*) ;;
        *) die "refusing to remove '$dir': not inside NEXUS_WORK or NEXUS_OUT" ;;
    esac
    if awk -v d="$(realpath "$dir")" '$5==d || index($5, d"/")==1 {found=1} END{exit !found}' /proc/self/mountinfo; then
        die "refusing to remove '$dir': filesystems are still mounted below it"
    fi
    rm -rf --one-file-system "$dir"
}

# Reproducible timestamps: default to the last commit time.
if [ -z "${SOURCE_DATE_EPOCH:-}" ]; then
    SOURCE_DATE_EPOCH="$(git -C "$NEXUS_ROOT" log -1 --format=%ct 2>/dev/null || date +%s)"
fi
export SOURCE_DATE_EPOCH
