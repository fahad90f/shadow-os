#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The per-application firewall with real nftables, real control groups and real packets.

    sudo python3 tests/network/test_appfw_netns.py [-v]

Needs root, unshare, nsenter, nft, ip and setpriv (ping is used when it is
installed); without root or one of them the program exits 77 (skipped). It re-executes itself with "unshare --net --mount
--fork": the rule is loaded in a private, throw-away network namespace
(checked before anything is changed), so the computer's own firewall and
network are never touched. /run is replaced by a tmpfs.

Control groups cannot be made private in the same way: the level of a group
is counted from the real root. The test therefore uses a slice of its own
name (shadowappfwtest.slice, never the real shadowblock.slice), creates it at
the top of the hierarchy and removes it again; only the test's own child
processes are ever put into it.

Topology (addresses from the documentation ranges, RFC 5737 and RFC 3849):

    private namespace   lo, veth-h 198.51.100.1/24, 2001:db8::1/64
    "peer" namespace    veth-p 198.51.100.2/24, 2001:db8::2/64, listening on TCP and UDP 7000

What is proved: a process put into the slice by "shadow-appfw enter" cannot
send over TCP, UDP or ICMP, IPv4 or IPv6, to another machine or to loopback,
and neither can its children; a process outside is not affected; an
unprivileged process cannot leave the slice; the rule follows a slice that
was removed and created again; the watchdog puts a deleted table back.
systemd's part (adopting the process into a scope) is replaced by a stand-in
that makes the directory and moves the process: there is no systemd in here.
"""
import importlib.machinery
import importlib.util
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TOOL_PATH = os.path.join(ROOT, "security", "firewall", "shadow-appfw")
INNER = "SHADOW_APPFW_TEST_INNER"
OUTER_NETNS = "SHADOW_APPFW_TEST_OUTER_NETNS"
TEST_SLICE = "shadowappfwtest.slice"
PEER4, PEER6, PORT = "198.51.100.2", "2001:db8::2", 7000
NOBODY = 65534

# A process that waits for a line on stdin and then tries each target; it
# prints one word per target: "ok", "refused", "denied", "unreachable" or "timeout".
PROBE = r"""
import json, socket, sys
targets = json.loads(sys.argv[1])
sys.stdout.write("ready\n"); sys.stdout.flush()
sys.stdin.readline()
out = []
for proto, addr, port in targets:
    family = socket.AF_INET6 if ":" in addr else socket.AF_INET
    s = socket.socket(family, socket.SOCK_STREAM if proto == "tcp" else socket.SOCK_DGRAM)
    s.settimeout(3)
    try:
        if proto == "tcp":
            s.connect((addr, port)); s.send(b"ping"); out.append("ok" if s.recv(4) == b"pong" else "odd")
        else:
            s.sendto(b"ping", (addr, port)); out.append("ok" if s.recvfrom(4)[0] == b"pong" else "odd")
    except ConnectionRefusedError: out.append("refused")
    except PermissionError: out.append("denied")
    except socket.timeout: out.append("timeout")
    except OSError as exc: out.append("unreachable" if exc.errno in (101, 113) else "error:%d" % exc.errno)
    finally: s.close()
print(json.dumps(out)); sys.stdout.flush()
if len(sys.argv) > 2:      # stay alive (the test wants to look at the process)
    sys.stdin.readline()
"""
# Answers "pong" on TCP and UDP, IPv4 and IPv6 (bound to the wildcard address).
ECHO = r"""
import socket, sys, threading
def tcp(family):
    s = socket.socket(family, socket.SOCK_STREAM); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    if family == socket.AF_INET6: s.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
    s.bind(("::" if family == socket.AF_INET6 else "0.0.0.0", int(sys.argv[1]))); s.listen(16)
    while True:
        c, _ = s.accept()
        try:
            c.settimeout(3); c.recv(4); c.send(b"pong")
        except OSError: pass
        c.close()
def udp(family):
    s = socket.socket(family, socket.SOCK_DGRAM)
    if family == socket.AF_INET6: s.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
    s.bind(("::" if family == socket.AF_INET6 else "0.0.0.0", int(sys.argv[1])))
    while True:
        _, peer = s.recvfrom(4); s.sendto(b"pong", peer)
for family in (socket.AF_INET, socket.AF_INET6)[:int(sys.argv[2])]:
    threading.Thread(target=tcp, args=(family,), daemon=True).start()
    threading.Thread(target=udp, args=(family,), daemon=True).start()
sys.stdout.write("ready\n"); sys.stdout.flush()
sys.stdin.readline()
"""


def have_ipv6():
    try:
        socket.socket(socket.AF_INET6, socket.SOCK_DGRAM).close()
        return os.path.exists("/proc/net/if_inet6")
    except OSError:
        return False


# A kernel without IPv6 (some containers): its targets are left out, and the
# test says so. The rule is one rule for both families (table family inet).
HAVE_V6 = have_ipv6()
EVERYWHERE = [("tcp", PEER4, PORT), ("udp", PEER4, PORT), ("tcp", "127.0.0.1", PORT), ("udp", "127.0.0.1", PORT)]
if HAVE_V6:
    EVERYWHERE += [("tcp", PEER6, PORT), ("udp", PEER6, PORT), ("tcp", "::1", PORT), ("udp", "::1", PORT)]
TOOL = None
STARTED = []
HAVE_PING = shutil.which("ping") is not None
# Exits 0 when a TCP connection to the peer can be made (the script is written in setUpModule).
CONNECT = f"{sys.executable} /run/shadow-test/connect.py"


def sh(command, check=True):
    done = subprocess.run(command, shell=True, capture_output=True, text=True)
    if check and done.returncode != 0:
        raise RuntimeError(f"{command}: {done.stderr.strip() or done.stdout.strip()}")
    return done


def netns_id():
    return os.readlink("/proc/self/ns/net")


def private_netns():
    return os.environ.get(OUTER_NETNS, "") not in ("", netns_id())


def start(argv, **kwargs):
    proc = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, **kwargs)
    STARTED.append(proc)
    assert proc.stdout.readline() == "ready\n", argv
    return proc


def stand_in_for_systemd(app, pid, uid):
    """What systemd does for StartTransientUnit(PIDs=[pid], Slice=...): the directory and the move."""
    scope = os.path.join(TOOL.CGROUP_ROOT, TOOL.SLICE, TOOL.scope_name(app, pid))
    os.makedirs(scope, exist_ok=True)
    with open(os.path.join(scope, "cgroup.procs"), "w") as fh:
        fh.write(str(pid))


def setUpModule():
    global TOOL
    sh("mount --make-rprivate /")
    sh("mount -t tmpfs tmpfs /run")
    if not os.path.isdir("/sys/fs/cgroup"):                 # a bare chroot: no /sys at all
        sh("mount -t sysfs sysfs /sys")
    os.makedirs("/run/shadow-test/etc", exist_ok=True)
    os.makedirs("/run/shadow-test/apps", exist_ok=True)
    os.makedirs("/run/systemd/system", exist_ok=True)       # "systemd is running", for the stand-in
    # The unified hierarchy where nft looks it up; a host without it mounted there gets it here.
    if sh("stat -fc %T /sys/fs/cgroup").stdout.strip() != "cgroup2fs":
        sh("mount -t cgroup2 none /sys/fs/cgroup")
    sys.dont_write_bytecode = True
    loader = importlib.machinery.SourceFileLoader("shadow_appfw", TOOL_PATH)
    spec = importlib.util.spec_from_loader(loader.name, loader)
    TOOL = importlib.util.module_from_spec(spec)
    loader.exec_module(TOOL)
    TOOL.SLICE = TEST_SLICE
    TOOL.CONFIG = "/run/shadow-test/etc/appfw.json"
    TOOL.SOURCE_DIRS = ["/run/shadow-test/apps"]
    TOOL.OVERRIDE_DIR = "/run/shadow-test/override"
    TOOL.start_scope = stand_in_for_systemd
    with open("/run/shadow-test/apps/probe.desktop", "w") as fh:
        fh.write("[Desktop Entry]\nType=Application\nName=Probe\nExec=probe\n")
    with open("/run/shadow-test/connect.py", "w") as fh:
        fh.write(f"import socket\nsocket.create_connection(({PEER4!r}, {PORT}), 3)\n")
    with open("/run/shadow-test/grandchild.sh", "w") as fh:
        fh.write(f"sh -c '{CONNECT}'\n")
    for command in ("ip link set lo up", "ip link add veth-h type veth peer name veth-p"):
        sh(command)
    families = "2" if HAVE_V6 else "1"
    if not HAVE_V6:
        print("note: this kernel has no IPv6; only IPv4 is tested", file=sys.stderr)
    peer = subprocess.Popen(["unshare", "--net", sys.executable, "-c", ECHO, str(PORT), families],
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    STARTED.append(peer)
    sh(f"ip link set veth-p netns {peer.pid}")
    inside = f"nsenter -t {peer.pid} -n"
    commands = ["ip addr add 198.51.100.1/24 dev veth-h", "ip link set veth-h up", f"{inside} ip link set lo up",
                f"{inside} ip addr add {PEER4}/24 dev veth-p", f"{inside} ip link set veth-p up"]
    if HAVE_V6:
        commands += ["ip -6 addr add 2001:db8::1/64 dev veth-h nodad",
                     f"{inside} ip -6 addr add {PEER6}/64 dev veth-p nodad"]
    for command in commands:
        sh(command)
    assert peer.stdout.readline() == "ready\n"
    start([sys.executable, "-c", ECHO, str(PORT), families])  # the local services (loopback)
    assert TOOL.main(["set", "probe", "deny"]) == 0
    for _ in range(100):                                      # the link and its IPv6 address
        ready = [PEER4] + ([PEER6] if HAVE_V6 else [])
        probe = (f"{sys.executable} -c 'import socket, sys; "
                 f"[socket.create_connection((a, {PORT}), 1) for a in sys.argv[1:]]' " + " ".join(ready))
        if sh(probe, check=False).returncode == 0:
            break
        time.sleep(0.1)
    else:
        raise RuntimeError("the peer namespace cannot be reached")


def tearDownModule():
    for proc in STARTED:
        try:
            proc.kill()
            proc.wait(5)
            proc.stdin.close()
            proc.stdout.close()
        except OSError:
            pass
    remove_slice()


def remove_slice():
    base = os.path.join("/sys/fs/cgroup", TEST_SLICE)
    if not os.path.isdir(base):
        return
    for name in os.listdir(base):
        scope = os.path.join(base, name)
        if not os.path.isdir(scope):
            continue
        for _ in range(50):
            try:
                with open(os.path.join(scope, "cgroup.procs")) as fh:
                    pids = fh.read().split()
                for pid in pids:
                    os.kill(int(pid), signal.SIGKILL)
                os.rmdir(scope)
                break
            except OSError:
                time.sleep(0.05)
    os.rmdir(base)


class AppFirewall(unittest.TestCase):
    def tearDown(self):
        for proc in STARTED[2:]:
            if proc.poll() is None:
                proc.kill()
            proc.wait(5)
            proc.stdin.close()
            proc.stdout.close()
        del STARTED[2:]

    def probe(self, targets, confine=True, stay=False, user=None):
        argv = [sys.executable, "-c", PROBE, json.dumps(targets)] + (["stay"] if stay else [])
        if user is not None:
            argv = ["setpriv", f"--reuid={user}", f"--regid={user}", "--clear-groups"] + argv
        proc = start(argv)
        if confine:
            self.assertEqual(TOOL.main(["enter", str(proc.pid), "probe"]), 0)
        proc.stdin.write("go\n")
        proc.stdin.flush()
        return json.loads(proc.stdout.readline()), proc

    def test_a_confined_process_reaches_nothing(self):
        started = time.monotonic()
        result, _ = self.probe(EVERYWHERE)
        # TCP is reset, everything else refused by the kernel on the spot: nothing waits for a timeout.
        self.assertEqual(result, ["refused", "denied"] * (len(EVERYWHERE) // 2))
        self.assertLess(time.monotonic() - started, 5)
        self.assertEqual(sh(CONNECT, check=False).returncode, 0)              # the test itself is not confined

    def test_a_process_outside_is_not_affected(self):
        self.assertTrue(TOOL.table_loaded())
        result, _ = self.probe(EVERYWHERE, confine=False)
        self.assertEqual(result, ["ok"] * len(EVERYWHERE))

    def test_children_and_ping_are_confined_too(self):
        _, proc = self.probe([], stay=True)
        scope = TOOL.cgroup_of(proc.pid)
        self.assertTrue(scope.startswith(f"/{TEST_SLICE}/shadowblock-probe-"), scope)

        def from_inside(command):
            # A process that puts itself where the confined one is and then runs COMMAND.
            mover = f"echo $$ > /sys/fs/cgroup{scope}/cgroup.procs && exec {command}"
            return subprocess.run(["sh", "-c", mover], capture_output=True, text=True, timeout=20)

        self.assertNotEqual(from_inside(CONNECT).returncode, 0)
        self.assertEqual(sh(CONNECT, check=False).returncode, 0)
        # A grandchild: a shell that starts a shell that starts the connecting program.
        self.assertNotEqual(from_inside("sh /run/shadow-test/grandchild.sh").returncode, 0)
        self.assertEqual(sh("sh /run/shadow-test/grandchild.sh", check=False).returncode, 0)
        if HAVE_PING:                                            # ICMP, both families
            self.assertNotEqual(from_inside(f"ping -c1 -W1 {PEER4}").returncode, 0)
            if HAVE_V6:
                self.assertNotEqual(from_inside(f"ping -6 -c1 -W1 {PEER6}").returncode, 0)
            self.assertEqual(sh(f"ping -c1 -W1 {PEER4}", check=False).returncode, 0)
        self.assertGreater(TOOL.rejected_packets(), 0)
        self.assertEqual([r["app"] for r in TOOL.running()], ["probe"])

    def test_an_unprivileged_process_cannot_leave(self):
        _, proc = self.probe([], stay=True, user=NOBODY)
        scope = TOOL.cgroup_of(proc.pid)
        os.makedirs("/sys/fs/cgroup/shadowappfwtest-other.slice", exist_ok=True)
        self.addCleanup(os.rmdir, "/sys/fs/cgroup/shadowappfwtest-other.slice")
        for target in ("/sys/fs/cgroup/shadowappfwtest-other.slice/cgroup.procs", "/sys/fs/cgroup/cgroup.procs"):
            leave = subprocess.run(["sh", "-c", f"echo $$ > /sys/fs/cgroup{scope}/cgroup.procs && exec setpriv "
                                    f"--reuid={NOBODY} --regid={NOBODY} --clear-groups sh -c 'echo $$ > {target}'"],
                                   capture_output=True, text=True)
            self.assertNotEqual(leave.returncode, 0, target)
            self.assertIn("ermission denied", leave.stderr)
        self.assertEqual(TOOL.cgroup_of(proc.pid), scope)

    def test_the_rule_follows_a_slice_that_came_back(self):
        result, proc = self.probe([("tcp", PEER4, PORT)])
        self.assertEqual(result, ["refused"])
        proc.wait(10)
        remove_slice()                                # the last confined application has ended
        self.assertFalse(TOOL.slice_in_set())
        result, _ = self.probe([("tcp", PEER4, PORT), ("udp", "127.0.0.1", PORT)])   # the next one starts
        self.assertEqual(result, ["refused", "denied"])
        self.assertTrue(TOOL.slice_in_set())

    def test_the_watchdog_puts_a_deleted_table_back(self):
        _, proc = self.probe([], stay=True)
        sh("nft delete table inet shadow_appfw")
        self.assertFalse(TOOL.table_loaded())
        self.assertEqual(TOOL.main(["reassert"]), 0)
        self.assertTrue(TOOL.table_loaded() and TOOL.slice_in_set())
        scope = TOOL.cgroup_of(proc.pid)
        blocked = subprocess.run(["sh", "-c", f"echo $$ > /sys/fs/cgroup{scope}/cgroup.procs && exec {CONNECT}"],
                                 capture_output=True)
        self.assertNotEqual(blocked.returncode, 0)

    def test_allowing_again_does_not_confine_the_next_start(self):
        self.assertEqual(TOOL.main(["set", "probe", "allow"]), 0)
        try:
            proc = start([sys.executable, "-c", PROBE, json.dumps([("tcp", PEER4, PORT)])])
            self.assertEqual(TOOL.main(["enter", str(proc.pid), "probe"]), TOOL.NOT_BLOCKED)
            proc.stdin.write("go\n")
            proc.stdin.flush()
            self.assertEqual(json.loads(proc.stdout.readline()), ["ok"])
        finally:
            self.assertEqual(TOOL.main(["set", "probe", "deny"]), 0)


def main():
    if os.environ.get(INNER) == "1":
        if not private_netns():
            print("refusing to run: not in a private network namespace", file=sys.stderr)
            return 2
        prog = unittest.main(exit=False, module="__main__")
        return 0 if prog.result.wasSuccessful() else 1
    # 77: skipped (tests/run-suites.sh), never a pass.
    if os.geteuid() != 0:
        print("SKIP: needs root (unshare --net, nftables, control groups)", file=sys.stderr)
        return 77
    for program in ("unshare", "nsenter", "nft", "ip", "setpriv"):
        if not shutil.which(program):
            print(f"SKIP: {program} is not installed", file=sys.stderr)
            return 77
    env = dict(os.environ, **{INNER: "1", OUTER_NETNS: netns_id(), "PYTHONDONTWRITEBYTECODE": "1"})
    return subprocess.call(["unshare", "--net", "--mount", "--fork", sys.executable, os.path.abspath(__file__),
                            *sys.argv[1:]], env=env)


if __name__ == "__main__":
    sys.exit(main())
