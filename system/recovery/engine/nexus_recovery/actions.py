# SPDX-License-Identifier: GPL-3.0-or-later
"""The repair actions. Each is one sub-command, does exactly what its
description says and reports every command it runs."""
from __future__ import annotations

import io
import json
import os
import pwd
import re
import shlex
import tarfile
import time

from . import devices, scan, target as target_mod
from .util import LOG_DIR, STATE_DIR, Output, RecoveryError, have, read_secret, run, stream

MAPPING_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._+-]{0,63}$")


# ---- steps ---------------------------------------------------------------------------

def run_steps(out: Output, steps: list[dict], dry_run: bool) -> dict:
    """steps: [{id, title, argv, optional?, ok?: exit statuses that count as success}].
    Stops at the first step that fails, unless it is optional."""
    done, failed = [], None
    for step in steps:
        command = " ".join(shlex.quote(a) for a in step["argv"])
        out.emit("step", id=step["id"], title=step["title"], command=command, dry_run=dry_run)
        if dry_run:
            done.append({"id": step["id"], "command": command, "status": "not-run"})
            continue
        code = stream(out, step["argv"], env=step.get("env"))
        ok = code in step.get("ok", (0,))
        status = "ok" if ok else ("skipped" if step.get("optional") else "failed")
        out.emit("step-done", id=step["id"], exit=code, status=status)
        done.append({"id": step["id"], "command": command, "exit": code, "status": status})
        if not ok and not step.get("optional"):
            failed = step
            break
    return {"steps": done, "dry_run": dry_run, "failed_step": failed["id"] if failed else None,
            "ok": failed is None}


def _need_programs(tgt: target_mod.Target, programs: dict[str, str]):
    for program, package in programs.items():
        if not tgt.has_program(program):
            raise RecoveryError(f"The installed system has no {program} (package {package}).",
                                "It cannot be repaired this way. Open a terminal in the installed system "
                                "to look closer, or reinstall keeping your data.", code="missing-tool")


# ---- boot ----------------------------------------------------------------------------

def repair_boot(out: Output, target_id: str, dry_run: bool) -> dict:
    tgt = target_mod.open_target(target_id, out, writable=True)
    installation = tgt.installation
    boot = installation["boot"]
    if boot is None:
        raise RecoveryError("The installed system has no /etc/nexus/boot.conf, the record of how it boots.",
                            "It was not installed by the NEXUS OS installer; its boot loader cannot be "
                            "re-installed automatically.", code="no-boot-conf")
    _need_programs(tgt, {"update-initramfs": "initramfs-tools", "nexus-grub-install": "nexus-boot",
                         "update-grub": "grub2-common"})
    warnings = []
    for filesystem in installation["filesystems"]:
        if filesystem["mountpoint"] in ("/boot", boot.get("ESP_MOUNT", "/boot/efi")) and not tgt.mounted(filesystem["mountpoint"]):
            raise RecoveryError(f"{filesystem['mountpoint']} of the installed system is not mounted"
                                + ("" if filesystem["device"] else f" ({filesystem['spec']} was not found on any disk)") + ".",
                                "Without it the boot loader would be written to the wrong place.", code="not-mounted")
    crypt = installation["crypt"]
    if crypt and not crypt["name_matches"]:
        raise RecoveryError(f"The encrypted volume is open as \"{crypt['name']}\", but the installed system's "
                            f"/etc/crypttab calls it \"{crypt['crypttab_name']}\".",
                            "An initramfs built now would not unlock the disk at boot. Unmount, lock the volume "
                            "and unlock it again with NEXUS Recovery, which uses the expected name.",
                            code="crypt-name")
    firmware = boot.get("FIRMWARE", "")
    booted = "uefi" if os.path.isdir("/sys/firmware/efi") else "bios"
    if firmware == "uefi" and booted != "uefi" and boot.get("ESP_NVRAM") == "yes":
        warnings.append("This session was not started in UEFI mode: the firmware's boot entry \"NEXUS OS\" cannot be "
                        "re-created from here" + (" (the fallback path EFI/BOOT on the disk is refreshed)."
                                                 if boot.get("ESP_REMOVABLE") == "yes" else "."))
    steps = [
        {"id": "initramfs", "title": "Rebuild the initial RAM disks of all installed kernels",
         "argv": tgt.command(["update-initramfs", "-u", "-k", "all"])},
        {"id": "grub-install", "title": "Re-install the boot loader (GRUB, with shim for Secure Boot)",
         "argv": tgt.command(["nexus-grub-install", "--verbose"])},
        {"id": "grub-config", "title": "Regenerate the boot menu",
         "argv": tgt.command(["update-grub"])},
    ]
    result = run_steps(out, steps, dry_run)
    result["warnings"] = warnings
    return result


# ---- packages ------------------------------------------------------------------------

def repair_packages(out: Output, target_id: str, dry_run: bool) -> dict:
    tgt = target_mod.open_target(target_id, out, writable=True)
    _need_programs(tgt, {"dpkg": "dpkg", "apt-get": "apt"})
    steps = [
        {"id": "audit", "title": "List packages that are in a broken state", "optional": True,
         "argv": tgt.command(["dpkg", "--audit"])},
        {"id": "configure", "title": "Finish interrupted package installations",
         "argv": tgt.command(["dpkg", "--configure", "-a"])},
        {"id": "fix-broken", "title": "Repair broken dependencies",
         "argv": tgt.command(["apt-get", "-f", "install", "-y"])},
    ]
    return run_steps(out, steps, dry_run)


# ---- file-system check ---------------------------------------------------------------

def fsck(out: Output, target_id: str, device: str | None, repair: bool) -> dict:
    target_mod.check_id(target_id)
    installation = scan.find_installation(target_id, out)
    target_mod.unmounted_devices_of(installation)
    filesystems = target_mod.all_filesystems(installation)
    if device is not None:
        filesystems = [f for f in filesystems if f["device"] == device]
        if not filesystems:
            raise RecoveryError(f"{device} is not a file system of this installation.", code="usage")
    results, ok = [], True
    for filesystem in filesystems:
        entry = {"device": filesystem["device"], "mountpoint": filesystem["mountpoint"],
                 "fstype": filesystem["actual_fstype"], "repaired": False}
        results.append(entry)
        kind = filesystem["actual_fstype"]
        if filesystem["mounted_at"]:
            # Never check a mounted file system — not even read-only.
            entry.update(status="skipped", detail="mounted at " + ", ".join(filesystem["mounted_at"]))
            if filesystem["mountpoint"] == "/" or device is not None:
                raise RecoveryError(f"{filesystem['device']} is mounted at {filesystem['mounted_at'][0]}.",
                                    "A mounted file system must not be checked. Unmount it first.", code="mounted")
            continue
        if kind in ("ext2", "ext3", "ext4"):
            check = ["e2fsck", "-f", "-n", filesystem["device"]]
            fix = ["e2fsck", "-f", "-y", filesystem["device"]]
            clean, fixed = (0,), (0, 1, 2)
        elif kind == "vfat":
            check = ["fsck.vfat", "-n", filesystem["device"]]
            fix = ["fsck.vfat", "-a", filesystem["device"]]
            clean, fixed = (0,), (0, 1)
        else:
            entry.update(status="unsupported", detail=f"{kind} file systems are not checked by this tool")
            continue
        if not have(check[0]):
            entry.update(status="unsupported", detail=f"{check[0]} is not installed")
            continue
        out.emit("step", id=f"check:{filesystem['device']}", command=" ".join(check), dry_run=False,
                 title=f"Check {filesystem['device']} ({filesystem['mountpoint']}, {kind}) without changing it")
        code = stream(out, check)
        entry["check_exit"] = code
        status = "clean" if code in clean else "errors"
        out.emit("step-done", id=f"check:{filesystem['device']}", exit=code, status="ok" if status == "clean" else "failed")
        if status == "errors" and repair:
            out.emit("step", id=f"repair:{filesystem['device']}", command=" ".join(fix), dry_run=False,
                     title=f"Repair {filesystem['device']} ({filesystem['mountpoint']}, {kind})")
            code = stream(out, fix)
            entry["repair_exit"] = code
            entry["repaired"] = code in fixed
            status = "repaired" if code in fixed else "repair-failed"
            out.emit("step-done", id=f"repair:{filesystem['device']}", exit=code,
                     status="ok" if code in fixed else "failed")
        entry["status"] = status
        ok = ok and status in ("clean", "repaired")
    return {"filesystems": results, "repair": repair, "ok": ok,
            "errors_found": any(r.get("status") in ("errors", "repair-failed") for r in results)}


# ---- password ------------------------------------------------------------------------

def reset_password(out: Output, target_id: str, user: str) -> dict:
    password = read_secret("new password")
    if not scan.USER_NAME.match(user):
        raise RecoveryError(f"Not a user name: {user}", code="usage")
    tgt = target_mod.open_target(target_id, out, writable=True)
    if user not in [u["name"] for u in tgt.installation["users"]]:
        raise RecoveryError(f"The installed system has no login account named {user}.", code="not-found")
    if b":" in user.encode() or b"\n" in password:
        raise RecoveryError("The password must be a single line.", code="usage")
    _need_programs(tgt, {"chpasswd": "passwd"})
    out.emit("step", id="password", title=f"Set a new password for {user}", dry_run=False,
             command=" ".join(tgt.command(["chpasswd"])) + "   (input: “user:password” on stdin, not logged)")
    code, text = run(out, tgt.command(["chpasswd"]), input=user.encode() + b":" + password + b"\n", redact=True)
    out.emit("step-done", id="password", exit=code, status="ok" if code == 0 else "failed")
    if code != 0:
        raise RecoveryError("The password could not be changed: " + (text.strip().splitlines()[-1] if text.strip() else f"exit status {code}"))
    cleared = None
    if tgt.has_program("faillock"):
        # Too many failed log-ins lock an account for a while; forget them.
        code, _ = run(out, tgt.command(["faillock", "--user", user, "--reset"]))
        cleared = code == 0
    return {"user": user, "failed_logins_cleared": cleared}


# ---- logs ----------------------------------------------------------------------------

LOG_FILES = ("etc/os-release", "etc/fstab", "etc/crypttab", "etc/nexus/boot.conf", "etc/default/grub",
             "boot/grub/grub.cfg", "var/log/boot.log", "var/log/dpkg.log", "var/log/apt/history.log",
             "var/log/apt/term.log", "var/log/syslog", "var/log/kern.log")
LOG_DIRECTORIES = ("var/log/installer", "var/log/nexus-installer", "var/log/nexus-recovery")
MAX_LOG_FILE = 16 * 1024 * 1024


def collect_logs(out: Output, target_id: str, output: str) -> dict:
    if not output.startswith("/") or "\0" in output or not output.endswith(".tar.gz"):
        raise RecoveryError("The output file must be an absolute path ending in .tar.gz.", code="usage")
    tgt = target_mod.open_target(target_id, out)
    members: list[str] = []
    skipped: list[str] = []

    # The archive is created with the rights of the user who asked for it
    # (pkexec records who that is), never with root's: root must not be
    # talked into writing where that user could not.
    uid = int(os.environ.get("PKEXEC_UID") or os.environ.get("SUDO_UID") or os.geteuid())
    try:
        account = pwd.getpwuid(uid)
    except KeyError:
        raise RecoveryError("The calling user is unknown.", code="usage")
    if uid != os.geteuid():
        os.setegid(account.pw_gid)
        os.seteuid(uid)
    try:
        fd = os.open(output, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    except FileExistsError:
        raise RecoveryError(f"{output} already exists.", "Choose another name; nothing is overwritten.", code="exists")
    except OSError as exc:
        raise RecoveryError(f"{output} cannot be created: {exc.strerror}.", code="output")
    finally:
        if uid != os.getuid():
            os.seteuid(os.getuid())
            os.setegid(os.getgid())

    def add_bytes(archive: tarfile.TarFile, name: str, data: bytes):
        info = tarfile.TarInfo(name)
        info.size = len(data)
        info.mtime = int(time.time())
        info.mode = 0o600
        archive.addfile(info, io.BytesIO(data))
        members.append(name)

    def add_file(archive: tarfile.TarFile, relative: str):
        path = tgt.path(relative)
        if path is None or not os.path.isfile(path):
            return
        size = os.path.getsize(path)
        with open(path, "rb") as handle:
            if size > MAX_LOG_FILE:
                handle.seek(size - MAX_LOG_FILE)        # the end is what matters
                skipped.append(f"{relative}: only the last {MAX_LOG_FILE // 1024**2} MiB")
            add_bytes(archive, "target/" + relative, handle.read(MAX_LOG_FILE))

    def add_command(archive: tarfile.TarFile, name: str, argv: list[str], timeout: float = 120):
        if not have(argv[0]):
            skipped.append(f"{name}: {argv[0]} is not installed")
            return
        code, text = run(out, argv, timeout=timeout)
        out.log(f"(output stored in the archive as {name})")
        add_bytes(archive, name, (text if code == 0 or text.strip() else f"{argv[0]} failed with exit status {code}\n").encode())

    with os.fdopen(fd, "wb") as raw, tarfile.open(fileobj=raw, mode="w:gz") as archive:
        for relative in LOG_FILES:
            add_file(archive, relative)
        for directory in LOG_DIRECTORIES:
            base = tgt.path(directory)
            if base is None or not os.path.isdir(base):
                continue
            for name in sorted(os.listdir(base))[:200]:
                if os.path.isfile(os.path.join(base, name)) and not os.path.islink(os.path.join(base, name)):
                    add_file(archive, f"{directory}/{name}")
        # The journal of the last three boots of the installed system.
        journal = tgt.path("var/log/journal")
        if journal is not None and os.path.isdir(journal) and os.listdir(journal):
            where = ["-D", journal] if tgt.chroot else []
            for boot in (0, -1, -2):
                add_command(archive, f"journal/boot{boot}.txt",
                            ["journalctl"] + where + ["-b", str(boot), "--no-pager", "-o", "short-iso", "-n", "40000"])
        else:
            skipped.append("journal: the installed system has no persistent journal (/var/log/journal)")
        add_command(archive, "recovery/lsblk.txt", ["lsblk", "-o", "NAME,TYPE,SIZE,FSTYPE,LABEL,UUID,MOUNTPOINTS"]
                    + (devices.test_devices() or []))
        if os.path.isdir("/sys/firmware/efi/efivars"):
            add_command(archive, "recovery/efibootmgr.txt", ["efibootmgr", "-v"])
        add_bytes(archive, "recovery/installation.json", json.dumps(tgt.installation, indent=2).encode())
        # What this tool did so far (the log of this and earlier runs).
        if out.log_path is not None and out.log_path.exists():
            add_bytes(archive, "recovery/" + out.log_path.name, out.log_path.read_bytes()[-MAX_LOG_FILE:])
        if skipped:
            add_bytes(archive, "recovery/not-included.txt", ("\n".join(skipped) + "\n").encode())
    return {"output": output, "files": len(members), "size": os.path.getsize(output), "not_included": skipped,
            "owner": account.pw_name}


# ---- shell ---------------------------------------------------------------------------

def shell(out: Output, target_id: str, execute: bool) -> dict:
    tgt = target_mod.open_target(target_id, out)
    if not tgt.chroot:
        raise RecoveryError("This is the running system; a terminal is already inside it.", code="running")
    program = "/bin/bash" if tgt.path("bin/bash") else "/bin/sh"
    argv = ["chroot", tgt.root, program, "-l"]
    if execute:
        out.log("EXEC " + " ".join(shlex.quote(a) for a in argv))
        os.environ.update({"debian_chroot": "recovery", "HOME": "/root",
                           "PATH": "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"})
        os.execvp(argv[0], argv)
    return {"argv": argv, "command": " ".join(shlex.quote(a) for a in argv), "root": tgt.root}


# ---- encrypted volumes and LVM -------------------------------------------------------

def _unlocked_path():
    return STATE_DIR / "unlocked.json"


def _load_unlocked() -> list[dict]:
    try:
        return json.loads(_unlocked_path().read_text())
    except (OSError, json.JSONDecodeError):
        return []


def _save_unlocked(entries: list[dict]):
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    os.chmod(STATE_DIR, 0o700)
    _unlocked_path().write_text(json.dumps(entries, indent=2))


def _volume_groups_on(out: Output, mapper: str) -> list[str]:
    if not have("pvs"):
        return []
    run(out, ["udevadm", "settle", "--timeout=10"], timeout=30)
    code, text = run(out, ["pvs", "--noheadings", "-o", "vg_name", mapper], timeout=60)
    return sorted({line.strip() for line in text.splitlines() if line.strip()}) if code == 0 else []


def _open(out: Output, device: str, name: str, passphrase: bytes) -> list[str]:
    code, text = run(out, ["cryptsetup", "open", "--type", "luks", "--key-file", "-", device, name],
                     input=passphrase, redact=True, timeout=180)
    if code == 2:
        raise RecoveryError("The passphrase is not correct.", code="bad-passphrase")
    if code != 0:
        raise RecoveryError("The encrypted volume could not be opened: "
                            + (text.strip().splitlines()[-1] if text.strip() else f"cryptsetup exit status {code}"),
                            "If the message mentions device-mapper, this kernel lacks dm-crypt.", code="unlock-failed")
    groups = _volume_groups_on(out, f"/dev/mapper/{name}")
    for group in groups:
        run(out, ["vgchange", "-ay", group], timeout=120)
    run(out, ["udevadm", "settle", "--timeout=10"], timeout=30)
    return groups


def _close(out: Output, name: str, groups: list[str]):
    for group in groups:
        code, text = run(out, ["vgchange", "-an", group], timeout=120)
        if code != 0:
            raise RecoveryError(f"The volume group {group} is still in use.", text.strip()[-200:], code="busy")
    code, text = run(out, ["cryptsetup", "close", name], timeout=120)
    if code != 0:
        raise RecoveryError(f"The encrypted volume {name} could not be closed: "
                            + (text.strip().splitlines()[-1] if text.strip() else f"exit status {code}"), code="busy")


def unlock(out: Output, device: str) -> dict:
    passphrase = read_secret("passphrase")
    nodes = devices.block_devices()
    node = devices.find_device(device, nodes)
    if node.get("fstype") != "crypto_LUKS":
        raise RecoveryError(f"{node['path']} is not a LUKS-encrypted volume.", code="usage")
    if node["has_children"]:
        raise RecoveryError(f"{node['path']} is already unlocked.", code="already-open")
    if not have("cryptsetup"):
        raise RecoveryError("cryptsetup is not installed; encrypted volumes cannot be opened.", code="missing-tool")
    name = "nexus-recovery-" + re.sub(r"[^A-Za-z0-9]", "", node.get("uuid") or node["kname"])[:8]
    groups = _open(out, node["path"], name, passphrase)
    renamed_from = None
    # The installed system's crypttab says which name its initramfs expects;
    # only under that name can the boot files be rebuilt correctly.
    report = scan.scan(out)
    wanted = next((i["crypt"]["crypttab_name"] for i in report["installations"]
                   if i["crypt"] and i["crypt"]["device"] == node["path"] and i["crypt"]["crypttab_name"]), None)
    if wanted and wanted != name and MAPPING_NAME.match(wanted) and not os.path.exists(f"/dev/mapper/{wanted}"):
        _close(out, name, groups)
        renamed_from, name = name, wanted
        groups = _open(out, node["path"], name, passphrase)
        report = scan.scan(out)
    entries = [e for e in _load_unlocked() if e["device"] != node["path"]]
    entries.append({"device": node["path"], "name": name, "volume_groups": groups})
    _save_unlocked(entries)
    return {"device": node["path"], "name": name, "renamed_from": renamed_from, "volume_groups": groups,
            "installations": [i["id"] for i in report["installations"]
                              if i["crypt"] and i["crypt"]["device"] == node["path"]]}


def lock(out: Output, device: str | None) -> dict:
    entries = _load_unlocked()
    chosen = [e for e in entries if device is None or e["device"] == device]
    if device is not None and not chosen:
        raise RecoveryError(f"{device} was not unlocked by NEXUS Recovery.", code="not-found")
    report = scan.scan(out)
    closed = []
    for entry in chosen:
        for installation in report["installations"]:
            if installation["crypt"] and installation["crypt"]["device"] == entry["device"] and installation["root"]["mounted_at"]:
                raise RecoveryError(f"{installation['name']} on {entry['device']} is still mounted.",
                                    "Unmount it first.", code="busy")
        _close(out, entry["name"], entry["volume_groups"])
        closed.append(entry["device"])
        entries = [e for e in entries if e["device"] != entry["device"]]
        _save_unlocked(entries)
    return {"locked": closed}


def activate_lvm(out: Output, group: str) -> dict:
    if not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.+-]{0,126}", group):
        raise RecoveryError(f"Not a volume group name: {group}", code="usage")
    known = {entry["volume_group"] for entry in scan.scan(out)["inactive_lvm"]}
    if group not in known:
        raise RecoveryError(f"There is no inactive volume group named {group}.", code="not-found")
    code, text = run(out, ["vgchange", "-ay", group], timeout=120)
    if code != 0:
        raise RecoveryError(f"The volume group {group} could not be activated: "
                            + (text.strip().splitlines()[-1] if text.strip() else f"exit status {code}"))
    run(out, ["udevadm", "settle", "--timeout=10"], timeout=30)
    return {"volume_group": group}
