#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Run a command where shadow-firewall can be exercised for real without
# touching the firewall or the configuration of the machine it runs on:
#
#   * a new network namespace  → its own, empty nftables ruleset
#   * a new mount namespace with a private copy of /etc bound over /etc
#     → /etc/shadow-os/firewall.json, /etc/nftables.d/ and the "systemctl enable"
#       symlink are written to the copy (on a tmpfs), which is discarded afterwards
#
#   tests/isolated.sh <command> [args...]          (as root, e.g. in the build chroot)
#
# Example (build chroot):
#   scripts/dev.sh exec --dir seccenter -- /src/applications/security-center/tests/isolated.sh \
#       /src/applications/security-center/tests/firewall-roundtrip.sh
set -eu
if [ "${SHADOW_SC_ISOLATED:-}" != 1 ]; then
    [ "$(id -u)" = 0 ] || { echo "isolated.sh: must run as root" >&2; exit 1; }
    SHADOW_SC_ISOLATED=1 exec unshare --net --mount --propagation unchanged "$0" "$@"
fi
# The copy lives on a tmpfs of this mount namespace: it disappears with the
# namespace even when this script is killed.
copy="$(mktemp -d /tmp/shadow-sc-etc.XXXXXX)"
trap 'umount /etc 2>/dev/null; umount "$copy" 2>/dev/null; rmdir "$copy" 2>/dev/null' EXIT
trap 'exit 143' TERM INT HUP
mount -t tmpfs -o mode=755 tmpfs "$copy"
cp -a /etc/. "$copy/"
mount --bind "$copy" /etc
# The SHADOW ruleset from the source tree, when the package is not installed.
if [ -f /src/security/firewall/nftables.conf ] && ! grep -q shadow_filter /etc/nftables.conf 2>/dev/null; then
    cp /src/security/firewall/nftables.conf /etc/nftables.conf
fi
mkdir -p /etc/shadow-os /etc/nftables.d
rc=0
"$@" &
child=$!
trap 'kill "$child" 2>/dev/null; exit 143' TERM INT HUP
wait "$child" || rc=$?
exit "$rc"
