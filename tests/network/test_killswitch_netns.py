#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Kill switch and panic with real nftables and real packets.

    sudo python3 tests/network/test_killswitch_netns.py [-v]

Needs root, unshare, nft, ip, setpriv and conntrack (the conntrack test is
skipped without it; without root or unshare the program exits 77, skipped). The program re-executes itself with
"unshare --net --mount --fork": everything below happens in a private,
throw-away network namespace (checked before anything is changed) with /run
replaced by a tmpfs, so the computer's own network is never touched.

Topology, all addresses from the documentation ranges (RFC 5737), plus one
private range for the LAN test and the benchmarking range (RFC 2544) for the
bridged guests:

    host (the private namespace, where the kill switch runs)
      veth-h  198.51.100.1/24, 10.77.0.1/24, 10.192.0.1/10  ──  "up" (upstream)
                                                                198.51.100.2, 10.77.0.2, 10.192.0.9
      wg0     203.0.113.1/25     (a layer-3 tunnel: two TUN devices, type "none" like
                                  WireGuard, joined by a relay thread)  ──  "vpn" wgp 203.0.113.2
      office0 203.0.113.129/25   (the same, known only through NetworkManager) ── "vpn" 203.0.113.130
      tun0    192.0.2.129/25     (a real tun device; the test reads what goes out of it)
      vm-h    192.0.2.1/25   ──  "vm" 192.0.2.10 (a virtual machine routed through the host)
      br0     a bridge without an address, ports:
                brup  (macvlan passthru on brup-h, the stand-in for a network card) ── "up" 198.18.0.2/24
                bvm-h   ──  "bvm"  198.18.0.50/24  (a virtual machine bridged to the network card)
                bvm2-h  ──  "bvm2" 198.18.0.60/24  (a second guest on the same bridge)
      The namespace's bridge-nf-call-iptables/ip6tables/arptables are 0, as on a
      Debian system without br_netfilter: bridged frames never reach the inet hooks.
      127.0.0.1:26 (UDP) and :40 (TCP) stand in for Tor's DNSPort and TransPort
      (shadow-anon's DNS_PORT and TPROXY_PORT, read from the tool).

Every echo server answers "ping" with "ping". The probes run as different
users with setpriv: 4242 (an ordinary user), 4243 (stands in for
debian-tor), 4244 (stands in for nm-openvpn) and root. The tools are run as
programs, with their path constants pointed at a temporary directory and the
Tor/OpenVPN users set to those uids. Each test has a positive control: a
packet that must pass does pass. Counters are read with "nft -j list table".

Not covered here (see docs/limitations.md): IPv6 packets (this test kernel
runs with ipv6.disable=1; the IPv6 rules are only checked to load), real
WireGuard and OpenVPN, NetworkManager itself (a fake nmcli answers), boot
ordering.
"""
import fcntl
import json
import os
import re
import select
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
FW = os.path.join(ROOT, "security", "firewall")
KILLSWITCH = os.path.join(FW, "shadow-killswitch")
PANIC = os.path.join(FW, "shadow-panic")
NFTABLES_CONF = os.path.join(FW, "nftables.conf")
DROPIN = os.path.join(FW, "systemd", "nftables.service.d", "shadow.conf")
WATCH_SERVICE = os.path.join(FW, "systemd", "shadow-killswitch-watch.service")
INNER = "SHADOW_KILLSWITCH_NETNS_INNER"
USER, TOR, OPENVPN = 4242, 4243, 4244
sys.path.insert(0, os.path.join(ROOT, "tests", "security"))
import shadow_anon_testlib as anon_lib  # noqa: E402
ANON = anon_lib.load_tool()                 # Hide Me (shadow-anon): its rules and Tor's ports
DNS_PORT, TPROXY_PORT, CLOSED_PORT = int(ANON.DNS_PORT), int(ANON.TPROXY_PORT), int(ANON.CLOSED_PORT)


def hide_me_nft(tor_uid):
    """shadow-anon's SHADOWTOR chain (tproxy_rules(): iptables arguments, in their order) as
    an nft table hooked into nat output, with Tor's user as tor_uid. An argument without a
    translation here fails the test instead of leaving out part of Hide Me's rules."""
    lines = []
    for args in ANON.tproxy_rules():
        a, nft, i, proto = list(args), [], 0, None
        while i < len(a):
            if a[i:i + 3] == ["-m", "owner", "--uid-owner"] and a[i + 3] == ANON.TOR_USER:
                nft.append(f"meta skuid {tor_uid}")
                i += 4
            elif a[i] == "-d":
                nft.append(f"ip daddr {a[i + 1]}")
                i += 2
            elif a[i] == "-p" and a[i + 1] in ("tcp", "udp"):
                proto = a[i + 1]
                nft.append(f"meta l4proto {proto}")
                i += 2
            elif a[i] == "--dport" and proto:
                nft.append(f"{proto} dport {a[i + 1]}")
                i += 2
            elif a[i] == "--syn" and proto == "tcp":
                nft.append("tcp flags & (fin | syn | rst | ack) == syn")
                i += 1
            elif a[i:i + 2] == ["-j", "RETURN"]:
                nft.append("return")
                i += 2
            elif a[i:i + 3] == ["-j", "REDIRECT", "--to-ports"]:
                nft.append(f"redirect to :{a[i + 3]}")
                i += 4
            else:
                raise AssertionError(f"no nft translation for {a[i:]} in shadow-anon's rule {args}")
        lines.append("\t\t" + " ".join(nft) + "\n")
    return ("table ip hide_me_test {\n\tchain output {\n\t\ttype nat hook output priority -100; policy accept;\n"
            + "".join(lines) + "\t}\n}\n")

# Runs a tool with its constants replaced (no production code path is skipped).
BOOTSTRAP = r"""
import importlib.machinery, importlib.util, json, sys
sys.dont_write_bytecode = True
loader = importlib.machinery.SourceFileLoader("tool_under_test", sys.argv[1])
spec = importlib.util.spec_from_loader(loader.name, loader)
mod = importlib.util.module_from_spec(spec)
loader.exec_module(mod)
for key, value in json.loads(sys.argv[2]).items():
    setattr(mod, key, value)
sys.exit(mod.main(sys.argv[3:]))
"""

SERVER = r"""
import socket, sys, threading
def tcp(addr, port):
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind((addr, port)); s.listen(64)
    def serve(c):
        with c:
            while True:
                d = c.recv(64)
                if not d:
                    return
                c.sendall(d)
    while True:
        c, _ = s.accept()
        threading.Thread(target=serve, args=(c,), daemon=True).start()
def udp(addr, port):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind((addr, port))
    while True:
        d, peer = s.recvfrom(64)
        s.sendto(d, peer)
threads = []
for spec in sys.argv[1:]:
    proto, addr, port = spec.split(":")
    t = threading.Thread(target=tcp if proto == "tcp" else udp, args=(addr, int(port)), daemon=True)
    t.start()
    threads.append(t)
import time; time.sleep(0.3)
print("ready", flush=True)
for t in threads:
    t.join()
"""

PROBE = r"""
import errno, json, socket, sys, threading
def check(c):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM if c["proto"] == "tcp" else socket.SOCK_DGRAM)
    s.settimeout(c.get("timeout", 1.5))
    try:
        if c.get("bind_dev"):          # SO_BINDTODEVICE: no privilege needed since Linux 5.7
            s.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE, c["bind_dev"].encode())
        if c.get("sport"):
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind(("0.0.0.0", c["sport"]))
        if c["proto"] == "tcp":
            s.connect((c["dst"], c["port"]))
            s.sendall(b"ping")
            data = s.recv(16)
        else:
            s.sendto(b"ping", (c["dst"], c["port"]))
            if c.get("noreply"):
                return "sent"
            data, _ = s.recvfrom(16)
        return "ok" if data == b"ping" else "bad"
    except PermissionError:
        return "eperm"
    except socket.timeout:
        return "timeout"
    except OSError as e:
        return "error:" + errno.errorcode.get(e.errno, str(e.errno))
    finally:
        s.close()
checks = json.loads(sys.argv[1])
out = [None] * len(checks)
def run(i):
    out[i] = check(checks[i])
ts = [threading.Thread(target=run, args=(i,)) for i in range(len(checks))]
[t.start() for t in ts]
[t.join() for t in ts]
print(json.dumps(out))
"""

CLIENT = r"""
import errno, json, os, socket, sys, time
dst, port, go = sys.argv[1], int(sys.argv[2]), sys.argv[3]
s = socket.create_connection((dst, port), timeout=3)
s.sendall(b"ping"); assert s.recv(16) == b"ping"
print(json.dumps({"sport": s.getsockname()[1]}), flush=True)
while not os.path.exists(go):
    time.sleep(0.05)
try:
    s.settimeout(3); s.sendall(b"ping"); d = s.recv(16)
    print(json.dumps("ok" if d == b"ping" else "closed"), flush=True)
except OSError as e:
    print(json.dumps("error:" + errno.errorcode.get(e.errno, str(e.errno))), flush=True)
"""

FAKE_NMCLI = """#!/bin/sh
case "$*" in
    "-t -f UUID,TYPE,DEVICE connection show --active") echo "u1:wireguard:office0" ;;
    "-t -f UUID,TYPE connection show") echo "u1:wireguard" ;;
    *) exit 2 ;;
esac
"""
FAKE_WG = """#!/bin/sh
if [ "$*" = "show office0 endpoints" ]; then printf 'PUBKEY=\\t198.51.100.2:7003\\n'; exit 0; fi
exit 1
"""


TUNSETIFF, IFF_TUN, IFF_TAP, IFF_NO_PI = 0x400454ca, 0x0001, 0x0002, 0x1000


def open_tun(name, tap=False):
    """A TUN (layer 3, device type "none") or TAP device of this thread's network namespace."""
    fd = os.open("/dev/net/tun", os.O_RDWR | os.O_NONBLOCK)
    fcntl.ioctl(fd, TUNSETIFF, struct.pack("16sH", name.encode(), (IFF_TAP if tap else IFF_TUN) | IFF_NO_PI))
    return fd


def relay(a, b):
    """Copy every packet between the two TUN devices (until their descriptors are closed)."""
    while True:
        try:
            ready = select.select([a, b], [], [], 0.5)[0]
        except (OSError, ValueError):
            return
        for src in ready:
            dst = b if src == a else a
            while True:
                try:
                    pkt = os.read(src, 65535)
                except BlockingIOError:
                    break
                except OSError:
                    return
                try:
                    os.write(dst, pkt)
                except OSError:
                    pass                        # the other side is down: the packet is lost


def sh(argv, check=True, input=None, env=None):
    p = subprocess.run(argv, input=input, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
    if check and p.returncode != 0:
        raise AssertionError(f"{' '.join(argv)} failed ({p.returncode}): {p.stderr.strip() or p.stdout.strip()}")
    return p


OUTER_NETNS = "SHADOW_KILLSWITCH_OUTER_NETNS"


def netns_id():
    return str(os.stat("/proc/self/ns/net").st_ino)


def private_netns():
    """True only in a network namespace other than the one this program was started in."""
    outer = os.environ.get(OUTER_NETNS)
    return bool(outer) and netns_id() != outer


def setUpModule():
    if os.environ.get(INNER) != "1" or not private_netns():
        raise unittest.SkipTest("run this file directly as root: it re-executes itself in a private network "
                                "namespace and never runs in the computer's own")


class Lab:
    """The topology; built once."""

    def __init__(self):
        self.tmp = tempfile.mkdtemp(prefix="shadow-ks-netns.")
        os.chmod(self.tmp, 0o755)
        self.procs = []
        for name, text in (("server.py", SERVER), ("probe.py", PROBE), ("client.py", CLIENT)):
            with open(os.path.join(self.tmp, name), "w") as fh:
                fh.write(text)
            os.chmod(os.path.join(self.tmp, name), 0o644)
        self.bin = os.path.join(self.tmp, "bin")
        os.makedirs(self.bin)
        for name, text in (("nmcli", FAKE_NMCLI), ("wg", FAKE_WG)):
            with open(os.path.join(self.bin, name), "w") as fh:
                fh.write(text)
            os.chmod(os.path.join(self.bin, name), 0o755)
        sh(["mount", "-t", "tmpfs", "tmpfs", "/run"])          # private mount namespace: hides nothing outside
        os.makedirs("/run/netns")
        cmds = [
            "ip link set lo up",
            "ip netns add up", "ip netns add vpn", "ip netns add vm",
            "ip link add veth-h type veth peer name veth-u", "ip link set veth-u netns up",
            "ip addr add 198.51.100.1/24 dev veth-h", "ip addr add 10.77.0.1/24 dev veth-h",
            "ip addr add 10.192.0.1/10 dev veth-h", "ip link set veth-h up",
            "ip link add vm-h type veth peer name vm-v", "ip link set vm-v netns vm",
            "ip addr add 192.0.2.1/25 dev vm-h", "ip link set vm-h up",
            "ip tuntap add tun0 mode tun",
            # bridged guests: br0 joins the "network card" (a macvlan, not a veth: a local
            # guest's port is a veth or a TAP, which the kill switch lets frames go to) and
            # two guests
            "ip netns add bvm", "ip netns add bvm2",
            "ip link add br0 type bridge", "ip link set br0 up",
            "ip link add brup-h type veth peer name brup-u", "ip link set brup-u netns up", "ip link set brup-h up",
            "ip link add brup link brup-h type macvlan mode passthru", "ip link set brup master br0",
            "ip link set brup up",
            "ip link add bvm-h type veth peer name bvm-v", "ip link set bvm-v netns bvm",
            "ip link set bvm-h master br0", "ip link set bvm-h up",
            "ip link add bvm2-h type veth peer name bvm2-v", "ip link set bvm2-v netns bvm2",
            "ip link set bvm2-h master br0", "ip link set bvm2-h up",
        ]
        ns_cmds = {
            "up": ["ip link set lo up", "ip addr add 198.51.100.2/24 dev veth-u", "ip addr add 10.77.0.2/24 dev veth-u",
                   "ip addr add 10.192.0.9/10 dev veth-u", "ip link set veth-u up",
                   "ip route add 192.0.2.0/25 via 198.51.100.1",
                   "ip addr add 198.18.0.2/24 dev brup-u", "ip link set brup-u up"],
            "vpn": ["ip link set lo up", "ip addr add 203.0.113.2/25 dev wgp", "ip link set wgp up",
                    "ip addr add 203.0.113.130/25 dev offp", "ip link set offp up",
                    "ip route add 192.0.2.0/25 via 203.0.113.1"],
            "vm": ["ip link set lo up", "ip addr add 192.0.2.10/25 dev vm-v", "ip link set vm-v up",
                   "ip route add default via 192.0.2.1"],
            "bvm": ["ip link set lo up", "ip addr add 198.18.0.50/24 dev bvm-v", "ip link set bvm-v up"],
            "bvm2": ["ip link set lo up", "ip addr add 198.18.0.60/24 dev bvm2-v", "ip link set bvm2-v up"],
        }
        for c in cmds:
            sh(c.split())
        # Layer-3 tunnels to "vpn": TUN devices (device type "none", as WireGuard's), relayed.
        self.tun_fds = []
        for name, peer in (("wg0", "wgp"), ("office0", "offp")):
            self.tun_pair(name, "vpn", peer)
        sh("ip addr add 203.0.113.1/25 dev wg0".split())
        sh("ip link set wg0 up".split())
        sh("ip addr add 203.0.113.129/25 dev office0".split())
        sh("ip link set office0 up".split())
        for ns, lst in ns_cmds.items():
            for c in lst:
                sh(["ip", "netns", "exec", ns] + c.split())
        for name, value in (("ipv4/ip_forward", "1"),            # this namespace's own settings
                            ("bridge/bridge-nf-call-iptables", "0"), ("bridge/bridge-nf-call-ip6tables", "0"),
                            ("bridge/bridge-nf-call-arptables", "0")):
            if os.path.exists(f"/proc/sys/net/{name}"):
                with open(f"/proc/sys/net/{name}", "w") as fh:
                    fh.write(value + "\n")
        # tun0 with this process as its reader, so what goes out of it can be checked
        self.tun = os.open("/dev/net/tun", os.O_RDWR | os.O_NONBLOCK)
        fcntl.ioctl(self.tun, 0x400454ca, struct.pack("16sH", b"tun0", 0x0001 | 0x1000))   # TUNSETIFF, TUN|NO_PI
        sh("ip addr add 192.0.2.129/25 dev tun0".split())
        sh("ip link set tun0 up".split())
        self.server("up", "tcp:0.0.0.0:7000", "udp:0.0.0.0:7001", "udp:0.0.0.0:7002", "udp:0.0.0.0:7003",
                    "udp:0.0.0.0:67", "udp:0.0.0.0:69")
        self.server("vpn", "tcp:0.0.0.0:7000")
        self.server("bvm2", "tcp:0.0.0.0:7000")
        # loopback, the inbound test, and stand-ins for Tor's DNSPort and TransPort
        self.server(None, "tcp:127.0.0.1:7100", "tcp:198.51.100.1:7200", f"udp:127.0.0.1:{DNS_PORT}",
                    f"tcp:127.0.0.1:{TPROXY_PORT}")

    def tun_pair(self, name, ns, peer):
        """TUN device name here and peer in network namespace ns, joined by a relay thread."""
        here = open_tun(name)
        out = {}

        def open_there():                       # setns() moves only this thread
            fd = os.open(f"/run/netns/{ns}", os.O_RDONLY)
            try:
                os.setns(fd, os.CLONE_NEWNET)
            finally:
                os.close(fd)
            out["fd"] = open_tun(peer)
        t = threading.Thread(target=open_there)
        t.start()
        t.join()
        self.tun_fds += [here, out["fd"]]
        threading.Thread(target=relay, args=(here, out["fd"]), daemon=True).start()

    def server(self, ns, *specs):
        argv = [sys.executable, os.path.join(self.tmp, "server.py"), *specs]
        if ns is not None:
            argv = ["ip", "netns", "exec", ns] + argv
        p = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        self.procs.append(p)
        if p.stdout.readline().strip() != "ready":
            raise AssertionError(f"echo server {specs} did not start: {p.stderr.read()}")

    def close(self):
        for p in self.procs:
            p.kill()
            p.communicate()
        os.close(self.tun)
        for fd in self.tun_fds:
            os.close(fd)
        shutil.rmtree(self.tmp, True)

    def drain_tun(self):
        while select.select([self.tun], [], [], 0)[0]:
            try:
                os.read(self.tun, 65535)
            except BlockingIOError:
                break

    def tun_packets(self, wait=1.0):
        pkts, end = [], time.time() + wait
        while time.time() < end:
            if select.select([self.tun], [], [], 0.1)[0]:
                try:
                    pkts.append(os.read(self.tun, 65535))
                except BlockingIOError:
                    pass
        return pkts


LAB = None


class NetnsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        global LAB
        for tool in ("ip", "nft", "setpriv"):
            if not shutil.which(tool):
                raise unittest.SkipTest(f"{tool} is not installed")
        if LAB is None:
            LAB = Lab()

    def setUp(self):
        self.state = tempfile.mkdtemp(prefix="state.", dir=LAB.tmp)
        os.chmod(self.state, 0o755)
        for family, table in (("inet", "shadow_egress"), ("inet", "shadow_panic"), ("inet", "shadow_filter"),
                              ("bridge", "shadow_egress"), ("bridge", "shadow_panic"), ("ip", "hide_me_test")):
            sh(["nft", "-f", "-"], input=f"table {family} {table} {{}}\ndelete table {family} {table}\n")
        self.cmdline("BOOT_IMAGE=/vmlinuz ro quiet")
        LAB.drain_tun()

    # ---- helpers
    def cmdline(self, text):
        with open(os.path.join(self.state, "cmdline"), "w") as fh:
            fh.write(text + "\n")

    def ks_consts(self):
        s = self.state
        return {"CONFIG": f"{s}/etc/killswitch.json", "RUN_DIR": f"{s}/run", "CMDLINE": f"{s}/cmdline",
                "TOR_USER": str(TOR), "OPENVPN_USER": str(OPENVPN)}

    def ks(self, *args, ok=True, fake_nm=False):
        env = dict(os.environ, PATH=(LAB.bin + ":" if fake_nm else "") + os.environ["PATH"],
                   PYTHONDONTWRITEBYTECODE="1")
        p = sh([sys.executable, "-c", BOOTSTRAP, KILLSWITCH, json.dumps(self.ks_consts()), *args], check=False,
               env=env)
        if ok:
            self.assertEqual(p.returncode, 0, f"shadow-killswitch {' '.join(args)}: {p.stderr}")
        return p

    def ks_status(self):
        return json.loads(self.ks("status", "--json", ok=False).stdout)

    def panic(self, *args, ok=True):
        s = self.state
        consts = {"CONFIG": f"{s}/etc/panic.json", "RUN_DIR": f"{s}/run", "PENDING": f"{s}/var/panic-pending.json",
                  "NM_STATE_FILE": f"{s}/var/NetworkManager.state", "RFKILL_SYSFS": f"{s}/sys/rfkill",
                  "RFKILL_SAVED": f"{s}/var/rfkill"}
        os.makedirs(f"{s}/etc", exist_ok=True)
        with open(f"{s}/etc/panic.json", "w") as fh:
            json.dump({"rfkill": True, "lock_session": False, "clear_clipboard": False}, fh)
        p = sh([sys.executable, "-c", BOOTSTRAP, PANIC, json.dumps(consts), *args], check=False,
               env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"))
        if ok:
            self.assertEqual(p.returncode, 0, f"shadow-panic {' '.join(args)}: {p.stderr}")
        return p

    def probe(self, uid, checks, ns=None):
        """Run checks [(proto, dst, port, extra...)] as uid (None: root) → list of results."""
        lst = []
        for c in checks:
            d = {"proto": c[0], "dst": c[1], "port": c[2]}
            if len(c) > 3:
                d.update(c[3])
            lst.append(d)
        argv = [sys.executable, os.path.join(LAB.tmp, "probe.py"), json.dumps(lst)]
        if uid is not None:
            argv = ["setpriv", f"--reuid={uid}", f"--regid={uid}", "--clear-groups"] + argv
        if ns:
            argv = ["ip", "netns", "exec", ns] + argv
        return json.loads(sh(argv).stdout)

    def counters(self, table="shadow_egress", family="inet"):
        out = json.loads(sh(["nft", "-j", "list", "table", family, table]).stdout)
        res = {}
        for item in out["nftables"]:
            r = item.get("rule")
            if r:
                key = (r["chain"], r.get("comment", ""))
                res[key] = res.get(key, 0) + sum(e["counter"]["packets"] for e in r["expr"]
                                                 if isinstance(e, dict) and "counter" in e)
        return res

    def table_present(self, table):
        return sh(["nft", "list", "table", "inet", table], check=False).returncode == 0

    def chains(self, table):
        out = json.loads(sh(["nft", "-j", "list", "table", "inet", table]).stdout)
        return {c["chain"]["name"]: (c["chain"].get("prio"), c["chain"].get("policy"))
                for c in out["nftables"] if "chain" in c}

    def watch_service(self):
        """Run the ExecStart= lines of shadow-killswitch-watch.service as systemd would: in order,
        "-" ignores a failure, otherwise a failure ends the run; with the unit's capability
        bounding set (CAP_NET_ADMIN only). Returns [(command, rc, stdout, stderr)]."""
        with open(WATCH_SERVICE) as fh:
            lines = [ln.strip().split("=", 1)[1] for ln in fh if ln.startswith("ExecStart=")]
        with open(WATCH_SERVICE) as fh:
            caps = [ln.strip().split("=", 1)[1] for ln in fh if ln.startswith("CapabilityBoundingSet=")]
        self.assertEqual(caps, ["CAP_NET_ADMIN"])
        s = self.state
        consts = {"/usr/sbin/shadow-killswitch": (KILLSWITCH, self.ks_consts()),
                  "/usr/sbin/shadow-panic": (PANIC, {
                      "CONFIG": f"{s}/etc/panic.json", "RUN_DIR": f"{s}/run",
                      "PENDING": f"{s}/var/panic-pending.json", "NM_STATE_FILE": f"{s}/var/NetworkManager.state",
                      "RFKILL_SYSFS": f"{s}/sys/rfkill", "RFKILL_SAVED": f"{s}/var/rfkill"})}
        results = []
        for line in lines:
            ignore = line.startswith("-")
            argv = line.lstrip("-").split()
            tool, tool_consts = consts[argv[0]]
            p = sh(["setpriv", "--inh-caps=-all", "--bounding-set=-all,+net_admin", sys.executable, "-c", BOOTSTRAP,
                    tool, json.dumps(tool_consts), *argv[1:]], check=False,
                   env=dict(os.environ, PYTHONDONTWRITEBYTECODE="1"))
            results.append((line, p.returncode, p.stdout, p.stderr))
            if p.returncode != 0 and not ignore:
                break
        return results

    def drop_counter(self, table, chain):
        return self.counters(table).get((chain, "kill switch" if table == "shadow_egress" else ""), 0)

    UP_TCP = ("tcp", "198.51.100.2", 7000)
    UP_UDP = ("udp", "198.51.100.2", 7001)
    LO_TCP = ("tcp", "127.0.0.1", 7100)
    WG_TCP = ("tcp", "203.0.113.2", 7000)

    # ---- tests
    def test_00_positive_controls_with_the_kill_switch_off(self):
        self.ks("set", "off")
        self.assertEqual(self.probe(USER, [self.UP_TCP, self.UP_UDP, self.LO_TCP, self.WG_TCP]), ["ok"] * 4)
        self.assertEqual(self.probe(None, [self.UP_TCP, self.WG_TCP], ns="vm"), ["ok", "ok"])
        self.assertEqual(self.probe(USER, [("udp", "192.0.2.130", 9, {"noreply": True})]), ["sent"])
        self.assertTrue(any(p[16:20] == bytes([192, 0, 2, 130]) for p in LAB.tun_packets()))

    def test_tor_mode(self):
        self.ks("set", "tor")
        st = self.ks_status()
        self.assertEqual((st["mode"], st["active"], st["loaded"], st["error"]), ("tor", True, True, None))
        # an ordinary user and root: nothing leaves (TCP: the SYN is dropped; UDP: EPERM at once)
        self.assertEqual(self.probe(USER, [self.UP_TCP, self.UP_UDP, self.WG_TCP, ("tcp", "10.77.0.2", 7000)]),
                         ["timeout", "eperm", "timeout", "timeout"])
        self.assertEqual(self.probe(None, [self.UP_TCP, self.UP_UDP]), ["timeout", "eperm"])
        # positive controls: the Tor user, loopback, DHCP-shaped UDP 68 → 67
        self.assertEqual(self.probe(TOR, [self.UP_TCP, self.UP_UDP]), ["ok", "ok"])
        self.assertEqual(self.probe(USER, [self.LO_TCP]), ["ok"])
        self.assertEqual(self.probe(None, [("udp", "198.51.100.2", 67, {"sport": 68})]), ["ok"])
        self.assertEqual(self.probe(None, [("udp", "198.51.100.2", 69, {"sport": 68})]), ["eperm"])
        self.assertEqual(self.probe(USER, [("udp", "192.0.2.130", 9, {"noreply": True})]), ["eperm"])
        # forwarding (a virtual machine) is dropped
        self.assertEqual(self.probe(None, [self.UP_TCP, self.WG_TCP], ns="vm"), ["timeout", "timeout"])
        c = self.counters()
        self.assertGreater(c[("output", "kill switch")], 0)
        self.assertGreater(c[("output", "tor")], 0)
        self.assertGreater(c[("output", "dhcp")], 0)
        self.assertGreater(c[("forward", "kill switch")], 0)
        self.assertGreater(self.ks_status()["dropped_packets"], 0)

    def test_vpn_mode(self):
        self.ks("vpn-endpoint", "add", "198.51.100.2:7002/udp")
        self.ks("vpn-endpoint", "add", "[2001:db8::1]:51820/udp")       # IPv6 elements load (not testable here)
        self.ks("set", "vpn")
        st = self.ks_status()
        self.assertEqual((st["mode"], st["loaded"], st["endpoints"]), ("vpn", True, 2))
        self.assertEqual(self.probe(USER, [("udp", "198.51.100.2", 7002), self.WG_TCP]), ["ok", "ok"])
        self.assertEqual(self.probe(USER, [self.UP_UDP, self.UP_TCP, ("tcp", "198.51.100.2", 7002)]),
                         ["eperm", "timeout", "timeout"])
        self.assertEqual(self.probe(OPENVPN, [self.UP_TCP]), ["ok"])
        self.assertEqual(self.probe(TOR, [self.UP_TCP]), ["timeout"])     # Tor is not a way out in vpn mode
        LAB.drain_tun()
        self.assertEqual(self.probe(USER, [("udp", "192.0.2.130", 9, {"noreply": True})]), ["sent"])
        self.assertTrue(any(p[16:20] == bytes([192, 0, 2, 130]) for p in LAB.tun_packets()),
                        "nothing came out of tun0")
        # a virtual machine: only into the tunnel (the echo proves the replies come back)
        self.assertEqual(self.probe(None, [self.WG_TCP, self.UP_TCP], ns="vm"), ["ok", "timeout"])
        c = self.counters()
        for key in (("output", "vpn endpoint"), ("output", "vpn tunnel"), ("output", "openvpn"),
                    ("output", "kill switch"), ("forward", "into the tunnel"), ("forward", "tunnel replies"),
                    ("forward", "kill switch")):
            self.assertGreater(c[key], 0, key)

    def test_tor_or_vpn_mode(self):
        self.ks("set", "tor-or-vpn")
        self.assertEqual(self.probe(TOR, [self.UP_TCP]), ["ok"])
        self.assertEqual(self.probe(USER, [self.WG_TCP, self.UP_TCP]), ["ok", "timeout"])

    def test_sync_with_networkmanager(self):
        self.ks("set", "vpn")
        self.assertEqual(self.probe(USER, [("tcp", "203.0.113.130", 7000), ("udp", "198.51.100.2", 7003)]),
                         ["timeout", "eperm"])
        p = self.ks("sync", fake_nm=True)
        self.assertEqual(p.stdout + p.stderr, "")                          # nothing logged on success
        st = self.ks_status()
        self.assertEqual((st["loaded"], st["endpoints"]), (True, 1))
        # office0 (only NetworkManager knows it) and the WireGuard peer it reported are now allowed
        self.assertEqual(self.probe(USER, [("tcp", "203.0.113.130", 7000), ("udp", "198.51.100.2", 7003)]),
                         ["ok", "ok"])
        self.assertEqual(self.probe(USER, [self.UP_UDP]), ["eperm"])

    def test_lan(self):
        self.ks("set", "tor", "--lan")
        self.assertEqual(self.probe(USER, [("tcp", "10.77.0.2", 7000)]), ["ok"])
        self.assertEqual(self.probe(USER, [("tcp", "10.192.0.9", 7000), self.UP_TCP]), ["timeout", "timeout"])
        self.assertGreater(self.counters()[("output", "onion range is never the LAN")], 0)
        self.ks("set", "tor", "--no-lan")
        self.assertEqual(self.probe(USER, [("tcp", "10.77.0.2", 7000)]), ["timeout"])

    # ---- review r1-02: loopback destinations through a device-bound socket
    def capture_upstream(self):
        """Count, on the upstream side of veth-h, what arrives addressed to 127.0.0.0/8 → callable.

        Also gives this namespace a default route through "up" (removed afterwards), as a
        real network has: without any route the kernel treats the device-bound destination
        as on-link and asks ARP for 127.x, which nobody answers."""
        sh("ip route add default via 198.51.100.2".split())
        self.addCleanup(sh, "ip route del default via 198.51.100.2".split(), check=False)
        sh(["ip", "netns", "exec", "up", "nft", "-f", "-"], input=(
            'table netdev capture {\n\tchain in {\n\t\ttype filter hook ingress device "veth-u" priority 0;\n'
            '\t\tip daddr 127.0.0.0/8 counter\n\t}\n}\n'))
        self.addCleanup(sh, ["ip", "netns", "exec", "up", "nft", "delete", "table", "netdev", "capture"], check=False)

        def count():
            out = json.loads(sh(["ip", "netns", "exec", "up", "nft", "-j", "list", "table", "netdev",
                                 "capture"]).stdout)
            return sum(e["counter"]["packets"] for i in out["nftables"] if "rule" in i
                       for e in i["rule"]["expr"] if isinstance(e, dict) and "counter" in e)
        return count

    BOUND = {"bind_dev": "veth-h"}

    def test_device_bound_loopback_destinations_never_leave(self):
        """An ordinary user binds a socket to the network card (SO_BINDTODEVICE needs no
        privilege since Linux 5.7) and sends to 127.x: the kernel skips the local route and
        puts the packet, with the real address and an arbitrary payload, on the wire. The
        output chain accepts loopback destinations (Hide Me's redirect needs that); the
        postrouting chain, which sees the real device, must drop it."""
        captured = self.capture_upstream()
        leaks = [("udp", "127.0.0.1", 9999, dict(self.BOUND, noreply=True)),
                 ("udp", "127.200.1.1", 4444, dict(self.BOUND, noreply=True))]
        # positive control: with the kill switch off the packets do reach the wire
        self.ks("set", "off")
        self.assertEqual(self.probe(USER, leaks), ["sent", "sent"])
        self.assertGreaterEqual(captured(), 2)
        for mode in ("tor", "vpn", "tor-or-vpn"):
            before = captured()
            self.ks("set", mode)
            self.assertEqual(self.probe(USER, leaks), ["eperm", "eperm"], mode)
            self.assertIn(self.probe(USER, [("tcp", "127.0.0.1", 80, self.BOUND)])[0], ("eperm", "timeout"), mode)
            self.assertEqual(captured(), before, f"{mode}: a loopback destination reached the network")
            self.assertGreaterEqual(self.counters()[("postrouting", "loopback destination on a real device")], 3)
            # loopback itself still works, and so does the Tor user
            self.assertEqual(self.probe(USER, [self.LO_TCP]), ["ok"])
            self.assertEqual(self.ks_status()["loaded"], True)

    def test_hide_me_redirects_pass_but_device_bound_ones_do_not_leave(self):
        """Hide Me's transparent proxy (shadow-anon's own tproxy_rules(), the nat OUTPUT
        REDIRECTs to the TransPort and the DNSPort, loaded as nft rules) with Ghost Mode's kill
        switch (tor). A redirected packet of a device-bound socket is re-routed out of the
        network card (ip_route_me_harder keeps the bound device), addressed to
        127.0.0.1:DNS_PORT with the DNS query in clear."""
        sh(["nft", "-f", "-"], input=hide_me_nft(TOR))
        # Hide Me's current rules (review d1-10: this test had kept the old ports 5353 and
        # 9040 and TCP port 53 sent to the DNSPort): Tor's ports, and port 1 for DNS over TCP
        listed = sh(["nft", "list", "table", "ip", "hide_me_test"]).stdout
        self.assertEqual({int(p) for p in re.findall(r"redirect to :(\d+)", listed)},
                         {DNS_PORT, TPROXY_PORT, CLOSED_PORT}, listed)
        captured = self.capture_upstream()
        self.ks("set", "tor")
        dns, web = ("udp", "198.51.100.2", 53), ("tcp", "198.51.100.200", 80)
        # positive controls: redirected DNS and TCP reach the fake DNSPort and TransPort;
        # DNS over TCP is refused on loopback (CLOSED_PORT), it does not leave either
        self.assertEqual(self.probe(USER, [dns, web, ("tcp", "198.51.100.2", 53)]),
                         ["ok", "ok", "error:ECONNREFUSED"])
        self.assertEqual(self.probe(USER, [(*dns, dict(self.BOUND, noreply=True))]), ["eperm"])
        self.assertIn(self.probe(USER, [(*web, self.BOUND)])[0], ("eperm", "timeout"))
        self.assertEqual(captured(), 0)
        self.assertEqual(self.probe(USER, [dns, web]), ["ok", "ok"])

    # ---- review r1-08: a tunnel is a tunnel device, not a name
    def test_a_device_only_named_like_a_tunnel_is_not_a_tunnel(self):
        """A macvlan "tun7" on the network card matches tun* by name but puts packets on the
        wire with the real address. A TAP device named tun8 is a tunnel (its frames go to the
        reader, e.g. OpenVPN in TAP mode)."""
        for c in ("ip link add tun7 link veth-h type macvlan mode bridge", "ip addr add 198.51.100.77/32 dev tun7",
                  "ip link set tun7 up", "ip route add 198.51.100.99/32 dev tun7 src 198.51.100.77"):
            sh(c.split())
        self.addCleanup(sh, ["ip", "link", "del", "tun7"], check=False)
        sh("ip netns exec up ip addr add 198.51.100.99/32 dev veth-u".split())
        self.addCleanup(sh, "ip netns exec up ip addr del 198.51.100.99/32 dev veth-u".split(), check=False)
        tap = open_tun("tun8", tap=True)
        self.addCleanup(os.close, tap)
        for c in ("ip link set tun8 up", "ip addr add 198.18.1.1/24 dev tun8",
                  "ip neigh add 198.18.1.2 lladdr 02:00:00:00:00:02 dev tun8"):
            sh(c.split())
        via_tun7 = ("udp", "198.51.100.99", 7001)
        via_tap = ("udp", "198.18.1.2", 9, {"noreply": True})
        self.ks("set", "off")
        self.assertEqual(self.probe(USER, [via_tun7]), ["ok"])                 # positive control: a real path
        self.ks("set", "vpn")
        self.assertEqual(self.probe(USER, [via_tun7, self.WG_TCP]), ["eperm", "ok"])
        while select.select([tap], [], [], 0)[0]:
            os.read(tap, 65535)
        self.assertEqual(self.probe(USER, [via_tap]), ["sent"])
        frames = []
        end = time.time() + 1.0
        while time.time() < end and not frames:
            if select.select([tap], [], [], 0.1)[0]:
                frames.append(os.read(tap, 65535))
        self.assertTrue(any(f[12:14] == b"\x08\x00" and f[30:34] == bytes([198, 18, 1, 2]) for f in frames),
                        "nothing came out of the TAP device")
        c = self.counters()
        self.assertGreater(c[("output", "vpn tunnel (tap)")], 0)
        self.assertGreater(c[("output", "vpn tunnel")], 0)                    # wg0, a layer-3 tunnel
        # the same for forwarded traffic: a routed virtual machine is not let "into the tunnel" tun7
        before = self.counters()
        self.assertEqual(self.probe(None, [("udp", "198.51.100.99", 7001, {"noreply": True})], ns="vm"), ["sent"])
        after = self.counters()
        self.assertEqual(after[("forward", "into the tunnel")], before[("forward", "into the tunnel")])
        self.assertGreater(after[("forward", "kill switch")], before[("forward", "kill switch")])

    # ---- review r1-07: bridged virtual machines and containers
    def test_bridged_guests_are_cut_off_too(self):
        """A guest bridged to the network card is switched at layer 2: without br_netfilter its
        frames never reach the inet hooks. The kill switch's and the panic's bridge tables
        drop them; frames to another local guest on the bridge pass the kill switch."""
        out, guest2 = ("tcp", "198.18.0.2", 7000), ("tcp", "198.18.0.60", 7000)
        self.ks("set", "off")
        self.assertEqual(self.probe(None, [out, guest2], ns="bvm"), ["ok", "ok"])        # positive controls
        for mode in ("tor", "vpn", "tor-or-vpn"):
            self.ks("set", mode)
            self.assertEqual(self.probe(None, [out, guest2], ns="bvm"), ["timeout", "ok"], mode)
            self.assertEqual(self.probe(USER, [self.UP_UDP]), ["eperm"])           # the host itself, too
            self.assertTrue(self.ks_status()["loaded"])
        c = self.counters("shadow_egress", "bridge")
        self.assertGreater(c[("forward", "kill switch")], 0)
        self.assertGreater(c[("forward", "to a local virtual machine or container")], 0)
        # the watchdog notices the bridge table missing (the inet one alone is not the kill switch)
        sh(["nft", "delete", "table", "bridge", "shadow_egress"])
        self.assertEqual(self.probe(None, [out], ns="bvm"), ["ok"])               # really open (control)
        runs = self.watch_service()
        self.assertIn("had been changed", runs[1][2])
        self.assertEqual(self.probe(None, [out], ns="bvm"), ["timeout"])
        self.ks("set", "off")
        self.assertEqual(sh(["nft", "list", "ruleset"]).stdout.strip(), "")
        self.assertEqual(self.probe(None, [out, guest2], ns="bvm"), ["ok", "ok"])
        # the panic (alone) cuts bridged guests completely, also from each other
        self.panic("engage")
        self.assertEqual(self.probe(None, [out, guest2], ns="bvm"), ["timeout", "timeout"])
        self.assertGreater(self.counters("shadow_panic", "bridge")[("forward", "")], 0)
        sh(["nft", "delete", "table", "bridge", "shadow_panic"])                  # noticed and put back
        runs = self.watch_service()
        self.assertIn("had been changed", runs[0][2])
        self.assertEqual(self.probe(None, [out], ns="bvm"), ["timeout"])
        self.panic("release")
        self.assertEqual(sh(["nft", "list", "ruleset"]).stdout.strip(), "")
        self.assertEqual(self.probe(None, [out, guest2], ns="bvm"), ["ok", "ok"])

    def test_engaging_cuts_existing_connections(self):
        if not shutil.which("conntrack"):
            self.skipTest("conntrack is not installed")
        sh(["nft", "-f", NFTABLES_CONF])            # the host firewall: connection tracking is in use
        go = os.path.join(self.state, "go")
        clients = {}
        for uid in (USER, TOR):
            p = subprocess.Popen(["setpriv", f"--reuid={uid}", f"--regid={uid}", "--clear-groups", sys.executable,
                                  os.path.join(LAB.tmp, "client.py"), "198.51.100.2", "7000", go],
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            clients[uid] = (p, json.loads(p.stdout.readline())["sport"])
        try:
            def tracked():
                out = sh(["conntrack", "-L", "-p", "tcp", "--dport", "7000"]).stdout
                return {int(w.split("=")[1]) for line in out.splitlines() for w in line.split()
                        if w.startswith("sport=") and "dport=7000" in line}
            self.assertTrue({clients[USER][1], clients[TOR][1]} <= tracked())
            self.ks("set", "tor")
            st = self.ks_status()
            self.assertEqual(st["conntrack_flushed"], True)
            self.assertEqual(st["sockets_closed"], 1)                     # the user's, not Tor's
            self.assertFalse({clients[USER][1], clients[TOR][1]} & tracked())
            open(go, "w").close()
            user = json.loads(clients[USER][0].stdout.readline())
            tor = json.loads(clients[TOR][0].stdout.readline())
            self.assertTrue(user.startswith("error:"), user)             # closed by ss -K
            self.assertEqual(tor, "ok")                                   # Tor's connection survives
        finally:
            for p, _ in clients.values():
                p.kill()
                p.communicate()

    def test_panic_cuts_everything_but_loopback_and_release_restores_exactly(self):
        os.makedirs(f"{self.state}/sys/rfkill/rfkill0")
        for name, value in (("type", "wlan"), ("name", "phy0"), ("soft", "0"), ("hard", "0")):
            with open(f"{self.state}/sys/rfkill/rfkill0/{name}", "w") as fh:
                fh.write(value + "\n")
        before = sh(["nft", "-s", "list", "ruleset"]).stdout
        inbound = ("tcp", "198.51.100.1", 7200)
        self.assertEqual(self.probe(None, [inbound], ns="up"), ["ok"])          # positive controls
        self.assertEqual(self.probe(USER, [self.UP_TCP]), ["ok"])
        self.panic("engage")
        self.assertEqual(self.probe(USER, [self.UP_TCP, self.UP_UDP, self.LO_TCP]), ["timeout", "eperm", "ok"])
        self.assertEqual(self.probe(TOR, [self.UP_TCP]), ["timeout"])
        self.assertEqual(self.probe(None, [self.UP_TCP, self.WG_TCP]), ["timeout", "timeout"])
        self.assertEqual(self.probe(None, [inbound], ns="up"), ["timeout"])
        self.assertEqual(self.probe(None, [self.UP_TCP], ns="vm"), ["timeout"])
        c = self.counters("shadow_panic")
        for chain in ("input", "output", "forward"):
            self.assertGreater(c[(chain, "")], 0, chain)
        with open(f"{self.state}/sys/rfkill/rfkill0/soft") as fh:
            self.assertEqual(fh.read().strip(), "1")
        st = json.loads(self.panic("status", "--json").stdout)
        self.assertEqual((st["engaged"], st["loaded"], st["error"]), (True, True, None))
        self.panic("release")
        self.assertEqual(sh(["nft", "-s", "list", "ruleset"]).stdout, before)
        with open(f"{self.state}/sys/rfkill/rfkill0/soft") as fh:
            self.assertEqual(fh.read().strip(), "0")
        self.assertEqual(self.probe(USER, [self.UP_TCP]), ["ok"])
        self.assertEqual(self.probe(None, [inbound], ns="up"), ["ok"])

    def test_panic_leaves_the_kill_switch_alone(self):
        self.ks("set", "tor")
        before = sh(["nft", "-s", "list", "table", "inet", "shadow_egress"]).stdout
        self.panic("engage")
        self.assertEqual(self.probe(TOR, [self.UP_TCP]), ["timeout"])         # panic beats the Tor exemption
        self.panic("release")
        self.assertEqual(sh(["nft", "-s", "list", "table", "inet", "shadow_egress"]).stdout, before)
        self.assertTrue(self.ks_status()["loaded"])
        self.assertEqual(self.probe(TOR, [self.UP_TCP]), ["ok"])

    def test_nftables_service_stop_and_start_keep_the_kill_switch(self):
        with open(DROPIN) as fh:
            lines = [ln.strip() for ln in fh if ln.strip() and not ln.startswith("#")]
        stop = [ln.split("=", 1)[1] for ln in lines if ln.startswith("ExecStop=")][-1]
        post = [ln.split("=", 1)[1] for ln in lines if ln.startswith("ExecStartPost=")][-1]
        self.assertEqual(stop, "-/usr/sbin/nft delete table inet shadow_filter")
        self.ks("set", "tor")
        sh(["nft", "-f", NFTABLES_CONF])                                    # ExecStart
        self.assertTrue(self.table_present("shadow_filter"))
        sh(stop.lstrip("-").split())                                          # ExecStop of the drop-in
        self.assertFalse(self.table_present("shadow_filter"))
        self.assertTrue(self.table_present("shadow_egress"))
        sh(["nft", "-f", NFTABLES_CONF])                                    # ExecStart again (restart)
        self.assertTrue(self.ks_status()["loaded"])
        self.assertEqual(self.probe(USER, [self.UP_UDP]), ["eperm"])
        # negative control: Debian's own ExecStop wipes it, the status says so, ExecStartPost puts it back
        sh(["nft", "flush", "ruleset"])
        p = self.ks("status", "--json", ok=False)
        self.assertEqual(p.returncode, 3)
        self.assertFalse(json.loads(p.stdout)["loaded"])
        self.assertEqual(self.probe(USER, [self.UP_UDP]), ["ok"])           # really open: the status was right
        self.assertEqual(post, "-/usr/sbin/shadow-killswitch reassert")
        self.ks("reassert")
        self.assertTrue(self.ks_status()["loaded"])
        self.assertEqual(self.probe(USER, [self.UP_UDP]), ["eperm"])

    def test_boot_and_the_escape_hatch(self):
        os.makedirs(f"{self.state}/etc")
        with open(f"{self.state}/etc/killswitch.json", "w") as fh:
            json.dump({"version": 1, "mode": "tor"}, fh)
        self.cmdline("BOOT_IMAGE=/vmlinuz ro quiet shadow.killswitch=off")
        self.ks("boot")
        self.assertFalse(self.table_present("shadow_egress"))
        self.assertEqual(self.probe(USER, [self.UP_UDP]), ["ok"])
        st = self.ks_status()
        self.assertEqual((st["escape_hatch"], st["active"], st["persistent_mode"]), (True, False, "tor"))
        # the next boot without the parameter: /run is empty again
        shutil.rmtree(f"{self.state}/run")
        self.cmdline("BOOT_IMAGE=/vmlinuz ro quiet")
        self.ks("boot")
        self.assertTrue(self.table_present("shadow_egress"))
        self.assertEqual(self.probe(USER, [self.UP_UDP]), ["eperm"])
        self.assertEqual(self.probe(TOR, [self.UP_UDP]), ["ok"])

    def test_status_is_checked_against_the_kernel(self):
        self.ks("set", "tor")
        sh(["nft", "delete", "table", "inet", "shadow_egress"])
        p = self.ks("status", "--json", ok=False)
        self.assertEqual(p.returncode, 3)
        st = json.loads(p.stdout)
        self.assertEqual((st["active"], st["loaded"]), (True, False))
        with open(f"{self.state}/run/killswitch.json") as fh:
            self.assertFalse(json.load(fh)["loaded"])                          # what the desktop reads

    # ---- the watchdog (shadow-killswitch-watch.timer → .service)
    def test_watchdog_puts_back_a_flushed_kill_switch(self):
        self.ks("set", "tor")
        listing = sh(["nft", "-s", "list", "table", "inet", "shadow_egress"]).stdout
        self.assertEqual(self.probe(USER, [self.UP_UDP, self.UP_UDP]), ["eperm", "eperm"])
        counted = self.drop_counter("shadow_egress", "output")
        self.assertGreater(counted, 0)
        # nothing missing: the service is silent, reloads nothing, the counters keep counting
        runs = self.watch_service()
        self.assertEqual([(r[1], r[2], r[3]) for r in runs], [(0, "", "")] * 2, runs)
        self.assertEqual(self.drop_counter("shadow_egress", "output"), counted)
        # something flushes the ruleset: really open now (positive control) ...
        sh(["nft", "flush", "ruleset"])
        self.assertFalse(self.table_present("shadow_egress"))
        self.assertEqual(self.probe(USER, [self.UP_UDP]), ["ok"])
        # ... one run of the service closes it again, with exactly the same table
        runs = self.watch_service()
        self.assertEqual([r[1] for r in runs], [0, 0], runs)
        self.assertEqual(runs[0][2] + runs[0][3], "")                         # no panic: nothing to say
        self.assertEqual(len(runs[1][2].splitlines()), 1, runs)
        self.assertIn("loaded again (mode tor)", runs[1][2])
        self.assertEqual(sh(["nft", "-s", "list", "table", "inet", "shadow_egress"]).stdout, listing)
        self.assertEqual(self.chains("shadow_egress"), {"output": (-50, "drop"), "forward": (-50, "drop"),
                                                        "postrouting": (-50, "accept")})
        self.assertEqual(self.drop_counter("shadow_egress", "output"), 0)    # a new table counts from zero
        self.assertEqual(self.probe(USER, [self.UP_UDP]), ["eperm"])
        self.assertEqual(self.probe(TOR, [self.UP_UDP]), ["ok"])             # positive control: Tor still out
        self.assertEqual(self.drop_counter("shadow_egress", "output"), 1)
        st = self.ks_status()
        self.assertEqual((st["mode"], st["loaded"], st["error"]), ("tor", True, None))
        # a rule added by hand that opens it is noticed too
        sh(["nft", "insert", "rule", "inet", "shadow_egress", "output", "accept"])
        self.assertEqual(self.probe(USER, [self.UP_UDP]), ["ok"])
        runs = self.watch_service()
        self.assertIn("had been changed", runs[1][2])
        self.assertEqual(self.probe(USER, [self.UP_UDP]), ["eperm"])
        self.assertEqual(sh(["nft", "-s", "list", "table", "inet", "shadow_egress"]).stdout, listing)

    def test_watchdog_puts_back_an_engaged_panic(self):
        self.panic("engage")
        listing = sh(["nft", "-s", "list", "table", "inet", "shadow_panic"]).stdout
        self.assertEqual(self.probe(USER, [self.UP_UDP]), ["eperm"])
        counted = self.drop_counter("shadow_panic", "output")
        self.assertGreater(counted, 0)
        runs = self.watch_service()
        self.assertEqual([(r[1], r[2], r[3]) for r in runs], [(0, "", "")] * 2, runs)
        self.assertEqual(self.drop_counter("shadow_panic", "output"), counted)
        sh(["nft", "flush", "ruleset"])
        self.assertEqual(self.probe(USER, [self.UP_UDP]), ["ok"])                # really open (positive control)
        runs = self.watch_service()
        self.assertEqual([r[1] for r in runs], [0, 0], runs)
        self.assertIn("loaded again (the panic is still engaged)", runs[0][2])
        self.assertEqual(runs[1][2] + runs[1][3], "")                             # kill switch off: nothing
        self.assertEqual(sh(["nft", "-s", "list", "table", "inet", "shadow_panic"]).stdout, listing)
        self.assertEqual(self.chains("shadow_panic"),
                         {"input": (-300, "drop"), "output": (-300, "drop"), "forward": (-300, "drop")})
        self.assertEqual(self.probe(USER, [self.UP_UDP, self.LO_TCP]), ["eperm", "ok"])
        self.assertEqual(self.drop_counter("shadow_panic", "output"), 1)
        self.assertFalse(self.table_present("shadow_egress"))                     # the kill switch stays off
        # rules inserted by hand that let traffic through are noticed too (the panic also drops
        # input, so an output accept alone lets the request out but not the answer back in)
        sh(["nft", "insert", "rule", "inet", "shadow_panic", "output", "accept"])
        self.assertEqual(self.probe(USER, [self.UP_UDP, (*self.UP_UDP, {"noreply": True})]), ["timeout", "sent"])
        sh(["nft", "insert", "rule", "inet", "shadow_panic", "input", "accept"])
        self.assertEqual(self.probe(USER, [self.UP_UDP]), ["ok"])
        runs = self.watch_service()
        self.assertIn("had been changed", runs[0][2])
        self.assertEqual(sh(["nft", "-s", "list", "table", "inet", "shadow_panic"]).stdout, listing)
        self.assertEqual(self.probe(USER, [self.UP_UDP]), ["eperm"])
        # the release is final: the watchdog never brings a released panic back
        self.panic("release")
        sh(["nft", "flush", "ruleset"])
        runs = self.watch_service()
        self.assertEqual([(r[1], r[2], r[3]) for r in runs], [(0, "", "")] * 2, runs)
        self.assertEqual(sh(["nft", "list", "ruleset"]).stdout.strip(), "")
        self.assertEqual(self.probe(USER, [self.UP_UDP]), ["ok"])

    def test_watchdog_negative_controls_load_nothing(self):
        os.makedirs(f"{self.state}/etc")
        with open(f"{self.state}/etc/killswitch.json", "w") as fh:
            json.dump({"version": 1, "mode": "tor"}, fh)
        # configured, but nothing loaded in this boot yet: the watchdog does not engage it
        runs = self.watch_service()
        self.assertEqual([(r[1], r[2], r[3]) for r in runs], [(0, "", "")] * 2, runs)
        self.assertEqual(sh(["nft", "list", "ruleset"]).stdout.strip(), "")
        self.assertFalse(os.path.exists(f"{self.state}/run"))                  # not even a lock file
        # mode off in force (set off --runtime): flushing and watching loads nothing
        self.ks("set", "tor")
        self.ks("set", "off", "--runtime")
        sh(["nft", "flush", "ruleset"])
        runs = self.watch_service()
        self.assertEqual([(r[1], r[2], r[3]) for r in runs], [(0, "", "")] * 2, runs)
        self.assertEqual(sh(["nft", "list", "ruleset"]).stdout.strip(), "")
        self.assertEqual(self.probe(USER, [self.UP_UDP]), ["ok"])
        # the escape hatch: the boot keeps it off and so does the watchdog
        shutil.rmtree(f"{self.state}/run")
        self.cmdline("BOOT_IMAGE=/vmlinuz ro quiet shadow.killswitch=off")
        self.ks("boot")
        runs = self.watch_service()
        self.assertEqual([(r[1], r[2], r[3]) for r in runs], [(0, "", "")] * 2, runs)
        self.assertFalse(self.table_present("shadow_egress"))
        self.assertEqual(self.probe(USER, [self.UP_UDP]), ["ok"])

    def test_nftables_start_puts_back_kill_switch_and_panic(self):
        with open(DROPIN) as fh:
            posts = [ln.strip().split("=", 1)[1] for ln in fh if ln.startswith("ExecStartPost=")]
        self.assertEqual(posts, ["-/usr/sbin/shadow-panic reassert", "-/usr/sbin/shadow-killswitch reassert"])
        self.ks("set", "tor")
        self.panic("engage")
        with open(NFTABLES_CONF) as fh:
            conf = fh.read()
        sh(["nft", "-f", "-"], input="flush ruleset\n" + conf)                  # an edited nftables.conf
        self.assertFalse(self.table_present("shadow_egress") or self.table_present("shadow_panic"))
        self.panic("reassert")
        self.ks("reassert")
        self.assertTrue(self.table_present("shadow_panic") and self.table_present("shadow_egress"))
        self.assertEqual(self.probe(TOR, [self.UP_UDP]), ["eperm"])              # the panic beats Tor
        self.panic("release")
        self.assertEqual(self.probe(TOR, [self.UP_UDP]), ["ok"])


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
        print("SKIP: needs root (unshare --net, nftables)", file=sys.stderr)
        return 77
    if not shutil.which("unshare"):
        print("SKIP: unshare is not installed", file=sys.stderr)
        return 77
    env = dict(os.environ, **{INNER: "1", OUTER_NETNS: netns_id(), "PYTHONDONTWRITEBYTECODE": "1"})
    return subprocess.call(["unshare", "--net", "--mount", "--fork", sys.executable, os.path.abspath(__file__),
                            *sys.argv[1:]], env=env)


if __name__ == "__main__":
    sys.exit(main())
