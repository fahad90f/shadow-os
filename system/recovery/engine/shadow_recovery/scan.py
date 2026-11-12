# SPDX-License-Identifier: GPL-3.0-or-later
"""Finding installed systems. Changes nothing on any disk: a file system that
is not mounted is mounted read-only without journal replay for a moment, read,
and unmounted again."""
from __future__ import annotations

import json
import os
import re
import stat
import tempfile
from pathlib import Path

from . import devices
from .util import STATE_DIR, Output, RecoveryError, have, run

ROOT_FSTYPES = ("ext4", "ext3", "ext2", "btrfs", "xfs", "f2fs")
MOUNTABLE_FSTYPES = ROOT_FSTYPES + ("vfat",)
# Read-only AND without replaying the journal / log: scanning must not write.
PROBE_OPTIONS = {"ext4": "ro,noload", "ext3": "ro,noload", "ext2": "ro", "xfs": "ro,norecovery",
                 "btrfs": "ro,rescue=nologreplay", "f2fs": "ro,norecovery"}
USER_NAME = re.compile(r"^[a-z_][a-z0-9_-]{0,31}$")


def resolve_in_root(root: str, relative: str) -> str | None:
    """The real path of <root>/<relative>, following symbolic links the way a
    process chroot-ed into <root> would — a link in an installed system must
    never lead to a file of the system this tool runs on. None if it does
    not exist."""
    root = os.path.realpath(root)
    pending = [p for p in relative.split("/") if p not in ("", ".")]
    current: list[str] = []         # components below root
    hops = 0
    while pending:
        part = pending.pop(0)
        if part == "..":
            if current:
                current.pop()
            continue
        candidate = os.path.join(root, *current, part)
        try:
            mode = os.lstat(candidate).st_mode
        except OSError:
            return None
        if stat.S_ISLNK(mode):
            hops += 1
            if hops > 32:
                return None
            link = os.readlink(candidate)
            if link.startswith("/"):
                current = []
            pending = [p for p in link.split("/") if p not in ("", ".")] + pending
            continue
        current.append(part)
    return os.path.join(root, *current)


def read_in_root(root: str, relative: str, limit: int = 1024 * 1024) -> str | None:
    path = resolve_in_root(root, relative)
    if path is None:
        return None
    try:
        if not stat.S_ISREG(os.stat(path).st_mode):
            return None
        with open(path, "rb") as handle:
            return handle.read(limit).decode("utf-8", "replace")
    except OSError:
        return None


def parse_key_values(text: str) -> dict:
    values = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
        values[key.strip()] = value
    return values


def parse_table(text: str, minimum: int) -> list[list[str]]:
    rows = []
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split()
        if len(fields) >= minimum:
            rows.append(fields)
    return rows


def inspect_root(root: str) -> dict | None:
    """What an installed system says about itself, or None if 'root' is not
    the root of one."""
    release_text = read_in_root(root, "etc/os-release") or read_in_root(root, "usr/lib/os-release")
    if release_text is None or resolve_in_root(root, "usr") is None:
        return None
    release = parse_key_values(release_text)
    boot_conf_text = read_in_root(root, "etc/shadow-os/boot.conf")
    fstab = [{"spec": f[0], "mountpoint": f[1].replace("\\040", " "), "fstype": f[2],
              "options": f[3] if len(f) > 3 else "defaults"}
             for f in parse_table(read_in_root(root, "etc/fstab") or "", 3)]
    crypttab = [{"name": f[0], "source": f[1]} for f in parse_table(read_in_root(root, "etc/crypttab") or "", 2)]
    users = []
    for fields in ((line.split(":") for line in (read_in_root(root, "etc/passwd") or "").splitlines())):
        if len(fields) < 7 or not fields[2].isdigit() or not USER_NAME.match(fields[0]):
            continue
        uid = int(fields[2])
        if uid == 0 or (1000 <= uid < 60000 and not fields[6].endswith(("nologin", "/false"))):
            users.append({"name": fields[0], "uid": uid, "full_name": fields[4].split(",")[0]})
    return {
        "os": {"id": release.get("ID", ""), "name": release.get("NAME", ""),
               "pretty_name": release.get("PRETTY_NAME", "") or release.get("NAME", ""),
               "version": release.get("VERSION_ID", "") or release.get("VERSION", "")},
        "is_shadow": release.get("ID", "").lower() == "shadow" or boot_conf_text is not None,
        "hostname": (read_in_root(root, "etc/hostname") or "").strip().splitlines()[0:1],
        "boot": parse_key_values(boot_conf_text) if boot_conf_text is not None else None,
        "fstab": fstab,
        "crypttab": crypttab,
        "users": users,
        "has_installer_log": resolve_in_root(root, "var/log/installer") is not None,
    }


def _probe(out: Output | None, node: dict) -> tuple[dict | None, str | None]:
    """inspect_root() of a block device; returns (information, problem)."""
    places = devices.mounted_at(node)
    if places:
        return inspect_root(places[0]), None
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    os.chmod(STATE_DIR, 0o700)
    where = tempfile.mkdtemp(prefix="probe-", dir=STATE_DIR)
    try:
        code, text = run(out, ["mount", "-t", node["fstype"], "-o", PROBE_OPTIONS.get(node["fstype"], "ro"),
                               node["path"], where], timeout=60)
        if code != 0:
            return None, f"{node['path']} ({node['fstype']}) could not be read: {text.strip().splitlines()[-1] if text.strip() else 'mount failed'}"
        try:
            return inspect_root(where), None
        finally:
            code, text = run(out, ["umount", where], timeout=60)
            if code != 0:
                run(out, ["umount", "-l", where], timeout=60)
    finally:
        try:
            os.rmdir(where)
        except OSError:
            pass


def _state_of(target_id: str) -> dict | None:
    try:
        return json.loads((STATE_DIR / "targets" / f"{target_id}.json").read_text())
    except (OSError, json.JSONDecodeError):
        return None


def scan(out: Output | None = None) -> dict:
    nodes = devices.block_devices()
    table = devices.mounts()
    medium = devices.live_medium_devices(nodes, table)
    installations, unreadable, locked, inactive = [], [], [], []
    ids: set[str] = set()

    for node in nodes:
        fstype = node.get("fstype") or ""
        if fstype == "crypto_LUKS" and not node["has_children"]:
            locked.append({"device": node["path"], "uuid": node.get("uuid"), "label": node.get("label"),
                           "size": node["size"]})
        if fstype == "LVM2_member" and not node["has_children"]:
            entry = {"device": node["path"], "volume_group": None}
            if have("pvs"):
                code, text = run(out, ["pvs", "--noheadings", "-o", "vg_name", node["path"]], timeout=60)
                entry["volume_group"] = (text.strip() or None) if code == 0 else None
            inactive.append(entry)
        if fstype not in ROOT_FSTYPES or node["kname"] in medium or node.get("type") == "rom":
            continue
        info, problem = _probe(out, node)
        if problem:
            unreadable.append({"device": node["path"], "fstype": fstype, "problem": problem})
        if info is None:
            continue

        crypt = next((a for a in reversed(node["ancestors"]) if a.get("type") == "crypt"), None)
        if node.get("type") == "crypt":
            crypt = node
        crypt_info = None
        if crypt is not None:
            container = crypt["ancestors"][-1] if crypt["ancestors"] else None
            wanted = None
            for entry in info["crypttab"]:
                source = devices.resolve_spec(entry["source"], nodes)
                if container is not None and source is not None and source["kname"] == container["kname"]:
                    wanted = entry["name"]
            crypt_info = {"name": crypt["name"], "device": container["path"] if container else None,
                          "uuid": container.get("uuid") if container else None,
                          "crypttab_name": wanted, "name_matches": wanted is None or wanted == crypt["name"]}
        volume = node if node.get("type") == "lvm" else None

        filesystems, problems = [], []
        for entry in info["fstab"]:
            if entry["mountpoint"] == "/" or entry["fstype"] not in MOUNTABLE_FSTYPES or not entry["mountpoint"].startswith("/"):
                continue
            device = devices.resolve_spec(entry["spec"], nodes)
            filesystems.append({"mountpoint": entry["mountpoint"], "spec": entry["spec"], "fstype": entry["fstype"],
                                "device": device["path"] if device else None,
                                "mounted_at": devices.mounted_at(device, table) if device else []})
            if device is None:
                problems.append(f"The file system for {entry['mountpoint']} ({entry['spec']}) was not found on any disk.")
        if crypt_info and not crypt_info["name_matches"]:
            problems.append(f"The encrypted volume is open as \"{crypt_info['name']}\" but the system expects "
                            f"\"{crypt_info['crypttab_name']}\"; boot repair needs the expected name.")

        target_id = node.get("uuid") or ("dev-" + node["kname"])
        if target_id in ids:
            target_id = f"{target_id}-{node['kname']}"
        ids.add(target_id)
        state = _state_of(target_id)
        installations.append({
            "id": target_id,
            "name": info["os"]["pretty_name"] or "Unknown system",
            "os": info["os"],
            "is_shadow": info["is_shadow"],
            "hostname": info["hostname"][0] if info["hostname"] else "",
            "running": devices.is_running_root(node, table),
            "root": {"device": node["path"], "fstype": fstype, "uuid": node.get("uuid"), "label": node.get("label"),
                     "size": node["size"], "mounted_at": devices.mounted_at(node, table),
                     "disk": node["ancestors"][0]["path"] if node["ancestors"] else node["path"],
                     "disk_model": ((node["ancestors"][0] if node["ancestors"] else node).get("model") or "").strip()},
            "encrypted": crypt_info is not None,
            "crypt": crypt_info,
            "lvm": {"volume": volume["name"]} if volume else None,
            "boot": info["boot"],
            "filesystems": filesystems,
            "users": info["users"],
            "has_installer_log": info["has_installer_log"],
            "mounted_by_recovery": state["mountpoint"] if state and os.path.ismount(state["mountpoint"]) else None,
            "read_only": bool(state and state.get("read_only")) if state else False,
            "problems": problems,
        })

    return {
        "installations": installations,
        "locked_volumes": locked,
        "inactive_lvm": inactive,
        "unreadable": unreadable,
        "live_session": Path("/run/shadow/live-session").exists(),
        "firmware": "uefi" if os.path.isdir("/sys/firmware/efi") else "bios",
        "tools": {name: have(name) for name in ("cryptsetup", "vgchange", "e2fsck", "fsck.vfat", "journalctl", "chroot")},
        "restricted_to": devices.test_devices(),
    }


def find_installation(target_id: str, out: Output | None = None, report: dict | None = None) -> dict:
    report = scan(out) if report is None else report
    for installation in report["installations"]:
        if installation["id"] == target_id:
            return installation
    raise RecoveryError(f"No installed system with the id {target_id} was found.",
                        "The disks changed since the list was made. Scan again.", code="not-found")
