# SPDX-License-Identifier: GPL-3.0-or-later
"""Block devices and mounts, read-only."""
from __future__ import annotations

import json
import os
import re

from .util import RecoveryError, read_text, run

LSBLK_COLUMNS = "NAME,KNAME,PATH,PKNAME,TYPE,SIZE,RO,RM,FSTYPE,LABEL,UUID,PARTUUID,PARTLABEL,PARTTYPE,MOUNTPOINTS,MODEL,TRAN,MAJ:MIN"
ESP_GUID = "c12a7328-f81f-11d2-ba4b-00a0c93ec93b"
LIVE_MEDIUM_MOUNTS = ("/run/live/medium", "/lib/live/mount/medium", "/run/live/persistence")


def test_devices() -> list[str] | None:
    """SHADOW_RECOVERY_TEST_DEVICES=/dev/loopN[,…] confines the engine to
    those devices (and what is on them): nothing else is listed, so nothing
    else can become a target. For tests with loop devices."""
    value = os.environ.get("SHADOW_RECOVERY_TEST_DEVICES", "").strip()
    if not value:
        return None
    devices = [d for d in value.split(",") if d]
    for device in devices:
        if not re.fullmatch(r"/dev/loop\d+", device):
            raise RecoveryError(f"SHADOW_RECOVERY_TEST_DEVICES may only name loop devices, not {device}.", code="usage")
    return devices


def block_devices() -> list[dict]:
    """Every block device as a flat list; each entry knows its ancestors
    (outermost first), e.g. disk → partition → LUKS mapping → logical volume."""
    only = test_devices()
    code, text = run(None, ["lsblk", "-J", "-b", "-o", LSBLK_COLUMNS] + (only or []), timeout=60)
    if code != 0:
        raise RecoveryError("lsblk failed; the disks cannot be examined.", text.strip()[-300:])
    try:
        tree = json.loads(text).get("blockdevices", [])
    except json.JSONDecodeError:
        raise RecoveryError("lsblk printed something unexpected; the disks cannot be examined.")
    flat: list[dict] = []
    seen: set[str] = set()

    def walk(node: dict, ancestors: list[dict]):
        node = dict(node)
        children = node.pop("children", []) or []
        node["mountpoints"] = [m for m in (node.get("mountpoints") or []) if m]
        node["ancestors"] = ancestors
        node["size"] = int(node.get("size") or 0)
        key = node.get("kname") or node.get("name")
        if key not in seen:             # a logical volume on several physical volumes appears once per PV
            seen.add(key)
            flat.append(node)
        for child in children:
            walk(child, ancestors + [node])

    for top in tree:
        walk(top, [])
    # lsblk takes file-system type, UUID and label from the udev database.
    # Where that has nothing (no udev, or it has not caught up with a volume
    # that was just unlocked), ask the device itself.
    for node in flat:
        if node.get("fstype") or node["size"] == 0 or node.get("type") == "rom" or not node.get("path"):
            continue
        code, text = run(None, ["blkid", "-p", "-o", "export", node["path"]], timeout=30)
        if code != 0:
            continue
        probed = dict(line.split("=", 1) for line in text.splitlines() if "=" in line)
        for field, key in (("fstype", "TYPE"), ("uuid", "UUID"), ("label", "LABEL")):
            if probed.get(key) and not node.get(field):
                node[field] = probed[key]
    for node in flat:
        node["has_children"] = any(a.get("kname") == node.get("kname") for other in flat for a in other["ancestors"])
    return flat


def mounts() -> list[dict]:
    """The mount table: mount point, source, type, device number."""
    table = []
    for line in read_text("/proc/self/mountinfo").splitlines():
        fields = line.split(" ")
        try:
            dash = fields.index("-")
        except ValueError:
            continue
        table.append({
            "majmin": fields[2],
            "mountpoint": fields[4].replace("\\040", " "),
            "options": fields[5].split(","),
            "fstype": fields[dash + 1],
            "source": fields[dash + 2],
        })
    return table


def mounted_at(node: dict, table: list[dict] | None = None) -> list[str]:
    """Where a block device is mounted (lsblk's view and the mount table's)."""
    table = mounts() if table is None else table
    places = list(node.get("mountpoints") or [])
    for entry in table:
        if entry["majmin"] == node.get("maj:min") and entry["mountpoint"] not in places:
            places.append(entry["mountpoint"])
    return places


def is_running_root(node: dict, table: list[dict] | None = None) -> bool:
    return "/" in mounted_at(node, table)


def live_medium_devices(nodes: list[dict], table: list[dict]) -> set[str]:
    """Kernel names of the devices the live system runs from."""
    names: set[str] = set()
    for entry in table:
        if entry["mountpoint"] in LIVE_MEDIUM_MOUNTS or entry["mountpoint"].startswith("/run/live/rootfs"):
            for node in nodes:
                if node.get("maj:min") == entry["majmin"]:
                    names.add(node["kname"])
                    names.update(a["kname"] for a in node["ancestors"])
    return names


def resolve_spec(spec: str, nodes: list[dict]) -> dict | None:
    """The device an fstab or crypttab source (UUID=…, LABEL=…, /dev/…) names."""
    def by(field: str, value: str):
        value = value.strip('"')
        for node in nodes:
            if (node.get(field) or "").lower() == value.lower() and value:
                return node
        return None

    for prefix, field in (("UUID=", "uuid"), ("LABEL=", "label"), ("PARTUUID=", "partuuid"), ("PARTLABEL=", "partlabel")):
        if spec.startswith(prefix):
            return by(field, spec[len(prefix):])
    if spec.startswith("/dev/"):
        real = os.path.realpath(spec)
        for node in nodes:
            path = node.get("path") or ""
            if path == spec or (path and os.path.realpath(path) == real and os.path.exists(real)):
                return node
        # /dev/mapper/<vg>-<lv> and /dev/<vg>/<lv> of a system that is not this one
        tail = spec[len("/dev/mapper/"):] if spec.startswith("/dev/mapper/") else spec[len("/dev/"):].replace("-", "--").replace("/", "-")
        for node in nodes:
            if node.get("name") == tail:
                return node
    return None


def find_device(path: str, nodes: list[dict]) -> dict:
    if not re.fullmatch(r"/dev/[A-Za-z0-9._/+:-]+", path) or ".." in path:
        raise RecoveryError(f"Not a device path: {path}", code="usage")
    node = resolve_spec(path, nodes)
    if node is None:
        raise RecoveryError(f"{path} is not a block device this tool can see.", code="not-found")
    return node
