#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# real-tool-documents.sh OUT: run the REAL shadow-killswitch, shadow-panic and
# shadow-security-status from the tree in a throw-away network namespace (the
# machine's nft), with their path constants pointed at OUT, and keep the documents
# they publish: OUT/on/{killswitch,panic,live}.json (kill switch tor, panic
# engaged), OUT/off/... (kill switch off, panic released) and OUT/stale/...
# (mode vpn recorded as loaded, then its table deleted behind the tool's back).
# test_killswitch_backend.py reads them with SHADOW_CC_CONTRACT_DIR=OUT.
#
# Needs root, unshare, ip and nft on the machine that runs it. Everything
# happens in a private network and mount namespace with /run on a tmpfs (the
# namespace is checked to have nothing but "lo" first); the panic's radio and
# NetworkManager steps point at empty directories and files under OUT, and
# its configuration turns the radio and session-lock steps off.
set -euo pipefail
[ $# -eq 1 ] || { echo "usage: $0 OUT_DIR" >&2; exit 2; }
OUT="$(readlink -m "$1")"
TREE="$(cd "$(dirname "$0")/../../.." && pwd)"
# Only ever deletes a directory this script made before.
if [ -e "$OUT" ] && [ -n "$(ls -A "$OUT" 2>/dev/null)" ] && [ ! -e "$OUT/.real-tool-documents" ]; then
    echo "$OUT exists and was not made by this script; choose an empty or new directory" >&2
    exit 2
fi
rm -rf "$OUT"; mkdir -p "$OUT"; : > "$OUT/.real-tool-documents"
exec unshare --net --mount --fork --propagation private -- /bin/bash -euc '
OUT="$1"; TREE="$2"
# Safety: a fresh namespace has only "lo"; refuse anything else.
links=$(ip -o link show | awk -F": " "{print \$2}" | tr "\n" " ")
[ "$links" = "lo " ] || { echo "not a fresh network namespace: $links" >&2; exit 1; }
ip link set lo up
mount -t tmpfs tmpfs /run            # nothing below touches the host /run
S="$OUT/state"; mkdir -p "$S/run" "$S/etc" "$S/var" "$S/rfkill" "$S/rfkill-saved"
echo "BOOT_IMAGE=/vmlinuz root=/dev/vda1" > "$S/cmdline"
printf "%s\n" "{\"rfkill\": false, \"lock_session\": false, \"clear_clipboard\": true}" > "$S/etc/panic.json"
BOOT="
import importlib.machinery, importlib.util, json, sys
sys.dont_write_bytecode = True
loader = importlib.machinery.SourceFileLoader(\"tool\", sys.argv[1])
spec = importlib.util.spec_from_loader(loader.name, loader)
mod = importlib.util.module_from_spec(spec)
loader.exec_module(mod)
for k, v in json.loads(sys.argv[2]).items():
    setattr(mod, k, tuple(v) if isinstance(v, list) else v)
sys.argv = [sys.argv[1]] + sys.argv[3:]
r = mod.main(sys.argv[1:]) if mod.main.__code__.co_argcount else mod.main()
sys.exit(r)
"
KS="{\"RUN_DIR\": \"$S/run\", \"CONFIG\": \"$S/etc/killswitch.json\", \"CMDLINE\": \"$S/cmdline\"}"
PN="{\"RUN_DIR\": \"$S/run\", \"CONFIG\": \"$S/etc/panic.json\", \"PENDING\": \"$S/var/panic-pending.json\", \"NM_STATE_FILE\": \"$S/var/nm.state\", \"RFKILL_SYSFS\": \"$S/rfkill\", \"RFKILL_SAVED\": \"$S/rfkill-saved\"}"
ST="{\"KILLSWITCH_TOOL\": \"$TREE/security/firewall/shadow-killswitch\", \"KILLSWITCH_STATUS\": \"$S/run/killswitch.json\", \"KILLSWITCH_CONFIG\": \"$S/etc/killswitch.json\", \"PANIC_TOOL\": \"$TREE/security/firewall/shadow-panic\", \"PANIC_STATUS\": \"$S/run/panic.json\"}"
tool() { python3 -I -c "$BOOT" "$@"; }
snap() {
    mkdir -p "$OUT/$1"
    tool "$TREE/security/audit/shadow-security-status" "$ST" --section killswitch,panic > "$OUT/$1/live.json"
    cp "$S/run/killswitch.json" "$S/run/panic.json" "$OUT/$1/" 2>/dev/null || true
    nft list tables > "$OUT/$1/tables.txt"
}
set +e
tool "$TREE/security/firewall/shadow-killswitch" "$KS" set tor --runtime; echo "set tor rc=$?"
tool "$TREE/security/firewall/shadow-panic" "$PN" engage; echo "engage rc=$?"
snap on
tool "$TREE/security/firewall/shadow-panic" "$PN" release; echo "release rc=$?"
tool "$TREE/security/firewall/shadow-killswitch" "$KS" set off --runtime; echo "set off rc=$?"
snap off
# A table removed behind the tool'"'"'s back: the recorded file still says loaded.
tool "$TREE/security/firewall/shadow-killswitch" "$KS" set vpn --runtime; echo "set vpn rc=$?"
nft delete table inet shadow_egress; echo "delete rc=$?"
snap stale
' x "$OUT" "$TREE"
