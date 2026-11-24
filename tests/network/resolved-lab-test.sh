#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs the real systemd-resolved of Debian trixie with Shadow OS's encrypted
# DNS configuration, in private network, mount and PID namespaces, against a
# fake "network" (a router with its own DNS server) and a fake DNS provider
# that speaks plain DNS and DNS-over-TLS (tests/network/dns_responder.py).
#
#     sudo bash tests/network/resolved-lab-test.sh <trixie-rootfs> <systemd-resolved_*.deb>
#
# <trixie-rootfs> is a Debian 13 root file system with systemd, dbus and
# python3 of the same systemd version as the .deb; it is only read (the test
# works on an overlay in a temporary directory). Proves, with resolved 257:
#   * the shipped resolved.conf.d file is accepted and in force (busctl), and
#     shadow-netprivacy's live_dns() reads resolved's D-Bus properties right;
#   * with Domains=~. every name goes to the provider only, the network's own
#     server answers only its own domain (without it, both get every name);
#   * opportunistic DNS-over-TLS uses TLS when the provider offers port 853
#     and falls back to plain DNS when it does not;
#   * strict DNS-over-TLS never sends a plain query, fails without TLS, and
#     verifies the provider's certificate;
#   * the routing domain "~." on a link (what scripts/fix-network.sh adds to
#     a network that blocks port 853) lets that network's server answer;
#   * Hide Me's DNS-over-Tor (shadow-anon's own drop-in text and SHADOWDNS
#     rules, a stand-in for Tor's DNSPort on shadow-anon's DNS_PORT, UDP only
#     and with SO_REUSEADDR like Tor's): Tor is the only global server, also
#     over a user's own provider, and no name reaches the provider (control:
#     the drop-in without its list resets sends names to Quad9 in plain
#     text); names under the network's search domains, and resolved's
#     DNS-over-TLS to the network's server, go to Tor as well (control: with
#     resolved exempt from SHADOWDNS they reach the network);
#     an unprivileged user (uid 4242) cannot bind Tor's DNSPort, neither UDP
#     next to Tor's socket (SO_REUSEADDR, with and without SO_BINDTODEVICE
#     "lo") nor TCP, and gets no name: not resolved's queries, not a
#     program's redirected ones, not resolved's DNS-over-TLS, not its TCP
#     retry after a truncated answer from Tor, not its fallback to TCP when
#     Tor does not answer (controls, with the old DNSPort 5353 that same user
#     binds: its UDP socket next to Tor's gets resolved's and the programs'
#     names, and with TCP sent to the DNSPort, as before the earlier fix, its
#     TCP listener answers resolved's DNS-over-TLS and TCP retries).
# Needs root, unshare, nsenter, ip, openssl, dpkg-deb, iptables, setpriv (only inside
# the private network namespace). Exit 0 pass, 1 fail,
# 77 skipped. Nothing outside the namespaces and the temporary directory changes.
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
LOWER=${1:-} DEB=${2:-}
if [ "$(id -u)" != 0 ] || [ ! -x "${LOWER:-/nonexistent}/usr/bin/busctl" ] || [ ! -f "${DEB:-/nonexistent}" ]; then
    echo "resolved-lab-test: SKIPPED (needs root, a trixie rootfs and the systemd-resolved .deb)"
    exit 77
fi
for tool in unshare nsenter ip openssl dpkg-deb python3 iptables setpriv; do
    command -v "$tool" >/dev/null || { echo "resolved-lab-test: SKIPPED ($tool missing)"; exit 77; }
done
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/upper" "$WORK/ovl" "$WORK/m" "$WORK/log"
openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj "/CN=dns.example.net" \
    -addext "subjectAltName=DNS:dns.example.net" -keyout "$WORK/key.pem" -out "$WORK/cert.pem" >/dev/null 2>&1 ||
    { echo "resolved-lab-test: openssl failed"; exit 1; }

unshare --net --mount --pid --fork --propagation private -- bash -s "$REPO" "$LOWER" "$DEB" "$WORK" <<'EOF'
set -u
REPO=$1 LOWER=$2 DEB=$3 WORK=$4
M=$WORK/m L=$WORK/log
PASSED=0 FAILED=0
check() { if "${@:2}"; then PASSED=$((PASSED + 1)); else FAILED=$((FAILED + 1)); echo "  FAILED: $1"; fi; }
die() { echo "resolved-lab-test: $*"; tail -n 25 "$L/resolved.log" 2>/dev/null; exit 1; }
mount -t proc proc /proc
mount -t overlay ovl -o "lowerdir=$LOWER,upperdir=$WORK/upper,workdir=$WORK/ovl" "$M" || die "overlay"
dpkg-deb -x "$DEB" "$M" || die "cannot unpack $DEB"
install -m 644 "$REPO/security/hardening/resolved.conf.d/80-shadow-privacy.conf" "$M/usr/lib/systemd/resolved.conf.d/"
install -m 755 "$REPO/security/hardening/shadow-netprivacy" "$M/usr/sbin/shadow-netprivacy"
cat "$WORK/cert.pem" >>"$M/etc/ssl/certs/ca-certificates.crt"
mount -t proc proc "$M/proc"
mount --rbind /dev "$M/dev"
mount -t tmpfs tmpfs "$M/run"
mkdir -p "$M/run/dbus" "$M/run/systemd"

# The network: the client namespace (here) and an "upstream" namespace with
# the router (198.51.100.1, DNS for the domain "lan") and the provider
# (192.0.2.53, DNS on 53 and DNS-over-TLS on 853 once started with --tls).
unshare --net sleep 600 & UP=$!
sleep 0.3
UPNS=(nsenter --net=/proc/$UP/ns/net)
ip link set lo up
ip link add lan0 type veth peer name up0 netns "$UP" || die "veth"
ip addr add 198.51.100.2/24 dev lan0; ip link set lan0 up
ip route add default via 198.51.100.1
"${UPNS[@]}" ip link set lo up
"${UPNS[@]}" ip addr add 192.0.2.53/32 dev lo
"${UPNS[@]}" ip addr add 198.51.100.1/24 dev up0
"${UPNS[@]}" ip link set up0 up
"${UPNS[@]}" python3 -I "$REPO/tests/network/dns_responder.py" 198.51.100.1 "$L/router" & ROUTER=$!
start_provider() {   # start_provider plain|tls
    [ -n "${PROVIDER:-}" ] && { kill "$PROVIDER"; wait "$PROVIDER" 2>/dev/null; }
    if [ "$1" = tls ]; then
        "${UPNS[@]}" python3 -I "$REPO/tests/network/dns_responder.py" 192.0.2.53 "$L/provider" --tls "$WORK/cert.pem" "$WORK/key.pem" &
    else
        "${UPNS[@]}" python3 -I "$REPO/tests/network/dns_responder.py" 192.0.2.53 "$L/provider" &
    fi
    PROVIDER=$!
    sleep 0.5
}
start_provider plain

in_m() { chroot "$M" env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin LC_ALL=C.UTF-8 "$@"; }
in_m systemd-sysusers >/dev/null 2>&1 || die "systemd-sysusers"
in_m dbus-daemon --system --fork --nopidfile || die "dbus-daemon"
stop_resolved() {
    [ -n "${RESOLVED:-}" ] || return 0
    kill "$RESOLVED" 2>/dev/null
    wait "$RESOLVED" 2>/dev/null
    RESOLVED=
}
start_resolved() {   # start_resolved [extra routing domain of lan0]
    stop_resolved
    # Started directly (not through a function), so that $! is resolved itself.
    chroot "$M" env -i PATH=/usr/sbin:/usr/bin LC_ALL=C.UTF-8 SYSTEMD_LOG_LEVEL="${LOGLEVEL:-info}" \
        /usr/lib/systemd/systemd-resolved >>"$L/resolved.log" 2>&1 &
    RESOLVED=$!
    for _ in $(seq 50); do
        in_m busctl get-property org.freedesktop.resolve1 /org/freedesktop/resolve1 org.freedesktop.resolve1.Manager DNSOverTLS >/dev/null 2>&1 && break
        sleep 0.1
    done
    # The link as NetworkManager configures it (default route, its search domain).
    in_m resolvectl dns lan0 198.51.100.1 && in_m resolvectl domain lan0 lan "$@" &&
        in_m resolvectl default-route lan0 yes && in_m resolvectl llmnr lan0 no && in_m resolvectl mdns lan0 no &&
        in_m resolvectl dnsovertls lan0 opportunistic || die "resolvectl could not configure lan0"
}
user_conf() {   # user_conf <rendered 90-shadow-user.conf text>
    mkdir -p "$M/etc/systemd/resolved.conf.d"
    printf '%s' "$1" >"$M/etc/systemd/resolved.conf.d/90-shadow-user.conf"
}
render() {   # render mode dnssec -> shadow-netprivacy's text for the lab provider
    python3 -I - "$REPO/security/hardening/shadow-netprivacy" "$1" "$2" <<'PY'
import importlib.machinery, importlib.util, sys
sys.dont_write_bytecode = True
loader = importlib.machinery.SourceFileLoader("np", sys.argv[1])
spec = importlib.util.spec_from_loader("np", loader); np = importlib.util.module_from_spec(spec); loader.exec_module(np)
sys.stdout.write(np.render_resolved(sys.argv[2], np.parse_provider("custom:192.0.2.53#dns.example.net"), sys.argv[3]))
PY
}
lookup() { timeout 30 chroot "$M" env -i PATH=/usr/bin LC_ALL=C.UTF-8 resolvectl query --cache=no --legend=no "$1" >/dev/null 2>&1; }
got() { grep -qx "$2 $3" "$L/$1" 2>/dev/null; }      # got router|provider udp|tcp|tls name
none() { ! grep -q " $2\$" "$L/$1" 2>/dev/null; }      # none router|provider name

# 1. The shipped file alone (Quad9 is unreachable here; only the settings count).
LOGLEVEL=debug start_resolved
check "resolved accepts the shipped file (no parse warnings)" bash -c "! grep -i 'unknown\|invalid\|failed to parse' '$L/resolved.log' | grep -i 80-shadow-privacy"
props=$(in_m busctl get-property org.freedesktop.resolve1 /org/freedesktop/resolve1 org.freedesktop.resolve1.Manager DNSOverTLS DNSSEC LLMNR MulticastDNS)
check "shipped defaults in force (got: $(echo "$props" | tr '\n' ' '))" [ "$props" = "$(printf 's "opportunistic"\ns "allow-downgrade"\ns "no"\ns "no"')" ]
live=$(in_m python3 -I -c '
import importlib.machinery, importlib.util, sys
sys.dont_write_bytecode = True
loader = importlib.machinery.SourceFileLoader("np", "/usr/sbin/shadow-netprivacy")
spec = importlib.util.spec_from_loader("np", loader); np = importlib.util.module_from_spec(spec); loader.exec_module(np)
print(np.live_dns())')
check "shadow-netprivacy reads resolved over D-Bus (got: $live)" grep -q "'dns_over_tls': 'opportunistic', 'dnssec': 'allow-downgrade', 'llmnr': 'no', 'mdns': 'no'" <<<"$live"
global=$(in_m resolvectl dns | awk '$1 == "Global:"')
check "Quad9 with TLS names is the global server" grep -q "9.9.9.9#dns.quad9.net" <<<"$global"

# 2. Routing: dot (opportunistic) to the lab provider, Domains=~.
user_conf "$(render dot allow-downgrade)"
start_resolved
lookup probe-a.example.net; rc=$?
check "dot, DNSSEC allow-downgrade, plain provider: the lookup works" [ "$rc" = 0 ]
check "a name goes to the provider" got provider udp probe-a.example.net
check "...and not to the network's server" none router probe-a.example.net
lookup printer.lan
check "the network's own domain goes to its server" got router udp printer.lan
check "...and not to the provider" none provider printer.lan
# Negative control: without Domains=~. the network's server sees every name.
user_conf "$(render dot allow-downgrade | sed '/^Domains=~\.$/d')"
start_resolved
lookup probe-b.example.net
check "control: without Domains=~. the router also gets the name" got router udp probe-b.example.net

# 3. Opportunistic DNS-over-TLS with a provider that offers port 853.
start_provider tls
user_conf "$(render dot allow-downgrade)"
start_resolved
lookup probe-c.example.net; rc=$?
check "opportunistic + allow-downgrade over TLS: the lookup works" [ "$rc" = 0 ]
check "opportunistic: the provider is asked over TLS" got provider tls probe-c.example.net
check "opportunistic: no plain query to the provider" bash -c "! grep -qx 'udp probe-c.example.net' '$L/provider'"

# 4. Strict DNS-over-TLS: TLS only, certificate verified.
user_conf "$(render dot-strict allow-downgrade)"
start_resolved
lookup probe-d.example.net; rc=$?
check "strict: the lookup works over TLS" [ "$rc" = 0 ]
check "strict: asked over TLS" got provider tls probe-d.example.net
start_provider plain
start_resolved
lookup probe-e.example.net; rc=$?
check "strict without port 853: the lookup fails" [ "$rc" != 0 ]
check "strict without port 853: nothing in plain text" none provider probe-e.example.net
# The relaxation fix-network.sh applies to one network that blocks port 853.
start_resolved "~."
lookup probe-f.example.net; rc=$?
check "strict + '~.' on the network: its server answers" [ "$rc" = 0 ]
check "strict + '~.': the router was asked" got router udp probe-f.example.net
check "strict + '~.': still nothing plain to the provider" none provider probe-f.example.net
# Strict with a certificate for another name is refused.
start_provider tls
sed -i 's/#dns.example.net/#other.example.net/g' "$M/etc/systemd/resolved.conf.d/90-shadow-user.conf"
start_resolved
lookup probe-g.example.net; rc=$?
check "strict: a certificate for another name is refused" [ "$rc" != 0 ]

# 5. Hide Me's DNS-over-Tor: shadow-anon's own drop-in, after the shipped file
# (and a user's own provider). A fake Quad9 (9.9.9.9) answers plain DNS; the
# stand-in for Tor's DNSPort answers on 127.0.0.1:$TORPORT (shadow-anon's
# DNS_PORT, a port only root may bind), UDP only and with SO_REUSEADDR, like
# Tor's. OLDPORT is the DNSPort of earlier versions, which any user may bind.
RESOLVE_UID=$(in_m id -u systemd-resolve) || die "no systemd-resolve user"
hideme() {   # hideme port | dropin | rules [exempt|old-port|tcp-to-dnsport]: DNS_PORT / the drop-in text / SHADOWDNS, loaded
    python3 -I - "$REPO/distro/base/rootfs/usr/bin/shadow-anon" "$RESOLVE_UID" "$@" <<'PY'
import importlib.machinery, importlib.util, sys
sys.dont_write_bytecode = True
loader = importlib.machinery.SourceFileLoader("sa", sys.argv[1])
spec = importlib.util.spec_from_loader("sa", loader); sa = importlib.util.module_from_spec(spec); loader.exec_module(sa)
if sys.argv[3] == "port":
    print(sa.DNS_PORT)
    sys.exit(0)
if sys.argv[3] == "dropin":
    sys.stdout.write(sa.RESOLVED_DROPIN_TEXT)
    sys.exit(0)
sa.TOR_USER, sa.RESOLVED_USER = "4243", sys.argv[2]          # the lab's uids
sa.user_exists = lambda name: True
rules = sa.dns_rules()
def to_ports(rules, ports):
    return [[ports.get(x, x) if i and r[i - 1] == "--to-ports" else x for i, x in enumerate(r)] for r in rules]
if sys.argv[4:] == ["exempt"]:                               # control: the earlier rules
    rules = [r for r in rules if "853" not in r]
    rules.insert(1, ["-m", "owner", "--uid-owner", sa.RESOLVED_USER, "-j", "RETURN"])
elif sys.argv[4:] == ["old-port"]:                           # control: the DNSPort on 5353, as before this fix
    rules = to_ports(rules, {sa.DNS_PORT: "5353"})
    rules = [["5353" if i and r[i - 1] == "--dport" and x == sa.DNS_PORT else x for i, x in enumerate(r)]
             for r in rules]
elif sys.argv[4:] == ["tcp-to-dnsport"]:                     # control: ...and TCP sent to it, as before that
    rules = to_ports(rules, {sa.DNS_PORT: "5353", sa.CLOSED_PORT: "5353"})
    rules = [["5353" if i and r[i - 1] == "--dport" and x == sa.DNS_PORT else x for i, x in enumerate(r)]
             for r in rules]
sa._load_chain(sa.DNS_CHAIN, rules)
PY
}
TORPORT=$(hideme port) && [ -n "$TORPORT" ] || die "shadow-anon's DNS_PORT"
OLDPORT=5353
"${UPNS[@]}" ip addr add 9.9.9.9/32 dev lo
"${UPNS[@]}" python3 -I "$REPO/tests/network/dns_responder.py" 9.9.9.9 "$L/quad9" & QUAD9=$!
python3 -I "$REPO/tests/network/dns_responder.py" 127.0.0.1 "$L/tor" --port "$TORPORT" --udp-only & TORDNS=$!
sleep 0.5
hideme_dropin() {   # hideme_dropin [sed expression]
    mkdir -p "$M/run/systemd/resolved.conf.d"
    hideme dropin | sed "${1:-}" >"$M/run/systemd/resolved.conf.d/95-shadow-anon.conf"
}
rm -f "$M/etc/systemd/resolved.conf.d/90-shadow-user.conf"
start_provider plain
hideme_dropin
start_resolved
check "resolved runs as systemd-resolve (uid $RESOLVE_UID)" [ "$(awk '/^Uid:/ {print $2}' "/proc/$RESOLVED/status")" = "$RESOLVE_UID" ]
global=$(in_m resolvectl dns | awk '$1 == "Global:"' | sed 's/ *$//')
check "Hide Me: Tor's DNSPort is the only global server (got: $global)" [ "$global" = "Global: 127.0.0.1:$TORPORT" ]
check "Hide Me: Tor's DNSPort is below 53 (only root may bind it): $TORPORT" [ "$TORPORT" -lt 53 ]
lookup hideme-a.example.net; rc=$?
check "Hide Me: the lookup works" [ "$rc" = 0 ]
check "Hide Me: the name went to Tor" got tor udp hideme-a.example.net
check "Hide Me: ...not to Quad9" none quad9 hideme-a.example.net
user_conf "$(render dot-strict allow-downgrade)"
start_resolved
global=$(in_m resolvectl dns | awk '$1 == "Global:"' | sed 's/ *$//')
check "Hide Me over a user's provider: Tor only (got: $global)" [ "$global" = "Global: 127.0.0.1:$TORPORT" ]
lookup hideme-b.example.net
check "Hide Me over a user's provider: the name went to Tor" got tor udp hideme-b.example.net
check "Hide Me over a user's provider: ...not to the provider" none provider hideme-b.example.net
# Control: the drop-in without its list resets (as up to this fix) leaves the
# provider first, and DNS-over-TLS off.
rm -f "$M/etc/systemd/resolved.conf.d/90-shadow-user.conf"
hideme_dropin '/^DNS=$/d; /^Domains=$/d; /^FallbackDNS=$/d'
start_resolved
lookup hideme-c.example.net
check "control: without the resets the name goes to Quad9 in plain text" got quad9 udp hideme-c.example.net
check "control: ...and not to Tor" none tor hideme-c.example.net

# 6. Names under the network's search domains (and resolved's DNS-over-TLS to
# the network's server) with SHADOWDNS: to Tor, not to the network. The router
# answers DNS-over-TLS too; its search domains include "com". An unprivileged
# user (uid 4242) tries to bind Tor's DNSPort: UDP next to Tor's socket (both
# SO_REUSEADDR, which on Linux lets a socket of any user bind the same
# address and port; with SO_BINDTODEVICE "lo", which needs no privilege, it
# would win every datagram) and TCP (Tor's DNSPort has none). It gets the old
# DNSPort 5353 on UDP and TCP (DNS-over-TLS with a certificate nobody trusts,
# and plain DNS), answering 203.0.113.66: with Hide Me's rules it must get no
# name, whatever Tor does.
kill "$ROUTER"; wait "$ROUTER" 2>/dev/null
"${UPNS[@]}" python3 -I "$REPO/tests/network/dns_responder.py" 198.51.100.1 "$L/router" --tls "$WORK/cert.pem" "$WORK/key.pem" & ROUTER=$!
R=$WORK/rogue
mkdir -m 755 "$R" && chmod 711 "$WORK" && cp "$REPO/tests/network/dns_responder.py" "$R/" || die "rogue directory"
openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj "/CN=attacker.invalid" -keyout "$R/key.pem" \
    -out "$R/cert.pem" >/dev/null 2>&1 || die "openssl (rogue)"
cat >"$R/rogue.py" <<'PY'
# rogue.py <dir> rogue <log> <cert> <key> <port> <old port> <binds>: a user (uid
#   4242) binding <port> (Tor's DNSPort: UDP with SO_REUSEADDR, with and without
#   SO_BINDTODEVICE "lo", and TCP) and <old port> (UDP the same way, and TCP:
#   DNS-over-TLS and plain DNS); what each bind gave goes to <binds>; every A
#   answer is 203.0.113.66, over UDP truncated for "tc-*" names (TCP follows).
# rogue.py <dir> tor-tc|tor-silent <log> <port>: the stand-in for Tor's DNSPort
#   (UDP only, SO_REUSEADDR) answering "tc-*" names truncated (TC set) / never.
import socket, ssl, struct, sys, threading
sys.path.insert(0, sys.argv[1])
import dns_responder as d
mode, log = sys.argv[2], sys.argv[3]
srv = d.Server("127.0.0.1", log)
def udp_socket(port, lo=False):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if lo:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE, b"lo")
    sock.bind(("127.0.0.1", port))
    return sock
def tcp_socket(port):
    sock = socket.socket()
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("127.0.0.1", port))
    sock.listen(16)
    return sock
def serve_udp(sock, tc=False, silent=False):
    while True:
        data, peer = sock.recvfrom(4096)
        name, reply = d.answer(data)
        srv.record("udp", name)
        if silent:
            continue
        if tc and name.startswith("tc-"):
            reply = reply[:2] + struct.pack(">H", struct.unpack(">H", reply[2:4])[0] | 0x0200) + reply[4:]
        sock.sendto(reply, peer)
if mode != "rogue":
    serve_udp(udp_socket(int(sys.argv[4])), tc=mode == "tor-tc", silent=mode == "tor-silent")
d.RECORDS[1] = bytes([203, 0, 113, 66])
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(sys.argv[4], sys.argv[5])
def serve(conn):
    try:
        tls = conn.recv(1, socket.MSG_PEEK) == b"\x16"          # a TLS handshake
        if tls:
            conn = ctx.wrap_socket(conn, server_side=True)
    except (OSError, ssl.SSLError):
        conn.close()
        return
    srv.stream(conn, "tls" if tls else "tcp")
def serve_tcp(sock):
    while True:
        threading.Thread(target=serve, args=(sock.accept()[0],), daemon=True).start()
port, old, results = int(sys.argv[6]), int(sys.argv[7]), []
for proto, p, lo in (("udp", port, True), ("udp", port, False), ("tcp", port, False),
                     ("udp", old, True), ("tcp", old, False)):
    what = f"{proto} {p}{' lo' if lo else ''}"
    try:
        sock = udp_socket(p, lo) if proto == "udp" else tcp_socket(p)
    except OSError as e:
        results.append(f"{what} {e.strerror}")
        continue
    results.append(f"{what} bound")
    threading.Thread(target=serve_udp if proto == "udp" else serve_tcp, args=(sock,),
                     kwargs={"tc": True} if proto == "udp" else {}, daemon=True).start()
with open(sys.argv[8], "w") as f:
    f.write("\n".join(results) + "\n")
threading.Event().wait()
PY
chmod 644 "$R"/* && : >"$L/rogue" && : >"$L/rogue-binds" && chmod 666 "$L/rogue" "$L/rogue-binds" || die "rogue files"
setpriv --reuid=4242 --regid=4242 --clear-groups python3 -I "$R/rogue.py" "$R" rogue "$L/rogue" "$R/cert.pem" "$R/key.pem" \
    "$TORPORT" "$OLDPORT" "$L/rogue-binds" & ROGUE=$!
tor_standin() {   # tor_standin plain|tc|silent [port]: (re)start the stand-in for Tor's DNSPort
    kill "$TORDNS" 2>/dev/null; wait "$TORDNS" 2>/dev/null
    if [ "$1" = plain ]; then
        python3 -I "$REPO/tests/network/dns_responder.py" 127.0.0.1 "$L/tor" --port "${2:-$TORPORT}" --udp-only & TORDNS=$!
    else
        python3 -I "$R/rogue.py" "$R" "tor-$1" "$L/tor" "${2:-$TORPORT}" & TORDNS=$!
    fi
    sleep 0.5
}
refused() {   # refused <awk regex>: packets SHADOWDNS sent to the closed port, by the matching rules
    iptables -t nat -L SHADOWDNS -v -n -x | awk -v p="$1" '$0 ~ p && / redir ports 1$/ {n += $1} END {print n + 0}'
}
progquery() {   # progquery <name>: another program (uid 4246) asks resolved's stub directly -> the A answer
    setpriv --reuid=4246 --regid=4246 --clear-groups python3 -I - "$1" <<'PY'
import socket, struct, sys
q = b"\x12\x34\x01\x00\x00\x01\x00\x00\x00\x00\x00\x00"
q += b"".join(bytes([len(x)]) + x.encode() for x in sys.argv[1].split(".")) + b"\x00\x00\x01\x00\x01"
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.settimeout(5)
s.sendto(q, ("127.0.0.53", 53))
try:
    print(".".join(map(str, s.recvfrom(4096)[0][-4:])))
except OSError:
    print("none")
PY
}
sleep 0.5
check "a user (uid 4242) may not bind UDP 127.0.0.1:$TORPORT next to Tor (SO_REUSEADDR, SO_BINDTODEVICE lo)" \
    grep -qx "udp $TORPORT lo Permission denied" "$L/rogue-binds"
check "...nor with SO_REUSEADDR alone" grep -qx "udp $TORPORT Permission denied" "$L/rogue-binds"
check "...nor TCP 127.0.0.1:$TORPORT" grep -qx "tcp $TORPORT Permission denied" "$L/rogue-binds"
check "...but binds the old DNSPort $OLDPORT (UDP and TCP: the controls below)" \
    bash -c "grep -qx 'udp $OLDPORT lo bound' '$L/rogue-binds' && grep -qx 'tcp $OLDPORT bound' '$L/rogue-binds'"
check "only the stand-in for Tor holds UDP $TORPORT" \
    bash -c "[ \"\$(ss -Hulnp 'sport = :$TORPORT' | grep -c .)\" = 1 ] && ss -Hulnp 'sport = :$TORPORT' | grep -q 'pid=$TORDNS,'"
check "...nor bind the port DNS over TCP is sent to" \
    bash -c "! setpriv --reuid=4242 --regid=4242 --clear-groups python3 -I -c 'import socket; socket.socket().bind((\"127.0.0.1\", 1))' 2>/dev/null"
hideme_dropin
hideme rules || die "SHADOWDNS could not be loaded"
start_resolved com
for name in printer-s.lan www-s.example.com hideme-d.example.net; do      # names not asked above
    lookup "$name"; rc=$?
    check "SHADOWDNS: $name resolves" [ "$rc" = 0 ]
    check "SHADOWDNS: $name went to Tor" got tor udp "$name"
    check "SHADOWDNS: ...not to the network's server" none router "$name"
    check "SHADOWDNS: ...not to the user (uid 4242)" none rogue "$name"
done
ans=$(progquery prog-s.example.net)
check "SHADOWDNS: a program's own query to the stub went to Tor (answer: $ans)" \
    bash -c "[ '$ans' = 203.0.113.7 ] && grep -qx 'udp prog-s.example.net' '$L/tor'"
check "SHADOWDNS: ...not to the user" none rogue prog-s.example.net
check "SHADOWDNS: resolved's DNS-over-TLS was refused (closed port, not the DNSPort)" [ "$(refused 'dpt:853 ')" -gt 0 ]
# Tor answers truncated: resolved retries over TCP, to Tor's DNSPort for a
# global name, to the network's server (port 53, redirected) for its domain.
tor_standin tc
start_resolved com
before=$(refused "dpt:$TORPORT ") before53=$(refused 'tcp dpt:53 ')
lookup tc-g.example.net
lookup tc-l.lan
check "truncated: the names went to Tor over UDP" bash -c "grep -qx 'udp tc-g.example.net' '$L/tor' && grep -qx 'udp tc-l.lan' '$L/tor'"
check "truncated: resolved's TCP retry to Tor's DNSPort was refused" [ "$(refused "dpt:$TORPORT ")" -gt "$before" ]
check "truncated: its TCP retry to the network's server was refused" [ "$(refused 'tcp dpt:53 ')" -gt "$before53" ]
check "truncated: ...neither reached the user" bash -c "! grep -q ' tc-' '$L/rogue'"
# Tor does not answer (not connected yet): after some lost queries resolved
# falls back to TCP.
tor_standin silent
start_resolved com
before=$(refused "dpt:$TORPORT ")
for i in 1 2 3 4; do lookup "silent-$i.example.net" & done      # they end when resolved restarts
for _ in $(seq 50); do
    [ "$(refused "dpt:$TORPORT ")" -gt "$before" ] && break
    sleep 1
done
check "silent Tor: the names were asked over UDP" got tor udp silent-1.example.net
check "silent Tor: resolved fell back to TCP, which was refused" [ "$(refused "dpt:$TORPORT ")" -gt "$before" ]
check "silent Tor: ...and that did not reach the user" bash -c "! grep -q ' silent-' '$L/rogue'"
# Control: Tor's DNSPort on 5353 (the drop-in and rules before this fix; TCP
# refused): the same user's UDP socket, bound next to Tor's, gets resolved's
# queries and the programs' redirected ones, and answers them.
tor_standin plain "$OLDPORT"
check "control: a stand-in for Tor (SO_REUSEADDR) and the user both hold UDP $OLDPORT" \
    bash -c "ss -Hulnp 'sport = :$OLDPORT' | grep -q 'pid=$TORDNS,' && ss -Hulnp 'sport = :$OLDPORT' | grep -q 'pid=$ROGUE,'"
hideme_dropin "s/127.0.0.1:$TORPORT/127.0.0.1:$OLDPORT/"
hideme rules old-port || die "SHADOWDNS (control) could not be loaded"
start_resolved com
lookup old-a.example.net
check "control: with the DNSPort on $OLDPORT the user gets resolved's query" got rogue udp old-a.example.net
check "control: ...and Tor does not" none tor old-a.example.net
ans=$(progquery prog-o.example.net)
check "control: ...and a program's, and answers it (answer: $ans)" \
    bash -c "[ '$ans' = 203.0.113.66 ] && grep -qx 'udp prog-o.example.net' '$L/rogue'"
# Control: TCP sent to the DNSPort too (the rules before the earlier fix):
# the user's TCP listener reads the names and answers for the whole system.
hideme rules tcp-to-dnsport || die "SHADOWDNS (control) could not be loaded"
start_resolved com
lookup printer-y.lan
check "control: with TCP sent to the DNSPort the user gets resolved's DNS-over-TLS" got rogue tls printer-y.lan
lookup tc-y.example.net
check "control: ...and its TCP retry after a truncated answer (the user's own)" got rogue tcp tc-y.example.net
tor_standin plain
hideme_dropin
# Control: resolved exempt (the earlier rules): the network gets those names.
hideme rules exempt || die "SHADOWDNS (control) could not be loaded"
start_resolved com
lookup printer-x.lan
lookup www-x.example.com
check "control: with resolved exempt the network's server gets its domain's names" \
    bash -c "grep -qx '\(tls\|udp\) printer-x.lan' '$L/router' && grep -qx '\(tls\|udp\) www-x.example.com' '$L/router'"
iptables -t nat -D OUTPUT -j SHADOWDNS 2>/dev/null
rm -f "$M/run/systemd/resolved.conf.d/95-shadow-anon.conf"
kill "$QUAD9" "$TORDNS" "$ROGUE" 2>/dev/null

stop_resolved
kill "$PROVIDER" "$ROUTER" "$UP" 2>/dev/null
echo "resolved-lab tests: $PASSED passed, $FAILED failed"
[ "$FAILED" = 0 ] || { echo "--- resolved log (tail) ---"; tail -n 30 "$L/resolved.log"; echo "--- provider ---"; cat "$L/provider"; echo "--- router ---"; cat "$L/router"; echo "--- tor ---"; cat "$L/tor" 2>/dev/null; echo "--- quad9 ---"; cat "$L/quad9" 2>/dev/null; echo "--- rogue (uid 4242) ---"; cat "$L/rogue" 2>/dev/null; }
[ "$FAILED" = 0 ]
EOF
