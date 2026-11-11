#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Run a command where nexus-firewall can be exercised for real without
# touching the firewall or the configuration of the machine it runs on:
#
#   * a new network namespace  → its own, empty nftables ruleset
#   * a new mount namespace with a private copy of /etc bound over /etc
#     → /etc/nexus/firewall.json, /etc/nftables.d/ and the "systemctl enable"
#       symlink are written to the copy, which is discarded afterwards
#
#   tests/isolated.sh <command> [args...]          (as root, e.g. in the build chroot)
#
# Example (build chroot):
#   scripts/dev.sh exec --dir seccenter -- /src/applications/security-center/tests/isolated.sh \
#       /src/applications/security-center/tests/firewall-roundtrip.sh
set -eu
if [ "${NEXUS_SC_ISOLATED:-}" != 1 ]; then
    [ "$(id -u)" = 0 ] || { echo "isolated.sh: must run as root" >&2; exit 1; }
    NEXUS_SC_ISOLATED=1 exec unshare --net --mount --propagation unchanged "$0" "$@"
fi
copy="$(mktemp -d /tmp/nexus-sc-etc.XXXXXX)"
cp -a /etc/. "$copy/"
mount --bind "$copy" /etc
# The NEXUS ruleset from the source tree, when the package is not installed.
if [ -f /src/security/firewall/nftables.conf ] && ! grep -q nexus_filter /etc/nftables.conf 2>/dev/null; then
    cp /src/security/firewall/nftables.conf /etc/nftables.conf
fi
mkdir -p /etc/nexus /etc/nftables.d
rc=0
"$@" || rc=$?
umount /etc 2>/dev/null || true
rm -rf "$copy"
exit "$rc"
