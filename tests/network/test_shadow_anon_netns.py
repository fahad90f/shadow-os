#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Hide Me's nat chains, and Ghost Mode's kill switch, with real packets.

    sudo python3 tests/network/test_shadow_anon_netns.py [-v]

Needs root, unshare, iptables (iptables-nft), nft, ip, setpriv and
conntrack; without root or unshare it exits 77 (skipped). The program re-executes itself with "unshare --net --mount
--fork": everything below happens in a private, throw-away network
namespace (checked before anything is changed), so the computer's own
network is never touched.

shadow-anon is loaded as a module (every path constant in a temporary
directory) and its real functions load the real rules through the real
iptables-nft: SHADOWTOR (transparent proxy) and SHADOWDNS (DNS-over-Tor).
For the kill switch, shadow-anon calls a "shadow-killswitch" on PATH that
is the real security/firewall/shadow-killswitch with its paths in the
temporary directory, so the whole delegation (set, status --json, restore)
runs against a real nftables table.

Topology (RFC 5737 documentation addresses, plus private ones for the
.onion and LAN cases):

    host (this namespace)                          "up" (the network)
      veth-h 198.51.100.1/24  ─────────────────────  veth-u 198.51.100.2/24
      default route via 198.51.100.2                        10.1.2.3/32, 10.192.0.5/32
      127.0.0.1:40    fake Tor TransPort              tcp :80  answers "up"
                      (answers "tp <original destination>")
      127.0.0.1:26    fake Tor DNSPort ("tor-dns",    udp :53  answers "net-dns"
                      UDP only, SO_REUSEADDR,         tcp :853 answers "net-dot" (DNS-over-TLS)
                      like Tor's)                     udp :7001 echoes

(The ports are shadow-anon's TPROXY_PORT and DNS_PORT; the stand-ins run as
root, as Tor binds them before it changes user.) Probes run as different
users with setpriv: 4242 (a program, or an attacker), 4243 (stands in for
debian-tor), 4245 (stands in for systemd-resolve), 4246 (another program).
Every test has a positive control. Counters are read with "iptables -t nat
-L -v -x".
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from io import StringIO
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tests", "security"))
import shadow_anon_testlib as lib  # noqa: E402

KILLSWITCH = os.path.join(ROOT, "security", "firewall", "shadow-killswitch")
INNER = "SHADOW_ANON_NETNS_INNER"
OUTER_NETNS = "SHADOW_ANON_OUTER_NETNS"
USER, TOR, RESOLVE, OTHER = 4242, 4243, 4245, 4246
_PORTS = lib.load_tool()
TPROXY_PORT, DNS_PORT = int(_PORTS.TPROXY_PORT), int(_PORTS.DNS_PORT)
OLD_TPROXY_PORT, OLD_DNS_PORT = 9040, 5353              # earlier versions': any user may bind them
# A dropped TCP SYN times out; a UDP datagram dropped in the output hook gives EPERM.
BLOCKED = ("timeout", "eperm")

SERVER = r"""
import socket, struct, sys, threading
def tcp(addr, port, answer):
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind((addr, port)); s.listen(64)
    while True:
        c, _ = s.accept()
        with c:
            text = answer
            if answer == "tp":                      # what a transparent proxy sees: the original destination
                raw = c.getsockopt(socket.SOL_IP, 80, 16)          # SO_ORIGINAL_DST
                port_, ip = struct.unpack("!2xH4s8x", raw)
                text = "tp %s:%d" % (socket.inet_ntoa(ip), port_)
            c.sendall(text.encode())
def udp(addr, port, answer, *opts):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)     # as Tor's DNSPort does
    if "lo" in opts:                                            # no privilege needed
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE, b"lo")
    s.bind((addr, port))
    while True:
        d, peer = s.recvfrom(64)
        s.sendto(d if answer == "echo" else answer.encode(), peer)
for spec in sys.argv[1:]:
    proto, addr, port, answer, *opts = spec.split(",")
    threading.Thread(target=tcp if proto == "tcp" else udp, args=(addr, int(port), answer, *opts),
                     daemon=True).start()
import time; time.sleep(0.3)
print("ready", flush=True)
threading.Event().wait()
"""

PROBE = r"""
import json, socket, sys
out = []
for proto, dst, port in json.loads(sys.argv[1]):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM if proto == "tcp" else socket.SOCK_DGRAM)
    s.settimeout(1.5)
    try:
        if proto == "tcp":
            s.connect((dst, port)); out.append(s.recv(64).decode())
        else:
            s.sendto(b"ping", (dst, port)); out.append(s.recvfrom(64)[0].decode())
    except PermissionError:
        out.append("eperm")
    except socket.timeout:
        out.append("timeout")
    except OSError as e:
        out.append("error")
    finally:
        s.close()
print(json.dumps(out))
"""

WRAPPER = r"""#!{python}
import importlib.machinery, importlib.util, json, sys
sys.dont_write_bytecode = True
loader = importlib.machinery.SourceFileLoader("shadow_killswitch", {tool!r})
spec = importlib.util.spec_from_loader(loader.name, loader)
mod = importlib.util.module_from_spec(spec)
loader.exec_module(mod)
for key, value in {consts!r}.items():
    setattr(mod, key, value)
sys.exit(mod.main(sys.argv[1:]))
"""


BIND = r"""
import socket, sys
proto, port, lo = sys.argv[1], int(sys.argv[2]), sys.argv[3] == "lo"
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM if proto == "udp" else socket.SOCK_STREAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
if lo:
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE, b"lo")
try:
    s.bind(("127.0.0.1", port))
    print("bound")
except PermissionError:
    print("denied")
except OSError as e:
    print("error %d" % e.errno)
"""


def sh(argv, check=True, input=None):
    p = subprocess.run(argv, input=input, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if check and p.returncode != 0:
        raise AssertionError(f"{' '.join(argv)} failed ({p.returncode}): {p.stderr.strip() or p.stdout.strip()}")
    return p


def netns_id():
    return str(os.stat("/proc/self/ns/net").st_ino)


def private_netns():
    outer = os.environ.get(OUTER_NETNS)
    return bool(outer) and netns_id() != outer


def setUpModule():
    if os.environ.get(INNER) != "1" or not private_netns():
        raise unittest.SkipTest("run this file directly as root: it re-executes itself in a private network "
                                "namespace and never runs in the computer's own")


class Lab:
    def __init__(self):
        self.tmp = tempfile.mkdtemp(prefix="shadow-anon-netns.")
        os.chmod(self.tmp, 0o755)
        self.procs = []
        for name, text in (("server.py", SERVER), ("probe.py", PROBE)):
            with open(os.path.join(self.tmp, name), "w") as fh:
                fh.write(text)
            os.chmod(os.path.join(self.tmp, name), 0o644)
        sh(["mount", "-t", "tmpfs", "tmpfs", "/run"])      # private mount namespace: hides nothing outside
        os.makedirs("/run/netns")
        for c in ("ip link set lo up", "ip netns add up",
                  "ip link add veth-h type veth peer name veth-u", "ip link set veth-u netns up",
                  "ip addr add 198.51.100.1/24 dev veth-h", "ip link set veth-h up",
                  "ip route add default via 198.51.100.2"):
            sh(c.split())
        for c in ("ip link set lo up", "ip addr add 198.51.100.2/24 dev veth-u",
                  "ip addr add 10.1.2.3/32 dev veth-u", "ip addr add 10.192.0.5/32 dev veth-u",
                  "ip link set veth-u up"):
            sh(["ip", "netns", "exec", "up"] + c.split())
        self.server("up", "tcp,0.0.0.0,80,up", "udp,0.0.0.0,53,net-dns", "tcp,0.0.0.0,853,net-dot",
                    "udp,0.0.0.0,7001,echo")
        self.server(None, f"tcp,127.0.0.1,{TPROXY_PORT},tp", f"udp,127.0.0.1,{DNS_PORT},tor-dns")

    def server(self, ns, *specs, uid=None):
        argv = [sys.executable, os.path.join(self.tmp, "server.py"), *specs]
        if uid is not None:
            argv = ["setpriv", f"--reuid={uid}", f"--regid={uid}", "--clear-groups"] + argv
        if ns:
            argv = ["ip", "netns", "exec", ns] + argv
        p = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.procs.append(p)
        if p.stdout.readline().strip() != "ready":
            raise AssertionError(f"server {specs} did not start: {p.stderr.read()}")
        return p

    def close(self):
        for p in self.procs:
            p.kill()
            p.communicate()
        shutil.rmtree(self.tmp, True)


LAB = None


class ShadowAnonNetns(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        global LAB
        for tool in ("ip", "iptables", "nft", "setpriv"):
            if not shutil.which(tool):
                raise unittest.SkipTest(f"{tool} is not installed")
        if LAB is None:
            LAB = Lab()

    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="case.", dir=LAB.tmp)
        os.chmod(self.dir, 0o755)
        self.root = os.path.join(self.dir, "root")
        self.bin = os.path.join(self.dir, "bin")
        os.makedirs(self.bin)
        self.tool = t = lib.load_tool()
        lib.redirect_paths(t, self.root)
        ks_consts = {"CONFIG": f"{self.dir}/ks/killswitch.json", "RUN_DIR": f"{self.dir}/ks/run",
                     "CMDLINE": f"{self.dir}/ks/cmdline", "TOR_USER": str(TOR), "OPENVPN_USER": "4244"}
        os.makedirs(f"{self.dir}/ks")
        with open(ks_consts["CMDLINE"], "w") as fh:
            fh.write("BOOT_IMAGE=/vmlinuz ro quiet\n")
        with open(os.path.join(self.bin, "shadow-killswitch"), "w") as fh:
            fh.write(WRAPPER.format(python=sys.executable, tool=KILLSWITCH, consts=ks_consts))
        os.chmod(os.path.join(self.bin, "shadow-killswitch"), 0o755)
        for name in ("systemctl", "nmcli"):                 # nothing may reach the host's services
            with open(os.path.join(self.bin, name), "w") as fh:
                fh.write("#!/bin/sh\nexit 1\n")
            os.chmod(os.path.join(self.bin, name), 0o755)
        self.patches = [
            mock.patch.object(t, "TOR_USER", str(TOR)),
            mock.patch.object(t, "RESOLVED_USER", str(RESOLVE)),
            mock.patch.object(t, "user_exists", lambda name: True),
            mock.patch.object(t, "need_root", lambda: None),
            mock.patch.dict(os.environ, {"PATH": self.bin + ":" + os.environ["PATH"]}),
        ]
        for p in self.patches:
            p.start()
        self.clean()

    def tearDown(self):
        self.clean()
        for p in self.patches:
            p.stop()

    def clean(self):
        for ipt, chain in (("iptables", "SHADOWTOR"), ("iptables", "SHADOWDNS"), ("ip6tables", "SHADOWDNS"),
                           ("iptables", "SHADOWSOCKS"), ("ip6tables", "SHADOWSOCKS")):
            while sh([ipt, "-t", "nat", "-D", "OUTPUT", "-j", chain], check=False).returncode == 0:
                pass
            sh([ipt, "-t", "nat", "-F", chain], check=False)
            sh([ipt, "-t", "nat", "-X", chain], check=False)
        sh(["nft", "-f", "-"], input="table inet shadow_egress {}\ndelete table inet shadow_egress\n")
        sh(["conntrack", "-F"], check=False)

    def quiet(self, fn, *args, **kw):
        lib.check_redirected(self.tool, self.root)
        with redirect_stdout(StringIO()):
            return fn(*args, **kw)

    def probe(self, uid, *checks):
        argv = ["setpriv", f"--reuid={uid}", f"--regid={uid}", "--clear-groups",
                sys.executable, os.path.join(LAB.tmp, "probe.py"), json.dumps(checks)]
        return json.loads(sh(argv).stdout)

    def bind(self, uid, proto, port, lo=False):
        """A user's bind of 127.0.0.1:port with SO_REUSEADDR (and SO_BINDTODEVICE "lo"):
        "bound", "denied" (EACCES: a privileged port) or "error <errno>"."""
        argv = ["setpriv", f"--reuid={uid}", f"--regid={uid}", "--clear-groups",
                sys.executable, "-c", BIND, proto, str(port), "lo" if lo else "-"]
        return sh(argv).stdout.strip()

    def counters(self, chain):
        """{rule text: packets} from iptables -t nat -L -v -x."""
        out = sh(["iptables", "-t", "nat", "-L", chain, "-v", "-n", "-x"]).stdout.splitlines()[2:]
        res = {}
        for line in out:
            tok = line.split()
            res[" ".join(tok[2:])] = int(tok[0])
        return res

    def load_proxy(self):
        t = self.tool
        self.quiet(t._load_chain, t.TPROXY_CHAIN, t.tproxy_rules())

    # ---- tests
    def test_onion_addresses_are_redirected_and_the_lan_is_not(self):
        t = self.tool
        self.assertEqual(self.probe(USER, ["tcp", "10.192.0.5", 80], ["tcp", "10.1.2.3", 80]), ["up", "up"])
        self.load_proxy()
        self.assertTrue(t.transparent_proxy_active())             # parsed from real iptables output
        self.assertTrue(t._tproxy_has_onion_rule())
        res = self.probe(USER, ["tcp", "10.192.0.5", 80], ["tcp", "10.1.2.3", 80], ["tcp", "198.51.100.2", 80])
        self.assertEqual(res, ["tp 10.192.0.5:80", "up", "tp 198.51.100.2:80"])
        self.assertEqual(self.probe(TOR, ["tcp", "198.51.100.2", 80]), ["up"])   # Tor itself is not redirected
        c = self.counters("SHADOWTOR")
        onion = [n for k, n in c.items() if "10.192.0.0/10" in k and k.endswith(f"redir ports {TPROXY_PORT}")]
        ten = [n for k, n in c.items() if k.startswith("RETURN") and "10.0.0.0/8" in k]
        self.assertEqual(onion, [1])
        self.assertEqual(ten, [1])

    def test_the_proxy_alone_lets_udp_out(self):
        # Why the transparent proxy engages the kill switch: it only redirects TCP and DNS.
        self.load_proxy()
        self.assertEqual(self.probe(USER, ["udp", "198.51.100.2", 7001]), ["ping"])

    def test_dns_redirect_exempts_only_tor(self):
        """systemd-resolved is not exempt: its queries to a network's own server
        (names under that network's search domains) go to Tor, and its
        DNS-over-TLS to such a server is refused (sent to a closed privileged
        port: resolved falls back to port 53, redirected). Before, resolved
        reached the network's server directly, in plain text or over TLS."""
        t = self.tool
        self.quiet(t._dns_redirect_on)
        self.assertEqual(self.probe(USER, ["udp", "198.51.100.2", 53], ["udp", "127.0.0.53", 53]),
                         ["tor-dns", "tor-dns"])
        self.assertEqual(self.probe(RESOLVE, ["udp", "198.51.100.2", 53], ["tcp", "198.51.100.2", 853]),
                         ["tor-dns", "error"])
        self.assertEqual(self.probe(TOR, ["udp", "198.51.100.2", 53]), ["net-dns"])
        self.assertEqual(self.probe(USER, ["tcp", "198.51.100.2", 853]), ["net-dot"])    # only resolved's DoT
        c = self.counters("SHADOWDNS")
        self.assertEqual([n for k, n in c.items() if "owner UID match 4245" in k and "dpt:853" in k], [1])
        self.quiet(t._nat_remove, t.DNS_CHAIN)
        # positive controls
        self.assertEqual(self.probe(USER, ["udp", "198.51.100.2", 53]), ["net-dns"])
        self.assertEqual(self.probe(RESOLVE, ["udp", "198.51.100.2", 53], ["tcp", "198.51.100.2", 853]),
                         ["net-dns", "net-dot"])

    def test_dns_over_tcp_is_refused(self):
        """Regression (an earlier fix): Tor's DNSPort is UDP only, and the
        redirect sent resolved's DNS-over-TLS (opportunistic: no certificate
        check) and every TCP port-53 query to its TCP side, and resolved
        itself retries Tor's DNSPort over TCP (truncated answer, or no UDP
        answer while Tor is not ready): on 5353 any user could listen there
        and answer for the system. TCP DNS is refused, on a port only root
        may bind; UDP goes to Tor. (Tor's DNSPort itself is privileged now,
        so its TCP side is out of a user's reach too.)"""
        t = self.tool
        for port in (int(t.CLOSED_PORT), DNS_PORT):
            self.assertEqual(self.bind(USER, "tcp", port), "denied")
        self.assertEqual(self.bind(USER, "tcp", OLD_DNS_PORT), "bound")          # control: the old port
        self.quiet(t._dns_redirect_on)
        self.assertEqual(self.probe(RESOLVE, ["tcp", "198.51.100.2", 853], ["tcp", "127.0.0.1", DNS_PORT],
                                    ["tcp", "198.51.100.2", 53], ["udp", "198.51.100.2", 53]),
                         ["error", "error", "error", "tor-dns"])
        self.assertEqual(self.probe(OTHER, ["tcp", "198.51.100.2", 53], ["tcp", "127.0.0.53", 53],
                                    ["udp", "127.0.0.53", 53]), ["error", "error", "tor-dns"])
        c = self.counters("SHADOWDNS")
        closed = {k: n for k, n in c.items() if k.endswith(f"redir ports {t.CLOSED_PORT}")}
        self.assertEqual(sorted(closed.values()), [1, 1, 3], closed)   # 853, resolved's DNSPort, port 53
        self.assertEqual([k for k in c if k.endswith(f"redir ports {DNS_PORT}") and "udp dpt:53" not in k], [])
        self.quiet(t._nat_remove, t.DNS_CHAIN)
        # the transparent proxy (Ghost Mode) refuses TCP port 53 the same way
        self.load_proxy()
        self.assertEqual(self.probe(OTHER, ["tcp", "198.51.100.2", 53], ["udp", "198.51.100.2", 53]),
                         ["error", "tor-dns"])

    def test_no_user_can_take_tors_ports(self):
        """Regression: Tor's DNSPort was UDP 127.0.0.1:5353 and its TransPort
        TCP 127.0.0.1:9040. Tor sets SO_REUSEADDR on its DNSPort, so while
        Tor ran any user's socket with SO_REUSEADDR bound the same address
        (with SO_BINDTODEVICE "lo", which needs no privilege, it wins every
        datagram) and got every redirected query and resolved's own; and a
        user who bound the TransPort while Tor restarted kept it and got
        every program's connections. Both ports are below 53 now: a user's
        bind is refused (EACCES), SO_REUSEADDR or not, and every query and
        connection reaches Tor. The controls run the same attack against
        the old ports, with shadow-anon's own rules pointed at them."""
        t = self.tool

        def old(rules):
            ports = {t.DNS_PORT: str(OLD_DNS_PORT), t.TPROXY_PORT: str(OLD_TPROXY_PORT)}
            return [[ports.get(x, x) if i and r[i - 1] == "--to-ports" else x for i, x in enumerate(r)]
                    for r in rules]
        # Control: the old ports. A stand-in for Tor (root, SO_REUSEADDR) holds UDP 5353.
        procs = [LAB.server(None, f"udp,127.0.0.1,{OLD_DNS_PORT},tor-dns")]
        try:
            procs.append(LAB.server(None, f"udp,127.0.0.1,{OLD_DNS_PORT},rogue,lo", uid=USER))   # co-binds
            procs.append(LAB.server(None, f"tcp,127.0.0.1,{OLD_TPROXY_PORT},rogue", uid=USER))  # Tor not there
            self.quiet(t._load_chain, t.DNS_CHAIN, old(t.dns_rules()))
            self.assertEqual(self.probe(OTHER, ["udp", "127.0.0.53", 53], ["udp", "198.51.100.2", 53]),
                             ["rogue", "rogue"])
            self.assertEqual(self.probe(RESOLVE, ["udp", "127.0.0.1", OLD_DNS_PORT]), ["rogue"])
            self.quiet(t._nat_remove, t.DNS_CHAIN)
            self.quiet(t._load_chain, t.TPROXY_CHAIN, old(t.tproxy_rules()))
            self.assertEqual(self.probe(OTHER, ["tcp", "198.51.100.2", 80], ["udp", "127.0.0.53", 53]),
                             ["rogue", "rogue"])
            self.quiet(t._nat_remove, t.TPROXY_CHAIN)
        finally:
            for p in procs:
                p.kill()
                p.communicate()
                LAB.procs.remove(p)
        # Tor's ports now: refused to a user, with or without a bound device
        # (Tor's stand-ins hold them, as root).
        for proto, port in (("udp", DNS_PORT), ("tcp", TPROXY_PORT), ("tcp", DNS_PORT), ("udp", TPROXY_PORT)):
            for lo in (False, True):
                self.assertEqual(self.bind(USER, proto, port, lo), "denied", (proto, port, lo))
        self.assertEqual(self.bind(USER, "udp", OLD_DNS_PORT, True), "bound")          # control
        # Tor's SOCKS port too (Tor exits when it cannot bind any of its
        # ports: a user who held Debian's 9050 while Tor was stopped kept it
        # from opening the two above, and got every SOCKS client).
        for lo in (False, True):
            self.assertEqual(self.bind(USER, "tcp", int(t.TOR_SOCKS_PORT), lo), "denied", lo)
        # control: Debian's 9050 stays bindable; what connects there goes to
        # Tor's port all the same (test_tors_old_socks_port_reaches_tor_not_a_user)
        self.assertEqual(self.bind(USER, "tcp", int(t.OLD_SOCKS_PORT)), "bound")
        self.quiet(t._dns_redirect_on)
        self.assertEqual(self.probe(OTHER, ["udp", "127.0.0.53", 53], ["udp", "198.51.100.2", 53]),
                         ["tor-dns", "tor-dns"])
        self.assertEqual(self.probe(RESOLVE, ["udp", "127.0.0.1", DNS_PORT]), ["tor-dns"])
        self.quiet(t._nat_remove, t.DNS_CHAIN)
        self.load_proxy()
        self.assertEqual(self.probe(OTHER, ["tcp", "198.51.100.2", 80], ["udp", "127.0.0.53", 53]),
                         ["tp 198.51.100.2:80", "tor-dns"])

    def test_tors_old_socks_port_reaches_tor_not_a_user(self):
        """Regression (d2-01): with Tor's SOCKS port moved to 39, nothing of
        Tor's holds Debian's 9050, so any user may bind it at any time, also
        while Tor runs; programs that use 9050 with no file to repoint
        (tor-resolve, sqlmap --tor, a user's scripts) then handed that user
        every name and connection. SHADOWSOCKS sends every new TCP
        connection to 9050 on a loopback address to Tor's port 39: the
        user's listener (here on 0.0.0.0:9050, so it would catch any address
        the redirect missed) gets nothing, and while Tor is not running the
        connection is refused. Before the chain is loaded, the same clients
        reach the user (control)."""
        t = self.tool
        socks, old = int(t.TOR_SOCKS_PORT), int(t.OLD_SOCKS_PORT)
        procs = [LAB.server(None, f"tcp,0.0.0.0,{old},rogue", uid=USER)]          # bound while "Tor" runs
        try:
            procs.append(LAB.server(None, f"tcp,127.0.0.1,{socks},tor-socks"))     # Tor's stand-in (root)
            clients = (["tcp", "127.0.0.1", old], ["tcp", "127.0.1.1", old], ["tcp", "127.0.0.1", socks])
            self.assertEqual(self.probe(OTHER, *clients), ["rogue", "rogue", "tor-socks"])     # control
            self.assertTrue(self.quiet(t.socks_redirect_on))
            self.assertTrue(t.socks_redirect_active())                # parsed from real iptables output
            self.assertFalse(self.quiet(t.socks_redirect_on))         # in place: not emptied and refilled
            self.assertEqual(sh(["iptables", "-t", "nat", "-S", "SHADOWSOCKS"]).stdout.splitlines(),
                             ["-N SHADOWSOCKS", f"-A SHADOWSOCKS -d 127.0.0.0/8 -p tcp -m tcp --dport {old} "
                                            f"-j REDIRECT --to-ports {socks}"])
            # d3-02: [::1]:9050 is refused (closed port 1), so "localhost:9050"
            # clients, which try ::1 first, move on to 127.0.0.1:9050. Real
            # ip6tables-nft; no IPv6 packets (this kernel has IPv6 disabled).
            self.assertEqual(sh(["ip6tables", "-t", "nat", "-S", "SHADOWSOCKS"]).stdout.splitlines(),
                             ["-N SHADOWSOCKS", f"-A SHADOWSOCKS -d ::1/128 -p tcp -m tcp --dport {old} "
                                            "-j REDIRECT --to-ports 1"])
            self.assertIn("-A OUTPUT -j SHADOWSOCKS", sh(["ip6tables", "-t", "nat", "-S", "OUTPUT"]).stdout.splitlines())
            sh(["ip6tables", "-t", "nat", "-D", "OUTPUT", "-j", "SHADOWSOCKS"])
            self.assertFalse(t.socks_redirect_active())
            self.assertTrue(self.quiet(t.socks_redirect_on))          # the IPv6 side only
            self.assertTrue(t.socks_redirect_active())
            self.assertEqual(self.probe(OTHER, *clients), ["tor-socks"] * 3)
            self.assertEqual(self.probe(TOR, ["tcp", "127.0.0.1", old]), ["tor-socks"])
            self.assertEqual([n for k, n in self.counters("SHADOWSOCKS").items()
                              if k.endswith(f"redir ports {socks}")], [3])
            # The other chains leave it alone, in either order of the hooks.
            self.load_proxy()
            self.quiet(t._dns_redirect_on)
            self.assertEqual(sh(["iptables", "-t", "nat", "-S", "OUTPUT"]).stdout.splitlines()[1:],
                             ["-A OUTPUT -j SHADOWDNS", "-A OUTPUT -j SHADOWTOR", "-A OUTPUT -j SHADOWSOCKS"])
            self.assertEqual(self.probe(OTHER, ["tcp", "127.0.0.1", old], ["tcp", "198.51.100.2", 80]),
                             ["tor-socks", "tp 198.51.100.2:80"])
            self.quiet(t._nat_remove, t.TPROXY_CHAIN)
            self.quiet(t._dns_redirect_off)
            # Tor not running: refused (fail closed), while the user still listens.
            procs[1].kill()
            procs[1].communicate()
            LAB.procs.remove(procs[1])
            procs.pop(1)
            self.assertEqual(self.probe(OTHER, ["tcp", "127.0.0.1", old], ["tcp", "127.0.1.1", old]),
                             ["error", "error"])
            self.assertEqual(self.probe(OTHER, ["tcp", "198.51.100.1", old]), ["rogue"])   # still listening
        finally:
            for p in procs:
                p.kill()
                p.communicate()
                LAB.procs.remove(p)

    def test_dns_redirect_has_an_ipv6_side_that_refuses(self):
        """Regression (d1-02): SHADOWDNS was IPv4 only, so DNS to a network's
        IPv6 server left unchanged. Its IPv6 side refuses every port-53 query
        and resolved's DNS-over-TLS (port 1 on ::1). Real ip6tables-nft here;
        no IPv6 packets (this kernel runs with IPv6 disabled)."""
        t = self.tool
        self.quiet(t._dns_redirect_on)
        out = sh(["ip6tables", "-t", "nat", "-S", "SHADOWDNS"]).stdout.splitlines()
        self.assertEqual(out, ["-N SHADOWDNS",
                               f"-A SHADOWDNS -m owner --uid-owner {TOR} -j RETURN",
                               f"-A SHADOWDNS -p tcp -m owner --uid-owner {RESOLVE} -m tcp --dport 853 "
                               "-j REDIRECT --to-ports 1",
                               "-A SHADOWDNS -p udp -m udp --dport 53 -j REDIRECT --to-ports 1",
                               "-A SHADOWDNS -p tcp -m tcp --dport 53 -j REDIRECT --to-ports 1"])
        self.assertIn("-A OUTPUT -j SHADOWDNS", sh(["ip6tables", "-t", "nat", "-S", "OUTPUT"]).stdout.splitlines())
        self.assertTrue(t._dns_redirect_hooked())                    # parsed from the real output
        self.assertFalse([r for r in t._nat_rules(t.DNS_CHAIN, t.IP6T) if r.endswith(f"--to-ports {DNS_PORT}")])
        sh(["ip6tables", "-t", "nat", "-D", "OUTPUT", "-j", "SHADOWDNS"])
        self.assertFalse(t._dns_redirect_hooked())
        self.assertTrue(t._dns6_missing())
        self.quiet(t._dns_redirect_off)
        for ipt in ("iptables", "ip6tables"):
            self.assertNotEqual(sh([ipt, "-t", "nat", "-S", "SHADOWDNS"], check=False).returncode, 0, ipt)

    def test_kill_switch_delegate_with_the_real_tool(self):
        t = self.tool
        self.quiet(t.kill_switch_on, by="ghost")
        st = t.killswitch_status()
        self.assertTrue(t.killswitch_tor_active(st), st)
        self.assertTrue(t.load_state()["kill_switch"])
        self.assertTrue(st["runtime_override"])
        self.assertEqual(self.probe(TOR, ["tcp", "198.51.100.2", 80]), ["up"])           # positive control
        for res in self.probe(USER, ["tcp", "198.51.100.2", 80], ["udp", "198.51.100.2", 7001]):
            self.assertIn(res, BLOCKED)
        self.load_proxy()
        # the proxy's UDP leak is closed
        self.assertIn(self.probe(USER, ["udp", "198.51.100.2", 7001])[0], BLOCKED)
        self.quiet(t.kill_switch_off)                    # also ends the proxy
        st = t.killswitch_status()
        self.assertEqual((st["mode"], st["loaded"], st["persistent_mode"]), ("off", False, "off"))
        self.assertFalse(t.transparent_proxy_active())
        self.assertNotEqual(sh(["nft", "list", "table", "inet", "shadow_egress"], check=False).returncode, 0)
        self.assertEqual(self.probe(USER, ["tcp", "198.51.100.2", 80]), ["up"])

    def test_hide_me_kill_switch_blocks_a_lan_allowed_before(self):
        """Regression: "set tor" without a flag kept a LAN setting saved
        earlier ("vpn --lan"), so Hide Me's Tor only let the private ranges
        out directly. Hide Me sets --no-lan and puts the LAN setting back."""
        t = self.tool
        sh(["shadow-killswitch", "set", "vpn", "--lan"])
        self.assertEqual(self.probe(USER, ["udp", "10.1.2.3", 7001]), ["ping"])            # positive control
        self.quiet(t.kill_switch_on, by="ghost")
        st = t.killswitch_status()
        self.assertEqual((st["mode"], st["lan"], st["loaded"]), ("tor", False, True))
        self.assertTrue(t.killswitch_tor_active(st))
        for res in self.probe(USER, ["udp", "10.1.2.3", 7001], ["tcp", "10.1.2.3", 80]):
            self.assertIn(res, BLOCKED)
        self.assertEqual(self.probe(TOR, ["tcp", "10.1.2.3", 80]), ["up"])
        self.quiet(t.kill_switch_off)
        st = t.killswitch_status()
        self.assertEqual((st["mode"], st["lan"], st["persistent_mode"]), ("vpn", True, "vpn"))
        self.assertEqual(self.probe(USER, ["udp", "10.1.2.3", 7001]), ["ping"])

    def test_ghost_mode_tcp_through_the_proxy_passes_the_kill_switch(self):
        """Regression: a packet that nat OUTPUT redirects to 127.0.0.1 keeps the
        output interface it was routed to first (state->out is fixed before the
        nat hook re-routes), so shadow_egress's 'oif "lo" accept' alone does not
        match it. shadow-killswitch accepts 'ip daddr 127.0.0.0/8' and
        'ip6 daddr ::1' for that reason; without them Ghost Mode has no TCP.
        """
        t = self.tool
        self.quiet(t.kill_switch_on, by="ghost")
        self.load_proxy()
        self.assertEqual(self.probe(USER, ["tcp", "198.51.100.2", 80], ["tcp", "10.192.0.5", 80]),
                         ["tp 198.51.100.2:80", "tp 10.192.0.5:80"])


def main():
    if os.environ.get(INNER) == "1":
        if not private_netns():
            print("refusing to run: not in a private network namespace", file=sys.stderr)
            return 2
        try:
            prog = unittest.main(exit=False, module="__main__")
            return 0 if prog.result.wasSuccessful() else 1
        finally:
            if LAB is not None:
                LAB.close()
    # 77: skipped (tests/run-suites.sh), never a pass.
    if os.geteuid() != 0:
        print("SKIP: needs root (unshare --net, iptables, nftables)", file=sys.stderr)
        return 77
    if not shutil.which("unshare"):
        print("SKIP: unshare is not installed", file=sys.stderr)
        return 77
    env = dict(os.environ, **{INNER: "1", OUTER_NETNS: netns_id(), "PYTHONDONTWRITEBYTECODE": "1"})
    return subprocess.call(["unshare", "--net", "--mount", "--fork", sys.executable, os.path.abspath(__file__),
                            *sys.argv[1:]], env=env)


if __name__ == "__main__":
    sys.exit(main())
