# SPDX-License-Identifier: GPL-3.0-or-later
"""Assembling an installed system under a private mount point, and taking it
apart again. The engine only ever unmounts what it mounted itself: every
mount is recorded in a state file in /run/shadow-recovery."""
from __future__ import annotations

import json
import os
import time
from pathlib import Path

from . import devices, scan
from .util import STATE_DIR, TARGET_ID, Output, RecoveryError, run

# Mount points of the installed system that a repair in a chroot needs.
# /home and other data file systems are deliberately left alone.
SYSTEM_MOUNTPOINTS = ("/boot", "/boot/efi", "/efi", "/usr", "/var")


def check_id(target_id: str) -> str:
    if target_id != "running" and not TARGET_ID.match(target_id):
        raise RecoveryError(f"Not a target id: {target_id}", code="usage")
    return target_id


def state_path(target_id: str) -> Path:
    return STATE_DIR / "targets" / f"{target_id}.json"


def load_state(target_id: str) -> dict | None:
    try:
        return json.loads(state_path(target_id).read_text())
    except (OSError, json.JSONDecodeError):
        return None


def save_state(state: dict):
    path = state_path(state["id"])
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(state, indent=2))
    os.replace(temporary, path)


class Target:
    """A system the repair commands work on: a mounted installation (commands
    run in a chroot) or the running system itself (they run directly)."""

    def __init__(self, installation: dict, root: str, state: dict | None):
        self.installation = installation
        self.root = root
        self.state = state
        self.chroot = root != "/"
        self.read_only = bool(state and state.get("read_only"))

    def command(self, argv: list[str]) -> list[str]:
        return ["chroot", self.root] + argv if self.chroot else argv

    def path(self, relative: str) -> str | None:
        """An existing file of the target (links resolved inside the target)."""
        return scan.resolve_in_root(self.root, relative)

    def has_program(self, name: str) -> bool:
        return any(self.path(f"{directory}/{name}") for directory in ("usr/sbin", "usr/bin", "sbin", "bin", "usr/local/sbin"))

    def mounted(self, mountpoint: str) -> bool:
        where = os.path.join(self.root, mountpoint.lstrip("/")) if self.chroot else mountpoint
        return os.path.ismount(where)


def open_target(target_id: str, out: Output | None = None, *, writable: bool = False) -> Target:
    """The target of a repair command. "running" is the system this tool
    runs on, if that is an installed system."""
    check_id(target_id)
    report = scan.scan(out)
    if target_id == "running":
        running = [i for i in report["installations"] if i["running"]]
        if not running:
            raise RecoveryError("This is not an installed system (a live session, or its root file system was not recognised).",
                                "Choose one of the installations found on the disks instead.", code="not-found")
        return Target(running[0], "/", None)
    installation = scan.find_installation(target_id, report=report)
    if installation["running"]:
        return Target(installation, "/", None)
    state = load_state(target_id)
    if state is None or not os.path.ismount(state["mountpoint"]):
        raise RecoveryError("This installation is not mounted.", "Mount it first (mount --target …).", code="not-mounted")
    if writable and state.get("read_only"):
        raise RecoveryError("This installation is mounted read-only.",
                            "Unmount it and mount it again without --read-only.", code="read-only")
    return Target(installation, state["mountpoint"], state)


def mount(out: Output, target_id: str, read_only: bool = False) -> dict:
    check_id(target_id)
    installation = scan.find_installation(target_id, out)
    if installation["running"]:
        raise RecoveryError("This is the system that is running; it is already mounted at /.",
                            "Repairs can be applied to it directly.", code="running")
    if not installation["is_shadow"]:
        raise RecoveryError(f"{installation['name']} is not a Shadow OS installation.",
                            "SHADOW Recovery only repairs Shadow OS.", code="foreign")
    existing = load_state(target_id)
    if existing and os.path.ismount(existing["mountpoint"]):
        raise RecoveryError(f"Already mounted at {existing['mountpoint']}.", code="already-mounted")
    if installation["root"]["mounted_at"]:
        raise RecoveryError(f"{installation['root']['device']} is mounted at {installation['root']['mounted_at'][0]} "
                            "by something else.", "Unmount it there first (for example in the file manager).",
                            code="busy")

    STATE_DIR.mkdir(parents=True, exist_ok=True)
    os.chmod(STATE_DIR, 0o700)
    mountpoint = str(STATE_DIR / f"mnt-{target_id}")
    os.makedirs(mountpoint, mode=0o700, exist_ok=True)
    mountpoint = os.path.realpath(mountpoint)
    state = {"id": target_id, "mountpoint": mountpoint, "root_device": installation["root"]["device"],
             "read_only": read_only, "mounts": [], "created": time.strftime("%Y-%m-%dT%H:%M:%S")}
    warnings: list[str] = []
    mode = "ro" if read_only else "rw"

    def do(argv: list[str], where: str, required: bool, what: str) -> bool:
        code, text = run(out, argv, timeout=120)
        if code == 0:
            state["mounts"].append(where)
            save_state(state)       # after every step, so a failure later can still be undone
            return True
        reason = text.strip().splitlines()[-1] if text.strip() else f"exit status {code}"
        if required:
            raise RecoveryError(f"{what} could not be mounted: {reason}")
        warnings.append(f"{what} could not be mounted: {reason}")
        return False

    try:
        do(["mount", "-t", installation["root"]["fstype"], "-o", mode, installation["root"]["device"], mountpoint],
           mountpoint, True, f"The root file system ({installation['root']['device']})")
        wanted = sorted((f for f in installation["filesystems"] if f["mountpoint"] in SYSTEM_MOUNTPOINTS),
                        key=lambda f: f["mountpoint"].count("/"))
        for filesystem in wanted:
            where = os.path.join(mountpoint, filesystem["mountpoint"].lstrip("/"))
            if filesystem["device"] is None:
                warnings.append(f"{filesystem['mountpoint']} ({filesystem['spec']}) was not found on any disk; it is not mounted.")
                continue
            if scan.resolve_in_root(mountpoint, filesystem["mountpoint"]) != where or not os.path.isdir(where):
                warnings.append(f"{filesystem['mountpoint']} has no directory in the installed system; it is not mounted.")
                continue
            do(["mount", "-t", filesystem["fstype"], "-o", mode, filesystem["device"], where], where, False,
               f"{filesystem['mountpoint']} ({filesystem['device']})")
        for name in ("dev", "proc", "sys", "run"):
            if scan.resolve_in_root(mountpoint, name) != os.path.join(mountpoint, name):
                raise RecoveryError(f"The installed system has no /{name} directory; it cannot be prepared for repairs.")
        do(["mount", "--rbind", "/dev", f"{mountpoint}/dev"], f"{mountpoint}/dev", True, "/dev")
        run(out, ["mount", "--make-rslave", f"{mountpoint}/dev"], timeout=60)
        do(["mount", "-t", "proc", "proc", f"{mountpoint}/proc"], f"{mountpoint}/proc", True, "/proc")
        do(["mount", "-t", "sysfs", "sysfs", f"{mountpoint}/sys"], f"{mountpoint}/sys", True, "/sys")
        if os.path.isdir("/sys/firmware/efi/efivars"):
            do(["mount", "-t", "efivarfs", "efivarfs", f"{mountpoint}/sys/firmware/efi/efivars"],
               f"{mountpoint}/sys/firmware/efi/efivars", False, "The UEFI variables")
        do(["mount", "--bind", "/run", f"{mountpoint}/run"], f"{mountpoint}/run", True, "/run")
    except RecoveryError:
        _unmount_all(out, state)
        raise
    return {"mountpoint": mountpoint, "read_only": read_only, "warnings": warnings,
            "mounts": [m[len(mountpoint):] or "/" for m in state["mounts"]]}


def _unmount_all(out: Output, state: dict) -> list[str]:
    """Unmounts what the state file lists, innermost first; returns what is
    still mounted. Removes the (empty) mount point and the state file when
    nothing is left."""
    busy = []
    for where in reversed(state["mounts"]):
        if not os.path.ismount(where):
            continue
        argv = ["umount", "-R", where] if where.endswith("/dev") else ["umount", where]
        code, text = run(out, argv, timeout=120)
        if code != 0:
            busy.append(where)
    if not busy:
        try:
            os.rmdir(state["mountpoint"])       # only ever an empty directory
        except OSError:
            pass
        try:
            state_path(state["id"]).unlink()
        except OSError:
            pass
    return busy


def umount(out: Output, target_id: str) -> dict:
    check_id(target_id)
    state = load_state(target_id)
    if state is None:
        raise RecoveryError("This installation was not mounted by SHADOW Recovery.", code="not-mounted")
    run(out, ["sync"], timeout=300)
    busy = _unmount_all(out, state)
    if busy:
        users = _users_of(state["mountpoint"])
        raise RecoveryError("Still in use: " + ", ".join(b[len(state["mountpoint"]):] or "/" for b in busy)
                            + (" (" + ", ".join(users) + ")" if users else ""),
                            "Close terminals and programs that work in the installed system, then try again.",
                            code="busy")
    return {"unmounted": True}


def _users_of(mountpoint: str) -> list[str]:
    """Processes whose root or working directory is inside the mount point."""
    found = []
    for pid in filter(str.isdigit, os.listdir("/proc")):
        for link in ("root", "cwd"):
            try:
                where = os.readlink(f"/proc/{pid}/{link}")
            except OSError:
                continue
            if where == mountpoint or where.startswith(mountpoint + "/"):
                try:
                    name = Path(f"/proc/{pid}/comm").read_text().strip()
                except OSError:
                    name = "?"
                found.append(f"{name} [{pid}]")
                break
    return found[:8]


def unmounted_devices_of(installation: dict) -> None:
    """Raises unless no file system of the installation is mounted."""
    if installation["running"]:
        raise RecoveryError("This is the running system; its file systems are in use and cannot be checked from here.",
                            "Start the computer from the Shadow OS medium and choose Recovery.", code="running")
    if installation["mounted_by_recovery"]:
        raise RecoveryError("The installation is mounted.", "Unmount it first; a mounted file system must not be checked.",
                            code="mounted")


def all_filesystems(installation: dict) -> list[dict]:
    """Root first, then the fstab file systems that were found."""
    result = [{"mountpoint": "/", "device": installation["root"]["device"], "fstype": installation["root"]["fstype"]}]
    nodes = {n["path"]: n for n in devices.block_devices()}
    table = devices.mounts()
    for filesystem in installation["filesystems"]:
        if filesystem["device"] and filesystem["device"] not in [r["device"] for r in result]:
            result.append({"mountpoint": filesystem["mountpoint"], "device": filesystem["device"],
                           "fstype": filesystem["fstype"]})
    for entry in result:
        node = nodes.get(entry["device"])
        entry["mounted_at"] = devices.mounted_at(node, table) if node else []
        entry["actual_fstype"] = (node.get("fstype") if node else None) or entry["fstype"]
    return result
