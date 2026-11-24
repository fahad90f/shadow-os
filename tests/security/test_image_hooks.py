#!/usr/bin/python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The image's configuration hooks must not change the build host.

    python3 tests/security/test_image_hooks.py [-v]

build/iso/stage-rootfs.sh runs distro/base/hooks/*.sh in a chroot
(build/lib/common.sh in_chroot): the chroot shares the build host's kernel,
its /proc (read-write, so /proc/sys too), /dev and network namespace. A hook
that runs "sysctl -p", loads AppArmor profiles, blocks radios, loads modules
or changes firewall rules changes the build machine, not the image; the image
applies those settings itself at boot.

Checked here:
  * every hook line (comments removed, continuation lines joined) against a
    list of commands that act on the running kernel or on running services;
  * that the check finds the old 70-hardening-setup.sh ("sysctl -p",
    "aa-enforce" without --no-reload), from git, as a negative control;
  * with root: 70-hardening-setup.sh really run, with stand-ins for every
    program it calls, in a private mount namespace whose /etc is a tmpfs
    (nothing on the host changes): it never calls sysctl and asks aa-enforce
    not to reload.

Standard library only.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
HOOKS = os.path.join(ROOT, "distro", "base", "hooks")
HARDENING = os.path.join(HOOKS, "70-hardening-setup.sh")


def cmd(name):
    """A command name as a word of its own: not "systemd-sysctl", not "/etc/sysctl.d"."""
    return rf"(?<![\w.-])(?:{name})(?![\w.-])"


# (pattern, why). Matched against one logical shell line without comments,
# quoted text and "command -v NAME".
FORBIDDEN = [
    (cmd("sysctl"), "sysctl writes the build host's /proc/sys"),
    (cmd("rfkill") + r"\s+(block|unblock)\b", "rfkill acts on the build host's radios (/dev/rfkill)"),
    (cmd("modprobe|insmod|rmmod"), "loads or unloads modules of the build host's kernel"),
    (r"\bip\s+(link|addr|address|route|rule|netns)\s+(add|del|delete|set|flush|change|replace)\b",
     "changes the build host's network"),
    (r"\b(iptables|ip6tables|ebtables|arptables)(-nft|-legacy)?\s+(-t\s+\S+\s+)?-[AIDFXNPRZ]\b",
     "changes the build host's firewall"),
    (r"\bnft\s+(-[a-zA-Z]+\s+)*(add|insert|delete|flush|create|replace|reset|-f\b)",
     "changes the build host's firewall"),
    (r"\b(netfilter-persistent|iptables-restore|ip6tables-restore)\b", "loads rules into the build host"),
    (r"\bsystemctl\s+(--\S+\s+)*(start|stop|restart|reload|try-restart|reload-or-restart|"
     r"try-reload-or-restart|kill|isolate|daemon-reload|daemon-reexec)\b",
     "talks to the build host's systemd (offline enable/disable/mask are fine)"),
    (r"\bsystemctl\s+(--\S+\s+)*(enable|disable|mask)\s+(--\S+\s+)*--now\b", "starts or stops a service now"),
    (r"\b(service|invoke-rc\.d)\s+\S+\s+(start|stop|restart|reload)\b", "starts or stops a service"),
    (cmd("apparmor_parser") + r"(?!.*(-Q|--skip-kernel-load|-p|--preprocess))",
     "loads profiles into the build host's kernel"),
    (cmd("aa-enforce|aa-complain|aa-disable|aa-audit") + r"(?!.*--no-reload)",
     "reloads profiles into the build host's kernel"),
    (r"\budevadm\s+(trigger|control)\b", "acts on the build host's devices"),
    (r"\b(auditctl|augenrules\s+--load)\b", "loads audit rules into the build host's kernel"),
    (r"\b(swapon|swapoff|hwclock|timedatectl|hostnamectl|loginctl)\b", "acts on the build host"),
    (r">\s*/proc/sys/", "writes the build host's /proc/sys"),
    (r">\s*/sys/", "writes the build host's /sys"),
]


def logical_lines(text):
    """[(line number, text)] with comments removed and continuation lines joined.
    Here-documents are skipped (they are file contents, not commands)."""
    out, buf, start, heredoc = [], "", None, None
    for no, raw in enumerate(text.splitlines(), 1):
        if heredoc is not None:
            if raw.strip() == heredoc:
                heredoc = None
            continue
        line = raw
        if not buf:
            start = no
        stripped = line.lstrip()
        if stripped.startswith("#"):
            line = ""
        else:
            line = re.sub(r"(^|\s)#.*$", "", line)            # trailing comment
        m = re.search(r"<<-?\s*['\"]?([A-Za-z_][A-Za-z0-9_]*)['\"]?", line)
        if line.rstrip().endswith("\\"):
            buf += line.rstrip()[:-1] + " "
            continue
        buf += line
        if buf.strip():
            out.append((start, buf.strip()))
        buf = ""
        if m:
            heredoc = m.group(1)
    return out


def strip_noise(line):
    """Quoted text (messages, file names) and "command -v NAME" are not commands."""
    line = re.sub(r"\b(command\s+-v|type\s+-p|which)\s+\S+", " ", line)
    line = re.sub(r'"(?:[^"\\]|\\.)*"', '""', line)
    return re.sub(r"'[^']*'", "''", line)


def findings(text):
    found = []
    for no, line in logical_lines(text):
        clean = strip_noise(line)
        for pattern, why in FORBIDDEN:
            if re.search(pattern, clean):
                found.append((no, line, why))
    return found


class StaticCheck(unittest.TestCase):
    def test_no_hook_changes_the_build_host(self):
        hooks = sorted(f for f in os.listdir(HOOKS) if f.endswith(".sh"))
        self.assertGreater(len(hooks), 5)
        problems = []
        for name in hooks:
            with open(os.path.join(HOOKS, name)) as fh:
                problems += [f"{name}:{no}: {line}  ({why})" for no, line, why in findings(fh.read())]
        self.assertEqual(problems, [])

    def test_commands_stage_rootfs_runs_in_the_chroot(self):
        with open(os.path.join(ROOT, "build", "iso", "stage-rootfs.sh")) as fh:
            lines = [(no, ln) for no, ln in logical_lines(fh.read()) if re.match(r"(run|in_chroot)\b", ln)]
        self.assertTrue(lines)
        problems = [f"stage-rootfs.sh:{no}: {ln} ({why})" for no, ln in lines
                    for pattern, why in FORBIDDEN if re.search(pattern, strip_noise(ln))]
        self.assertEqual(problems, [])

    def test_the_patterns_find_what_they_must(self):
        bad = {
            "sysctl -p /etc/sysctl.d/91-shadow-extreme.conf 2>/dev/null \\\n  || warn x": 1,
            "sysctl -q -w net.ipv4.ip_forward=0": 1,
            "aa-enforce /etc/apparmor.d 2>/dev/null || warn x": 1,
            "rfkill block bluetooth": 1,
            "modprobe -r usb_storage": 1,
            "iptables -t nat -A OUTPUT -j X": 1,
            "nft flush ruleset": 1,
            "nft -f /etc/nftables.conf": 1,
            "systemctl restart tor": 1,
            "systemctl enable --now tor": 1,
            "apparmor_parser -r /etc/apparmor.d/x": 1,
            "echo 1 > /proc/sys/net/ipv4/ip_forward": 1,
        }
        for text, n in bad.items():
            self.assertEqual(len(findings(text)), n, text)
        good = [
            "aa-enforce --no-reload /etc/apparmor.d",
            "systemctl enable tor || true",
            "systemctl disable suricata.service >/dev/null 2>&1 || true",
            "# sysctl -p would change the host",
            'log "91-shadow-extreme.conf in place (applied at boot by systemd-sysctl)."',
            "cat > /etc/sysctl.d/90-x.conf <<EOF\nnet.ipv4.ip_forward = 0\nsysctl -p\nEOF",
            'BLCONF="/etc/modprobe.d/shadow-blacklist.conf"',
            "apt-get install -y rfkill iptables",
            "apparmor_parser -Q -K -T /etc/apparmor.d/x",
            "if command -v aa-enforce >/dev/null 2>&1; then",
            'warn "aa-enforce not found; AppArmor profiles left in complain mode."',
            "ls -la /etc/sysctl.d/91-shadow-extreme.conf /etc/modprobe.d/shadow-blacklist.conf",
        ]
        for text in good:
            self.assertEqual(findings(text), [], text)

    def test_the_old_hardening_hook_is_caught(self):
        """Negative control: the hook as it was before the fix (from git)."""
        if not shutil.which("git"):
            self.skipTest("git is not installed")
        p = subprocess.run(["git", "-C", ROOT, "show", "7d3deb2:distro/base/hooks/70-hardening-setup.sh"],
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        if p.returncode != 0:
            self.skipTest("the old hook is not in this checkout's history")
        whys = {why for _, _, why in findings(p.stdout)}
        self.assertIn("sysctl writes the build host's /proc/sys", whys)
        self.assertIn("reloads profiles into the build host's kernel", whys)


FAKE = r"""#!/bin/sh
printf '%s\n' "$(basename "$0") $*" >> "$FAKE_LOG"
exit 0
"""

RUNNER = r"""
set -eu
mount -t tmpfs tmpfs /etc
mkdir -p /etc/sysctl.d /etc/apparmor.d /etc/security /etc/default
echo 'kernel.kptr_restrict = 2' > /etc/sysctl.d/91-shadow-extreme.conf
echo 'CRON_DAILY_RUN="false"' > /etc/default/rkhunter
exec bash "$1"
"""


class RunTheHardeningHook(unittest.TestCase):
    def run_hook(self, hook):
        if os.geteuid() != 0 or not shutil.which("unshare"):
            self.skipTest("needs root and unshare (a private mount namespace)")
        if os.path.exists("/usr/bin/shadow-harden"):
            self.skipTest("the hook would chmod this computer's /usr/bin/shadow-harden")
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp, True)
        bindir, log = os.path.join(tmp, "bin"), os.path.join(tmp, "calls.log")
        os.makedirs(bindir)
        for name in ("apt-get", "systemctl", "aa-enforce", "aa-complain", "sysctl", "update-initramfs",
                     "rkhunter", "apparmor_parser", "modprobe", "rfkill", "nft", "iptables", "udevadm"):
            with open(os.path.join(bindir, name), "w") as fh:
                fh.write(FAKE)
            os.chmod(os.path.join(bindir, name), 0o755)
        open(log, "w").close()
        runner = os.path.join(tmp, "runner.sh")
        with open(runner, "w") as fh:
            fh.write(RUNNER)
        env = dict(os.environ, PATH=f"{bindir}:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin", FAKE_LOG=log)
        p = subprocess.run(["unshare", "--mount", "--propagation", "private", "--fork", "bash", runner, hook],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env, timeout=120)
        self.assertEqual(p.returncode, 0, p.stdout)
        with open(log) as fh:
            return [ln.split() for ln in fh.read().splitlines()], p.stdout

    def test_no_sysctl_and_no_profile_reload(self):
        calls, out = self.run_hook(HARDENING)
        host_tools = ("sysctl", "apparmor_parser", "modprobe", "rfkill", "nft", "iptables", "udevadm")
        self.assertEqual([c for c in calls if c[0] in host_tools], [], out)
        self.assertIn(["aa-enforce", "--no-reload", "/etc/apparmor.d"], calls)
        self.assertNotIn(["aa-enforce", "/etc/apparmor.d"], calls)
        self.assertEqual([c for c in calls if c[0] == "systemctl"],
                         [["systemctl", "enable", "auditd"], ["systemctl", "enable", "apparmor"]])
        self.assertIn("applied at boot by systemd-sysctl", out)
        self.assertIn("faillock configured", out)                          # it ran to the end ...
        self.assertFalse(os.path.exists("/etc/polkit-1/rules.d/50-shadow-harden.rules"))   # ... in its own /etc

    def test_negative_control_the_old_hook(self):
        p = subprocess.run(["git", "-C", ROOT, "show", "7d3deb2:distro/base/hooks/70-hardening-setup.sh"],
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True) \
            if shutil.which("git") else None
        if p is None or p.returncode != 0:
            self.skipTest("the old hook is not in this checkout's history")
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp, True)
        old = os.path.join(tmp, "70-hardening-setup.sh")
        with open(old, "w") as fh:
            fh.write(p.stdout)
        calls, _ = self.run_hook(old)
        self.assertIn(["sysctl", "-p", "/etc/sysctl.d/91-shadow-extreme.conf"], calls)
        self.assertIn(["aa-enforce", "/etc/apparmor.d"], calls)


if __name__ == "__main__":
    unittest.main(verbosity=2 if "-v" in sys.argv else 1, argv=[sys.argv[0]])
