#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The maintainer scripts of shadow-hardening and shadow-base-files, run by dpkg.

    sudo python3 tests/security/test_hardening_maintscripts.py [-v]

Small stand-in packages carry the tree's real preinst, prerm, postinst and
postrm (without the #DEBHELPER# snippets: debhelper is not needed here) and
the configuration files dh_installdeb would list: /etc/nftables.conf (over
the stand-in "nftables" package's own, diverted) and /etc/issue,
/etc/issue.net (over a stand-in "base-files"). The programs the scripts call
are stand-ins that record their arguments (shadow-panic, shadow-harden,
systemctl, sysctl...), except nft, iptables and dpkg itself, which are the
build host's own.

Every case runs in a throw-away root inside private mount, pid and network
namespaces (checked before anything is changed): a tmpfs with the host's
/usr under an overlay and its own /etc, /var/lib/dpkg and /run; /proc/sys is
read-only. nft and iptables act on the private network namespace, where the
kill switch, panic and Hide Me state is set up first. Nothing on the host
changes.

  * the running system (dpkg runs with that root as the root of pid 1):
    an upgrade keeps an engaged panic and the kill switch; "remove" releases
    the panic (prerm), deletes the kill switch, panic and Hide Me state
    (Hide Me's redirect of 9050 to Tor's SOCKS port stays until purge) and
    succeeds although the diverted configuration files are kept; "purge"
    removes that redirect and puts Debian's /etc/nftables.conf and
    /etc/issue back; purging straight
    from installed works too; postinst rewrites an old hidepid unit;
  * a chroot of that root, with the live /run in it (the installer and the
    recovery tools bind-mount /run): remove and upgrade leave the network
    state of the running system alone, release no panic and reload nothing,
    with and without /proc in the chroot.

Needs root, unshare, dpkg, dpkg-deb, nft and overlayfs; skipped otherwise.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
DEBIAN = os.path.join(ROOT, "debian")
OUTER = "SHADOW_MAINTSCRIPT_OUTER_NS"
LOG = "/var/log/maintscript-test.log"
PATH = "/stub:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

DEBIAN_NFT = "#!/usr/sbin/nft -f\n# the nftables package's own file\nflush ruleset\n"
DEBIAN_ISSUE = "Debian GNU/Linux \\n \\l\n"
DEBIAN_OS_RELEASE = 'PRETTY_NAME="Debian GNU/Linux"\nID=debian\n'
OLD_HIDEPID_UNIT = "[Service]\nExecStart=/bin/mount -o remount,hidepid=2 /proc\n"
ENGAGED = json.dumps({"engaged": True, "loaded": True, "error": None, "version": 1}, indent=2, sort_keys=True) + "\n"

RECORDER = '#!/bin/sh\necho "{name} $*" >> ' + LOG + '\n'
STUBS = {   # programs outside the packages: recorded, never run
    "systemctl": RECORDER + 'case "$1" in is-active|is-enabled) exit 3 ;; esac\nexit 0\n',
    "deb-systemd-helper": RECORDER, "deb-systemd-invoke": RECORDER, "sysctl": RECORDER,
    "nmcli": RECORDER, "apparmor_parser": RECORDER,
}
SHADOW_PANIC = RECORDER + """case "$1" in
    status) cat /run/shadow/panic.json 2>/dev/null || echo '{"engaged": false}' ;;
    release) printf '{\\n  "engaged": false\\n}\\n' > /run/shadow/panic.json
             rm -f /var/lib/shadow-os/panic-pending.json ;;
esac
exit 0
"""
LIVE_FILES = ("/run/shadow/killswitch.json", "/run/shadow/killswitch-state.json", "/run/shadow/panic.json",
              "/run/shadow/mac-salt", "/run/systemd/resolved.conf.d/95-shadow-anon.conf")


def ns_ids():
    return f"{os.stat('/proc/self/ns/net').st_ino}:{os.stat('/proc/self/ns/mnt').st_ino}"


def tree_script(package, name):
    path = os.path.join(DEBIAN, f"{package}.{name}")
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8") as fh:
        return fh.read().replace("#DEBHELPER#", "")


def read_file(path):
    with open(path, encoding="utf-8") as fh:
        return fh.read()


def build_deb(out_dir, package, version, files, conffiles=(), scripts=None):
    """files: {"/abs/path": (text, mode)}; scripts: {"postrm": text or None}."""
    stage = os.path.join(out_dir, f"stage-{package}-{version}")
    os.makedirs(os.path.join(stage, "DEBIAN"))
    with open(os.path.join(stage, "DEBIAN", "control"), "w", encoding="utf-8") as fh:
        fh.write(f"Package: {package}\nVersion: {version}\nArchitecture: all\n"
                 "Maintainer: Shadow OS tests <tests@example.invalid>\n"
                 f"Description: stand-in for the maintainer-script test\n")
    if conffiles:
        with open(os.path.join(stage, "DEBIAN", "conffiles"), "w", encoding="utf-8") as fh:
            fh.write("".join(f"{c}\n" for c in conffiles))
    for name, text in (scripts or {}).items():
        if text is not None:
            path = os.path.join(stage, "DEBIAN", name)
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(text)
            os.chmod(path, 0o755)
    for target, (text, mode) in files.items():
        path = stage + target
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            fh.write(text)
        os.chmod(path, mode)
    deb = os.path.join(out_dir, f"{package}_{version}_all.deb")
    subprocess.run(["dpkg-deb", "--root-owner-group", "--build", stage, deb], check=True,
                   stdout=subprocess.DEVNULL)
    shutil.rmtree(stage)
    return os.path.basename(deb)


def build_packages(out_dir):
    hardening_scripts = {s: tree_script("shadow-hardening", s) for s in ("preinst", "prerm", "postinst", "postrm")}
    base_scripts = {s: tree_script("shadow-base-files", s) for s in ("preinst", "prerm", "postinst", "postrm")}
    tools = {"/usr/sbin/shadow-panic": (SHADOW_PANIC.replace("{name}", "shadow-panic"), 0o755)}
    for tool in ("/usr/bin/shadow-harden", "/usr/sbin/shadow-killswitch", "/usr/sbin/shadow-firewall"):
        tools[tool] = (RECORDER.replace("{name}", os.path.basename(tool)) + "exit 0\n", 0o755)
    ruleset = read_file(os.path.join(ROOT, "security", "firewall", "nftables.conf"))
    debs = {
        # (Each with one more file, as the real ones have many: a package all of
        # whose files another one takes over "disappears".)
        "nftables": build_deb(out_dir, "nftables", "1", {"/etc/nftables.conf": (DEBIAN_NFT, 0o755),
                                                         "/usr/share/nftables/README": ("nft\n", 0o644)},
                              ["/etc/nftables.conf"]),
        "base-files": build_deb(out_dir, "base-files", "1", {
            "/etc/issue": (DEBIAN_ISSUE, 0o644), "/etc/issue.net": (DEBIAN_ISSUE, 0o644),
            "/etc/debian_version": ("13.0\n", 0o644),
            "/usr/lib/os-release": (DEBIAN_OS_RELEASE, 0o644)}, ["/etc/issue", "/etc/issue.net"]),
        "shadow-base-files": build_deb(out_dir, "shadow-base-files", "1", {
            "/etc/issue": ("Shadow OS \\n \\l\n", 0o644), "/etc/issue.net": ("Shadow OS\n", 0o644),
            "/usr/lib/os-release": ('PRETTY_NAME="Shadow OS"\nID=shadow\n', 0o644)},
            ["/etc/issue", "/etc/issue.net"], base_scripts),
    }
    for version in ("1", "2"):
        debs[f"shadow-hardening-{version}"] = build_deb(
            out_dir, "shadow-hardening", version, dict(tools, **{"/etc/nftables.conf": (ruleset, 0o755)}),
            ["/etc/nftables.conf"], hardening_scripts)
    return debs


# --------------------------------------------------------------------------- inside the namespaces
class Lab:
    """The throw-away root and the commands run in it."""

    def __init__(self, workdir, mode):
        self.workdir, self.mode = workdir, mode
        self.lab = os.path.join(workdir, "lab")
        self.root = os.path.join(self.lab, "root")
        self.env = {"PATH": PATH, "LC_ALL": "C", "DEBIAN_FRONTEND": "noninteractive", "HOME": "/root"}

    def sh(self, *argv):
        subprocess.run(argv, check=True, stdout=subprocess.DEVNULL)

    def build_root(self):
        os.makedirs(self.lab, exist_ok=True)
        self.sh("mount", "-t", "tmpfs", "-o", "mode=755", "tmpfs", self.lab)
        r = self.root
        for d in ("usr", "etc/systemd/system", "etc/apparmor.d", "var/lib/dpkg/info", "var/lib/dpkg/updates",
                  "var/lib/dpkg/triggers", "var/log", "run", "proc", "dev", "tmp", "root", "stub", "debs",
                  "up/usr", "work/usr"):
            os.makedirs(os.path.join(r if not d.startswith(("up/", "work/")) else self.lab, d), exist_ok=True)
        os.chmod(os.path.join(r, "tmp"), 0o1777)
        for name in ("bin", "sbin", "lib", "lib64", "lib32", "libx32"):
            if os.path.islink("/" + name):
                os.symlink(os.readlink("/" + name), os.path.join(r, name))
            elif os.path.isdir("/" + name):
                return f"/{name} is not a link into /usr (no merged /usr)"
        try:
            self.sh("mount", "-t", "overlay", "overlay", "-o",
                    f"lowerdir=/usr,upperdir={self.lab}/up/usr,workdir={self.lab}/work/usr", os.path.join(r, "usr"))
        except subprocess.CalledProcessError:
            return "overlayfs is not available"
        open(os.path.join(r, "dev", "null"), "w").close()
        self.sh("mount", "--bind", "/dev/null", os.path.join(r, "dev", "null"))
        for name in ("passwd", "group", "nsswitch.conf", "ld.so.cache"):
            if os.path.exists("/etc/" + name):
                shutil.copy2("/etc/" + name, os.path.join(r, "etc", name))
        if os.path.isdir("/etc/alternatives"):
            shutil.copytree("/etc/alternatives", os.path.join(r, "etc", "alternatives"), symlinks=True)
        open(os.path.join(r, "var/lib/dpkg/status"), "w").close()
        for name, text in STUBS.items():
            path = os.path.join(r, "stub", name)
            with open(path, "w", encoding="utf-8") as fh:
                fh.write(text.replace("{name}", name))
            os.chmod(path, 0o755)
        for name in os.listdir(self.workdir):
            if name.endswith(".deb"):
                shutil.copy(os.path.join(self.workdir, name), os.path.join(r, "debs", name))
        return None

    def enter(self):
        """running: this process (pid 1) moves into the root; chroot: it stays outside."""
        if self.mode == "running":
            os.chroot(self.root)
            os.chdir("/")
            self.prefix, self.base = [], ""
        else:
            self.prefix, self.base = ["chroot", self.root], self.root
        self.mount_proc()

    def mount_proc(self):
        proc = self.base + "/proc"
        self.sh("mount", "-t", "proc", "proc", proc)
        self.sh("mount", "--bind", proc + "/sys", proc + "/sys")
        self.sh("mount", "-o", "remount,bind,ro", proc + "/sys")

    def umount_proc(self):
        proc = self.base + "/proc"
        self.sh("umount", proc + "/sys")
        self.sh("umount", proc)

    def run(self, *argv):
        p = subprocess.run([*self.prefix, *argv], env=self.env, stdin=subprocess.DEVNULL, capture_output=True,
                           text=True, timeout=120)
        return p.returncode, (p.stdout + p.stderr)

    def path(self, path):
        return self.base + path

    def write(self, path, text):
        os.makedirs(os.path.dirname(self.path(path)), exist_ok=True)
        with open(self.path(path), "w", encoding="utf-8") as fh:
            fh.write(text)

    def read(self, path):
        try:
            with open(self.path(path), encoding="utf-8") as fh:
                return fh.read()
        except FileNotFoundError:
            return None

    def dpkg(self, *args):
        return self.run("dpkg", "--force-confnew", *args)

    def live_state(self):
        """The kill switch, a panic and Hide Me as the running system has them."""
        rc, out = self.run("nft", "-f", "/debs/state.nft")
        assert rc == 0, out
        self.iptables = True
        for chain in ("SHADOWTOR", "SHADOWDNS", "SHADOWSOCKS"):
            for argv in (["-N", chain], ["-A", "OUTPUT", "-j", chain]):
                if self.run("iptables", "-t", "nat", *argv)[0] != 0:
                    self.iptables = False
        # DNS protection's IPv6 side (it refuses IPv6 DNS) and SHADOWSOCKS's
        # (it refuses [::1]:9050)
        self.ip6tables = all(self.run("ip6tables", "-t", "nat", *argv)[0] == 0
                             for chain in ("SHADOWDNS", "SHADOWSOCKS")
                             for argv in (["-N", chain], ["-A", "OUTPUT", "-j", chain]))
        for path in LIVE_FILES:
            self.write(path, ENGAGED if path.endswith("panic.json") else "{}\n")
        self.write("/var/lib/shadow-os/panic-pending.json", '{"version": 1}\n')

    def snapshot(self):
        log = self.read(LOG) or ""
        self.write(LOG, "")
        _, tables = self.run("nft", "list", "tables")
        nat = self.run("iptables", "-t", "nat", "-S")[1] if self.iptables else None
        nat6 = self.run("ip6tables", "-t", "nat", "-S")[1] if self.ip6tables else None
        _, diversions = self.run("dpkg-divert", "--list")
        _, status = self.run("dpkg-query", "-W", "-f", "${Package} ${db:Status-Abbrev}\\n")
        return {"log": log.splitlines(), "tables": tables, "nat": nat, "nat6": nat6, "diversions": diversions,
                "status": {name: abbrev.strip() for name, abbrev in
                           (line.split(None, 1) for line in status.splitlines() if " " in line.strip())},
                "files": {p: os.path.lexists(self.path(p)) for p in LIVE_FILES + (
                    "/var/lib/shadow-os/panic-pending.json", "/etc/nftables.conf.debian", "/etc/issue.debian")},
                "nftables.conf": self.read("/etc/nftables.conf"), "issue": self.read("/etc/issue"),
                "os-release": self.read("/usr/lib/os-release")}


def scenario(mode, workdir, debs):
    lab = Lab(workdir, mode)
    why = lab.build_root()
    if why:
        return {"skip": why}
    with open(os.path.join(lab.root, "debs", "state.nft"), "w", encoding="utf-8") as fh:
        fh.write("table inet shadow_egress {\n chain output { type filter hook output priority 0; policy accept; }\n}\n"
                 "table inet shadow_panic {\n chain output { type filter hook output priority -300; policy accept; }\n}\n")
    lab.enter()
    os.makedirs(lab.path("/run/systemd/system"))      # booted with systemd (the live /run in a chroot)
    obs = {}
    lab.write("/etc/systemd/system/proc-shadow-hidepid.service", OLD_HIDEPID_UNIT)
    lab.iptables = lab.ip6tables = False
    for name in ("base-files", "nftables", "shadow-base-files", "shadow-hardening-1"):
        obs[f"install {name}"] = lab.dpkg("-i", "/debs/" + debs[name])
    obs["after install"] = lab.snapshot()
    lab.live_state()
    obs["upgrade"] = lab.dpkg("-i", "/debs/" + debs["shadow-hardening-2"])
    obs["after upgrade"] = lab.snapshot()
    obs["remove"] = lab.dpkg("-r", "shadow-hardening")
    obs["after remove"] = lab.snapshot()
    if mode == "chroot":
        lab.umount_proc()
        obs["reinstall without /proc"] = lab.dpkg("-i", "/debs/" + debs["shadow-hardening-1"])
        obs["after reinstall without /proc"] = lab.snapshot()
        obs["remove without /proc"] = lab.dpkg("-r", "shadow-hardening")
        obs["after remove without /proc"] = lab.snapshot()
        return obs
    obs["purge"] = lab.dpkg("-P", "shadow-hardening")
    obs["after purge"] = lab.snapshot()
    obs["base remove"] = lab.dpkg("-r", "shadow-base-files")
    obs["after base remove"] = lab.snapshot()
    obs["base purge"] = lab.dpkg("-P", "shadow-base-files")
    obs["after base purge"] = lab.snapshot()
    obs["reinstall"] = lab.dpkg("-i", "/debs/" + debs["shadow-hardening-1"])
    obs["purge from installed"] = lab.dpkg("-P", "shadow-hardening")
    obs["after purge from installed"] = lab.snapshot()
    return obs


def inner(mode, workdir):
    if os.environ.get(OUTER) in (None, "", ns_ids()) or os.getpid() != 1:
        print("refusing to run: not in private network, mount and pid namespaces", file=sys.stderr)
        return 2
    with open(os.path.join(workdir, "debs.json"), encoding="utf-8") as fh:
        debs = json.load(fh)
    # Opened first: in the "running" case this process ends up inside the root.
    with open(os.path.join(workdir, "result.json"), "w", encoding="utf-8") as fh:
        json.dump(scenario(mode, workdir, debs), fh)
    return 0


# --------------------------------------------------------------------------- the tests
def missing_requirements():
    if os.geteuid() != 0:
        return "needs root (private namespaces, mounts)"
    for tool in ("unshare", "dpkg", "dpkg-deb", "dpkg-divert", "nft", "chroot", "mount"):
        if not shutil.which(tool):
            return f"{tool} is not installed"
    return None


class Base(unittest.TestCase):
    MODE = None
    obs = None

    @classmethod
    def setUpClass(cls):
        why = missing_requirements()
        if why:
            raise unittest.SkipTest(why)
        workdir = tempfile.mkdtemp(prefix="maintscripts.")
        try:
            with open(os.path.join(workdir, "debs.json"), "w", encoding="utf-8") as fh:
                json.dump(build_packages(workdir), fh)
            env = dict(os.environ, **{OUTER: ns_ids(), "PYTHONDONTWRITEBYTECODE": "1"})
            proc = subprocess.run(["unshare", "--mount", "--propagation", "private", "--net", "--pid", "--fork",
                                   sys.executable, "-I", os.path.abspath(__file__), "--inner", cls.MODE, workdir],
                                  env=env, capture_output=True, text=True, timeout=600)
            result = os.path.join(workdir, "result.json")
            if proc.returncode != 0 or not os.path.exists(result):
                raise AssertionError(f"the {cls.MODE} case did not run ({proc.returncode}): {proc.stderr[-3000:]}")
            cls.obs = json.loads(read_file(result))
        finally:
            shutil.rmtree(workdir, ignore_errors=True)
        if "skip" in cls.obs:
            raise unittest.SkipTest(cls.obs["skip"])

    def ok(self, step):
        rc, out = self.obs[step]
        self.assertEqual(rc, 0, f"{step}: dpkg exit status {rc}:\n{out}")

    def snap(self, step):
        return self.obs["after " + step]

    def assertNetworkStateKept(self, step):
        snap = self.snap(step)
        self.assertIn("table inet shadow_egress", snap["tables"], step)
        self.assertIn("table inet shadow_panic", snap["tables"], step)
        if snap["nat"] is not None:
            self.assertIn("-A OUTPUT -j SHADOWTOR", snap["nat"], step)
            self.assertIn("-A OUTPUT -j SHADOWDNS", snap["nat"], step)
            self.assertIn("-A OUTPUT -j SHADOWSOCKS", snap["nat"], step)
        if snap["nat6"] is not None:
            self.assertIn("-A OUTPUT -j SHADOWDNS", snap["nat6"], step)
            self.assertIn("-A OUTPUT -j SHADOWSOCKS", snap["nat6"], step)
        for path in LIVE_FILES:
            self.assertTrue(snap["files"][path], f"{step}: {path} was removed")


class RunningSystem(Base):
    MODE = "running"

    def test_installs(self):
        for step in ("install base-files", "install nftables", "install shadow-base-files",
                     "install shadow-hardening-1", "upgrade"):
            self.ok(step)
        snap = self.snap("install")
        self.assertIn("diversion of /etc/nftables.conf to /etc/nftables.conf.debian by shadow-hardening",
                      snap["diversions"])
        self.assertTrue(snap["files"]["/etc/nftables.conf.debian"], "the case this test is about")
        self.assertTrue(snap["files"]["/etc/issue.debian"])

    def test_old_hidepid_unit_migrated(self):
        self.assertIn("shadow-harden migrate-hidepid", self.snap("install")["log"])

    def test_upgrade_keeps_panic_and_kill_switch(self):
        snap = self.snap("upgrade")
        self.assertNetworkStateKept("upgrade")
        self.assertNotIn("shadow-panic release", snap["log"])
        self.assertIn("shadow-killswitch reassert", snap["log"], "an upgrade reloads a loaded kill switch")

    def test_remove_succeeds_and_lifts_the_network_state(self):
        self.ok("remove")
        snap = self.snap("remove")
        self.assertEqual(snap["status"].get("shadow-hardening"), "rc", snap["status"])
        self.assertNotIn("shadow_egress", snap["tables"])
        self.assertNotIn("shadow_panic", snap["tables"])
        if snap["nat"] is not None:
            for chain in ("SHADOWTOR", "SHADOWDNS"):
                self.assertNotIn(chain, snap["nat"])
            # Hide Me's redirect of 9050 to Tor's SOCKS port stays until purge:
            # Tor's SOCKS ports file stays too, so 9050 is not Tor's.
            self.assertIn("-A OUTPUT -j SHADOWSOCKS", snap["nat"])
        if snap["nat6"] is not None:
            self.assertNotIn("SHADOWDNS", snap["nat6"])
            self.assertIn("-A OUTPUT -j SHADOWSOCKS", snap["nat6"])    # its IPv6 side too (d3-02)
        for path in LIVE_FILES:
            self.assertFalse(snap["files"][path], f"{path} is left behind")
        # The configuration file stays (and with it the diversion) until purge.
        self.assertIn("by shadow-hardening", snap["diversions"])
        self.assertNotEqual(snap["nftables.conf"], DEBIAN_NFT)

    def test_remove_releases_an_engaged_panic(self):
        snap = self.snap("remove")
        self.assertIn("shadow-panic release", snap["log"])
        self.assertFalse(snap["files"]["/var/lib/shadow-os/panic-pending.json"])

    def test_purge_gives_debians_file_back(self):
        self.ok("purge")
        snap = self.snap("purge")
        self.assertIn(snap["status"].get("shadow-hardening"), (None, "un", "pn"), snap["status"])
        if snap["nat"] is not None:
            self.assertNotIn("SHADOW", snap["nat"])                    # SHADOWSOCKS too
        if snap["nat6"] is not None:
            self.assertNotIn("SHADOW", snap["nat6"])                   # SHADOWSOCKS's IPv6 side too
        self.assertNotIn("by shadow-hardening", snap["diversions"])
        self.assertEqual(snap["nftables.conf"], DEBIAN_NFT)
        self.assertFalse(snap["files"]["/etc/nftables.conf.debian"])

    def test_purge_straight_from_installed(self):
        self.ok("reinstall")
        self.ok("purge from installed")
        snap = self.snap("purge from installed")
        self.assertNotIn("by shadow-hardening", snap["diversions"])
        self.assertEqual(snap["nftables.conf"], DEBIAN_NFT)

    def test_shadow_base_files(self):
        self.ok("base remove")
        snap = self.snap("base remove")
        self.assertEqual(snap["os-release"], DEBIAN_OS_RELEASE, "not a configuration file: back at remove")
        self.assertIn("/etc/issue to /etc/issue.debian by shadow-base-files", snap["diversions"])
        self.ok("base purge")
        snap = self.snap("base purge")
        self.assertNotIn("shadow-base-files", snap["diversions"])
        self.assertEqual(snap["issue"], DEBIAN_ISSUE)
        self.assertFalse(snap["files"]["/etc/issue.debian"])
        self.assertEqual((snap["status"].get("base-files"), snap["status"].get("nftables")), ("ii", "ii"))


class Chroot(Base):
    """dpkg in a chroot of another root that has the running system's /run."""
    MODE = "chroot"

    def test_scripts_succeed(self):
        for step in ("install shadow-hardening-1", "upgrade", "remove", "reinstall without /proc",
                     "remove without /proc"):
            self.ok(step)

    def test_upgrade_reloads_nothing_on_the_running_system(self):
        log = self.snap("upgrade")["log"]
        for entry in log:
            self.assertFalse(entry.startswith(("sysctl", "systemctl daemon-reload", "shadow-killswitch",
                                               "shadow-firewall", "nmcli")), entry)
        self.assertIn("shadow-harden migrate-hidepid --offline", log)

    def test_remove_leaves_the_running_systems_network_alone(self):
        self.assertNetworkStateKept("remove")
        self.assertNotIn("shadow-panic release", self.snap("remove")["log"])
        self.assertNetworkStateKept("remove without /proc")
        self.assertNotIn("shadow-panic release", self.snap("remove without /proc")["log"])


def main():
    if len(sys.argv) == 4 and sys.argv[1] == "--inner":
        return inner(sys.argv[2], sys.argv[3])
    unittest.main()
    return 0


if __name__ == "__main__":
    sys.exit(main())
