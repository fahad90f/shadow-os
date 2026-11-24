#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Tests build/lib/common.sh in_chroot with an image whose /etc/resolv.conf is
# systemd-resolved's stub link into /run (what every image with
# systemd-resolved has): the command inside must see the build host's resolver
# configuration, and nothing of it may end up in the image.
#
#     sudo bash tests/build/in-chroot-resolver-test.sh <debian-rootfs>
#
# <debian-rootfs> is any Debian root file system (e.g. a build chroot); it is
# only read: the test works on an overlay whose upper layer is a temporary
# directory. Needs root (mounts, chroot). Exit 0 pass, 1 fail, 77 skipped.
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
LOWER=${1:-${SHADOW_TEST_ROOTFS:-}}
if [ "$(id -u)" != 0 ] || [ -z "$LOWER" ] || [ ! -x "$LOWER/bin/cat" ]; then
    echo "in-chroot-resolver-test: SKIPPED (needs root and a Debian rootfs as argument)"
    exit 77
fi
if [ ! -s /etc/resolv.conf ]; then
    echo "in-chroot-resolver-test: SKIPPED (this host has no /etc/resolv.conf)"
    exit 77
fi
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/upper" "$WORK/ovl" "$WORK/merged" "$WORK/sw" "$WORK/so"

# Everything below runs in a private mount namespace: the overlay disappears
# with it.
unshare --mount --propagation private -- bash -s "$REPO" "$LOWER" "$WORK" <<'EOF'
set -u
REPO=$1 LOWER=$2 WORK=$3
M=$WORK/merged
PASSED=0 FAILED=0
check() { if "${@:2}"; then PASSED=$((PASSED + 1)); else FAILED=$((FAILED + 1)); echo "  FAILED: $1"; fi; }
mount -t overlay ovl -o "lowerdir=$LOWER,upperdir=$WORK/upper,workdir=$WORK/ovl" "$M" || exit 1
export SHADOW_WORK=$WORK/sw SHADOW_OUT=$WORK/so
# shellcheck source=../../build/lib/common.sh
. "$REPO/build/lib/common.sh"
set +e
host=$(cat /etc/resolv.conf)
for link in ../run/systemd/resolve/stub-resolv.conf /run/systemd/resolve/stub-resolv.conf; do
    rm -f "$M/etc/resolv.conf"; ln -s "$link" "$M/etc/resolv.conf"
    inside=$(in_chroot "$M" -- cat /etc/resolv.conf 2>/dev/null)
    check "the chroot sees the host resolver through $link" [ "$inside" = "$host" ]
    check "the link itself is unchanged ($link)" [ "$(readlink "$M/etc/resolv.conf")" = "$link" ]
done
check "nothing was written to the image's /run" [ -z "$(ls -A "$WORK/upper/run" 2>/dev/null)" ]
# A link that leads out of /run is not followed or created.
rm -f "$M/etc/resolv.conf"; ln -s ../run/../etc/shadow-test-nowhere "$M/etc/resolv.conf"
in_chroot "$M" -- true
check "a link outside /run is left alone" [ ! -e "$M/etc/shadow-test-nowhere" ]
# A plain file is used as it is (the old behaviour).
rm -f "$M/etc/resolv.conf"; printf 'nameserver 192.0.2.1\n' >"$M/etc/resolv.conf"
check "a plain resolv.conf is used unchanged" [ "$(in_chroot "$M" -- cat /etc/resolv.conf)" = "nameserver 192.0.2.1" ]
echo "in-chroot-resolver tests: $PASSED passed, $FAILED failed"
[ "$FAILED" = 0 ]
EOF
