# SPDX-License-Identifier: GPL-3.0-or-later
"""Helpers shared by the shadow-anon (Hide Me) tests. Not a test module.

load_tool() loads distro/base/rootfs/usr/bin/shadow-anon as a module.
redirect_paths() points EVERY absolute path constant of it into a temporary
directory (the tool writes /proc/sys/net, /etc/hosts and shell histories, so
a missed constant would change the computer running the tests): a new
constant is redirected automatically, and check_redirected() fails if any
absolute path is left.
"""
import importlib.machinery
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TOOL = os.path.join(ROOT, "distro", "base", "rootfs", "usr", "bin", "shadow-anon")
UNITS = os.path.join(ROOT, "distro", "base", "rootfs", "usr", "lib", "systemd", "system")


def load_tool():
    sys.dont_write_bytecode = True
    loader = importlib.machinery.SourceFileLoader("shadow_anon", TOOL)
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


def path_constants(tool):
    return {name: value for name, value in vars(tool).items()
            if name.isupper() and isinstance(value, str) and value.startswith("/")}


def redirect_paths(tool, root):
    """Every absolute path constant X becomes root + X; the directories exist."""
    for name, value in path_constants(tool).items():
        setattr(tool, name, root + value)
    for d in ("etc", "run", "tmp", "root", "home", "sys/class/net", "proc/sys/net/ipv6/conf/all",
              "etc/tor/torrc.d", "etc/NetworkManager/conf.d", "run/NetworkManager",
              "run/systemd/resolve", "usr/lib/systemd"):
        os.makedirs(os.path.join(root, d), exist_ok=True)
    check_redirected(tool, root)


def check_redirected(tool, root):
    left = {n: v for n, v in path_constants(tool).items() if not v.startswith(root + "/")}
    if left:
        raise AssertionError(f"shadow-anon path constants not redirected into the test directory: {left}")


SHIPPED_RESOLVED = os.path.join(ROOT, "security", "hardening", "resolved.conf.d", "80-shadow-privacy.conf")
RESOLVED_DIRS = ("run/systemd/resolved.conf.d", "etc/systemd/resolved.conf.d")   # later wins on a name


def resolved_global_dns(root):
    """The global DNS= list systemd-resolved builds from the shipped
    80-shadow-privacy.conf and the drop-ins under root/run and root/etc, by
    resolved's rules: files in name order (/etc over /run over /usr/lib for
    the same name); DNS= is a list, a value adds to it, an empty one clears it."""
    chosen = {os.path.basename(SHIPPED_RESOLVED): SHIPPED_RESOLVED}
    for d in RESOLVED_DIRS:
        try:
            names = os.listdir(os.path.join(root, d))
        except OSError:
            continue
        for name in names:
            if name.endswith(".conf"):
                chosen[name] = os.path.join(root, d, name)
    servers = []
    for name in sorted(chosen):
        section = None
        with open(chosen[name]) as f:
            for raw in f:
                line = raw.strip()
                if not line or line[0] in "#;":
                    continue
                if line.startswith("[") and line.endswith("]"):
                    section = line[1:-1].strip()
                    continue
                key, _, value = line.partition("=")
                if section == "Resolve" and key.strip() == "DNS":
                    servers = servers + value.split() if value.strip() else []
    return servers
