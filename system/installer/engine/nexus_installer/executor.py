# SPDX-License-Identifier: GPL-3.0-or-later
"""Carry out an installation plan.

This is the only module that modifies disks. Every external command is run
with an argument vector and logged; secrets travel on stdin only. Any failure
raises InstallError, after which :meth:`Executor.abort_cleanup` unmounts and
closes whatever was opened and — for installations into free space — removes
the partitions this run created, so that a retry starts from the same state.
"""
from __future__ import annotations

import json
import os
import re
import shutil
import time
from pathlib import Path

from .answers import Answers
from .plan import partition_path
from .util import InstallError, Reporter, Runner, human_size, read_text

TARGET = "/target"

LIVE_PACKAGES = (
    "live-boot", "live-boot-initramfs-tools", "live-config", "live-config-systemd", "live-tools",
    "user-setup", "nexus-live", "nexus-installer", "nexus-installer-gui",
)

USER_GROUPS = ("sudo", "adm", "audio", "video", "cdrom", "dip", "plugdev", "netdev", "bluetooth",
               "lpadmin", "scanner", "wireshark", "kvm", "render", "input", "users")

# Relative weight of each step for the overall progress bar.
WEIGHTS = {"partition": 3, "encrypt": 4, "format": 4, "keep": 0, "mount": 1, "copy": 55, "configure": 4,
           "user": 2, "cleanup": 6, "initramfs": 9, "bootloader": 8, "profiles": 20, "finish": 4}


class Executor:
    def __init__(self, answers: Answers, plan: dict, report: dict, reporter: Reporter, runner: Runner,
                 target: str = TARGET):
        self.a = answers
        self.plan = plan
        self.report = report
        self.rep = reporter
        self.run = runner
        self.target = target
        self.created_partitions: list[int] = []
        self.mounted: list[str] = []
        self.chroot_mounts: list[str] = []
        self.crypt_open = False
        self.vg_active = False
        self._done = 0.0
        self._total = 1.0
        self._current_weight = 0.0

    # ------------------------------------------------------------------ driver
    def execute(self):
        order = ["partition", "encrypt", "format", "mount", "copy", "configure", "user", "cleanup",
                 "initramfs", "bootloader", "profiles", "finish"]
        planned = {s["id"] for s in self.plan["steps"]}
        titles = {s["id"]: s["title"] for s in self.plan["steps"]}
        titles.setdefault("mount", "Mount the target filesystems")
        titles.setdefault("initramfs", "Build the initial RAM disk")
        steps = [s for s in order if s in planned or s in ("mount", "initramfs")]
        if self.plan["mode"] == "manual" and "partition" in steps:
            steps.remove("partition")
        self._total = float(sum(WEIGHTS[s] for s in steps)) or 1.0
        for index, sid in enumerate(steps, start=1):
            self._current_weight = WEIGHTS[sid]
            self.rep.event("step", id=sid, index=index, total=len(steps), title=titles[sid])
            getattr(self, f"step_{sid}")()
            self._done += self._current_weight
            self._progress(0, done_step=True)
        self.rep.event("done", message="Installation complete. Remove the installation medium and restart.")

    def _progress(self, fraction: float, message: str | None = None, done_step: bool = False):
        base = self._done if done_step else self._done + self._current_weight * max(0.0, min(1.0, fraction))
        fields = {"percent": round(100.0 * base / self._total, 1)}
        if message:
            fields["message"] = message
        self.rep.event("progress", **fields)

    # ------------------------------------------------------------------ helpers
    def _settle(self, disk: str | None = None):
        if disk:
            self.run.run(["partprobe", disk], check=False)
        self.run.run(["udevadm", "settle", "--timeout=30"], check=False)

    def _wait_for(self, device: str, timeout: float = 20):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if os.path.exists(device):
                return
            time.sleep(0.25)
        raise InstallError(f"Device {device} did not appear.", "The kernel did not re-read the partition table.")

    def _sectors(self, start: int, size: int) -> tuple[int, int]:
        disk = next(d for d in self.report["disks"] if d["path"] == self.plan["target_disk"])
        sector = disk["sector_size"]
        return start // sector, (start + size) // sector - 1

    def _uuid(self, device: str, tag: str = "UUID") -> str:
        out = self.run.output(["blkid", "-s", tag, "-o", "value", device]).strip()
        if not out:
            raise InstallError(f"Could not read the {tag} of {device}.")
        return out

    def chroot(self, argv: list[str], **kw):
        env = {"PATH": "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", "HOME": "/root"}
        env.update(kw.pop("env", {}) or {})
        return self.run.run(["chroot", self.target] + argv, env=env, **kw)

    def _write(self, relpath: str, content: str, mode: int = 0o644):
        path = Path(self.target + relpath)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        os.chmod(path, mode)
        self.rep.log(f"WRITE {relpath} ({len(content)} bytes, mode {mode:o})")

    def _part(self, role: str) -> dict | None:
        return next((p for p in self.plan["partitions"] if p["role"] == role), None)

    # ------------------------------------------------------------------ partition
    def step_partition(self):
        disk = self.plan["target_disk"]
        new = [p for p in self.plan["partitions"] if p["create"]]
        if self.plan["mode"] == "erase-disk":
            # Last line of defence: re-verify identity of the disk about to be wiped.
            if os.path.realpath(disk) == os.path.realpath(self.report["live_medium"].get("disk") or "/nonexistent"):
                raise InstallError("Refusing to erase the installation medium.")
            self.run.run(["sgdisk", "--zap-all", disk], check=False)
            self.run.run(["wipefs", "--all", "--force", disk])
            self.run.run(["sgdisk", "--clear", disk])
            self._settle(disk)
            for p in new:
                first, last = self._sectors(p["start"], p["size"])
                self.run.run(["sgdisk", f"--new={p['number']}:{first}:{last}",
                              f"--typecode={p['number']}:{p['typecode']}",
                              f"--change-name={p['number']}:{p['partlabel']}", disk])
                self.created_partitions.append(p["number"])
        else:  # free-space
            table = json.loads(self.run.output(["sfdisk", "--json", disk]))["partitiontable"]
            used = set()
            for entry in table.get("partitions", []):
                m = re.search(r"(\d+)$", entry["node"])
                if m:
                    used.add(int(m.group(1)))
            for p in new:
                number = next(n for n in range(1, 129) if n not in used)
                used.add(number)
                first, last = self._sectors(p["start"], p["size"])
                self.run.run(["sgdisk", f"--new={number}:{first}:{last}",
                              f"--typecode={number}:{p['typecode']}",
                              f"--change-name={number}:{p['partlabel']}", disk])
                self.created_partitions.append(number)
                p["number"] = number
                p["device"] = partition_path(disk, number)
        self._settle(disk)
        for p in new:
            self._wait_for(p["device"])
            # Remove stale signatures left at these offsets by a previous layout.
            self.run.run(["wipefs", "--all", "--force", p["device"]])
        self._settle()

    # ------------------------------------------------------------------ encrypt
    def step_encrypt(self):
        luks = self._part("luks")
        lvm = self.plan["lvm"]
        passphrase = self.a.storage.passphrase
        self.rep.info("Creating the encrypted volume (key derivation takes a few seconds)…")
        self.run.run(["cryptsetup", "luksFormat", "--type", "luks2", "--cipher", "aes-xts-plain64",
                      "--key-size", "512", "--hash", "sha512", "--pbkdf", "argon2id",
                      "--label", "nexus-crypt", "--batch-mode", "--key-file", "-", luks["device"]],
                     input=passphrase, redact=True)
        self._progress(0.5)
        self.run.run(["cryptsetup", "open", "--type", "luks2", "--allow-discards", "--key-file", "-",
                      luks["device"], lvm["crypt_name"]], input=passphrase, redact=True)
        self.crypt_open = True
        mapper = f"/dev/mapper/{lvm['crypt_name']}"
        self.run.run(["pvcreate", "-ff", "-y", mapper])
        self.run.run(["vgcreate", lvm["vg"], mapper])
        self.vg_active = True
        for lv in lvm["lvs"]:
            size = ["-l", "100%FREE"] if lv["size"] is None else ["-L", f"{lv['size']}b"]
            self.run.run(["lvcreate", "-y", "--wipesignatures", "y", "-n", lv["name"]] + size + [lvm["vg"]])
            lv["device"] = f"/dev/{lvm['vg']}/{lv['name']}"
        self._settle()

    # ------------------------------------------------------------------ format
    def _mkfs(self, device: str, fmt: str, label: str | None):
        if fmt == "vfat":
            self.run.run(["mkfs.vfat", "-F", "32"] + (["-n", label[:11]] if label else []) + [device])
        elif fmt == "ext4":
            self.run.run(["mkfs.ext4", "-F", "-q"] + (["-L", label] if label else []) + [device])
        elif fmt == "swap":
            self.run.run(["mkswap", "-f"] + (["-L", label] if label else []) + [device])
        else:
            raise InstallError(f"Internal error: unknown filesystem type {fmt!r}.")

    def step_format(self):
        todo = [(p["device"], p["format"], p["label"]) for p in self.plan["partitions"]
                if p["format"] and p["format"] != "luks"]
        if self.plan["lvm"]:
            todo += [(lv["device"], lv["format"], lv["label"]) for lv in self.plan["lvm"]["lvs"]]
        for i, (device, fmt, label) in enumerate(todo):
            self._mkfs(device, fmt, label)
            self._progress((i + 1) / len(todo))
        self._settle()

    def step_keep(self):
        pass

    # ------------------------------------------------------------------ mount
    def _mount_table(self) -> list[dict]:
        """Everything that gets mounted or used, with resolved devices."""
        entries = []
        for p in self.plan["partitions"]:
            if p["mountpoint"] or p["role"] == "swap":
                fstype = p["format"] or p.get("existing_fstype")
                entries.append({"device": p["device"], "mountpoint": p["mountpoint"], "fstype": fstype,
                                "swap": p["role"] == "swap"})
        if self.plan["lvm"]:
            for lv in self.plan["lvm"]["lvs"]:
                entries.append({"device": lv["device"], "mountpoint": lv["mountpoint"], "fstype": lv["format"],
                                "swap": lv["format"] == "swap"})
        return entries

    def step_mount(self):
        os.makedirs(self.target, exist_ok=True)
        if os.path.ismount(self.target):
            raise InstallError(f"{self.target} is already a mount point.",
                               "A previous installation attempt may still be mounted; restart the live session.")
        mounts = sorted((e for e in self._mount_table() if e["mountpoint"]),
                        key=lambda e: e["mountpoint"].count("/") if e["mountpoint"] != "/" else 0)
        for e in mounts:
            where = self.target + ("" if e["mountpoint"] == "/" else e["mountpoint"])
            os.makedirs(where, exist_ok=True)
            opts = ["-o", "umask=0077"] if e["fstype"] == "vfat" else []
            self.run.run(["mount"] + opts + [e["device"], where])
            self.mounted.append(where)

    # ------------------------------------------------------------------ copy
    def step_copy(self):
        squashfs = self.plan["source"]["squashfs"]
        if not squashfs or not os.path.isfile(squashfs):
            raise InstallError("The system image is no longer available.",
                               "Was the installation medium removed?")
        last = [-1]

        def on_line(line: str):
            token = line.strip().rstrip("%")
            if token.isdigit():
                pct = int(token)
                if pct != last[0]:
                    last[0] = pct
                    self._progress(pct / 100.0, f"Copying files ({pct}%)")

        self.run.stream(["unsquashfs", "-f", "-percentage", "-d", self.target, squashfs], on_line)
        for d, mode in (("proc", 0o555), ("sys", 0o555), ("dev", 0o755), ("run", 0o755), ("tmp", 0o1777),
                        ("mnt", 0o755), ("media", 0o755)):
            os.makedirs(f"{self.target}/{d}", exist_ok=True)
            os.chmod(f"{self.target}/{d}", mode)
        if not os.path.exists(f"{self.target}/usr/bin/dpkg"):
            raise InstallError("The copied system is incomplete.", "The installation medium may be damaged.")

    # ------------------------------------------------------------------ chroot mounts
    def _enter_target(self):
        if self.chroot_mounts:
            return
        t = self.target
        for argv, where in (
            (["mount", "--rbind", "/dev", f"{t}/dev"], f"{t}/dev"),
            (["mount", "-t", "proc", "proc", f"{t}/proc"], f"{t}/proc"),
            (["mount", "-t", "sysfs", "sysfs", f"{t}/sys"], f"{t}/sys"),
            (["mount", "--bind", "/run", f"{t}/run"], f"{t}/run"),
        ):
            self.run.run(argv)
            self.chroot_mounts.append(where)
        self.run.run(["mount", "--make-rslave", f"{t}/dev"], check=False)
        if os.path.isdir("/sys/firmware/efi/efivars"):
            if self.run.run(["mount", "-t", "efivarfs", "efivarfs", f"{t}/sys/firmware/efi/efivars"],
                            check=False).returncode == 0:
                self.chroot_mounts.append(f"{t}/sys/firmware/efi/efivars")
        # Never let package scripts start services in the target.
        self._write("/usr/sbin/policy-rc.d", "#!/bin/sh\nexit 101\n", 0o755)

    def _leave_target(self):
        try:
            os.unlink(f"{self.target}/usr/sbin/policy-rc.d")
        except OSError:
            pass
        for where in reversed(self.chroot_mounts):
            flags = ["-R"] if where.endswith("/dev") else []
            self.run.run(["umount"] + flags + [where], check=False)
        self.chroot_mounts.clear()

    # ------------------------------------------------------------------ configure
    def step_configure(self):
        s = self.plan["system"]
        self._enter_target()

        # fstab
        lines = ["# /etc/fstab — written by the NEXUS OS installer.",
                 "# <file system>  <mount point>  <type>  <options>  <dump>  <pass>"]
        for e in sorted((x for x in self._mount_table() if x["mountpoint"]), key=lambda x: x["mountpoint"]):
            uuid = self._uuid(e["device"])
            if e["mountpoint"] == "/":
                opts, passno = "errors=remount-ro", 1
            elif e["fstype"] == "vfat":
                opts, passno = "umask=0077", 1
            else:
                opts, passno = "defaults", 2
            if e["fstype"] in ("btrfs", "xfs", "f2fs"):
                passno = 0
            if e["fstype"] == "btrfs" or e["fstype"] == "xfs":
                opts = "defaults"
            lines.append(f"UUID={uuid}  {e['mountpoint']}  {e['fstype']}  {opts}  0  {passno}")
        for e in self._mount_table():
            if e["swap"]:
                lines.append(f"UUID={self._uuid(e['device'])}  none  swap  sw  0  0")
        if self.plan["swapfile_bytes"]:
            self._create_swapfile(self.plan["swapfile_bytes"])
            lines.append("/swapfile  none  swap  sw  0  0")
        self._write("/etc/fstab", "\n".join(lines) + "\n")

        if self.plan["lvm"]:
            luks = self._part("luks")
            self._write("/etc/crypttab",
                        "# <target name> <source device> <key file> <options>\n"
                        f"{self.plan['lvm']['crypt_name']} UUID={self._uuid(luks['device'])} none luks,discard\n")

        # Resume device for the initramfs: a swap partition/LV, otherwise none
        # (prevents a 30 s wait for a resume device that does not exist).
        swap_dev = next((e["device"] for e in self._mount_table() if e["swap"]), None)
        self._write("/etc/initramfs-tools/conf.d/resume",
                    f"RESUME=UUID={self._uuid(swap_dev)}\n" if swap_dev else "RESUME=none\n")

        self._write("/etc/hostname", s["hostname"] + "\n")
        self._write("/etc/hosts",
                    "127.0.0.1\tlocalhost\n"
                    f"127.0.1.1\t{s['hostname']}\n\n"
                    "::1\t\tlocalhost ip6-localhost ip6-loopback\n"
                    "ff02::1\t\tip6-allnodes\nff02::2\t\tip6-allrouters\n")

        # locale
        gen = Path(f"{self.target}/etc/locale.gen")
        wanted = {s["locale"], "en_US.UTF-8"}
        text = gen.read_text(encoding="utf-8") if gen.exists() else ""
        out, found = [], set()
        for line in text.splitlines():
            m = re.match(r"^#?\s*(\S+)\s+UTF-8\s*$", line)
            if m and m.group(1) in wanted:
                out.append(f"{m.group(1)} UTF-8")
                found.add(m.group(1))
            else:
                out.append(line)
        out += [f"{loc} UTF-8" for loc in sorted(wanted - found)]
        self._write("/etc/locale.gen", "\n".join(out) + "\n")
        self.chroot(["locale-gen"])
        self._write("/etc/default/locale", f"LANG={s['locale']}\n")

        # time zone
        localtime = Path(f"{self.target}/etc/localtime")
        if localtime.is_symlink() or localtime.exists():
            localtime.unlink()
        os.symlink(f"/usr/share/zoneinfo/{s['timezone']}", localtime)
        self._write("/etc/timezone", s["timezone"] + "\n")

        # keyboard: console, X11/Xwayland default, and the Wayland session default
        self._write("/etc/default/keyboard",
                    "# KEYBOARD CONFIGURATION FILE — written by the NEXUS OS installer.\n"
                    f"XKBMODEL=\"{s['keyboard_model']}\"\nXKBLAYOUT=\"{s['keyboard_layout']}\"\n"
                    f"XKBVARIANT=\"{s['keyboard_variant']}\"\nXKBOPTIONS=\"\"\nBACKSPACE=\"guess\"\n")
        self._write("/etc/X11/xorg.conf.d/00-keyboard.conf",
                    "# Written by the NEXUS OS installer (same format as localectl).\n"
                    "Section \"InputClass\"\n        Identifier \"system-keyboard\"\n"
                    "        MatchIsKeyboard \"on\"\n"
                    f"        Option \"XkbLayout\" \"{s['keyboard_layout']}\"\n"
                    f"        Option \"XkbModel\" \"{s['keyboard_model']}\"\n"
                    + (f"        Option \"XkbVariant\" \"{s['keyboard_variant']}\"\n" if s["keyboard_variant"] else "")
                    + "EndSection\n")
        self._write("/etc/xdg/kxkbrc",
                    "# System default keyboard layout for the Wayland session (users can override it\n"
                    "# in Control Center → Keyboard). Written by the NEXUS OS installer.\n"
                    f"[Layout]\nUse=true\nLayoutList={s['keyboard_layout']}\nVariantList={s['keyboard_variant']}\n"
                    f"Model={s['keyboard_model']}\n")
        self.chroot(["setupcon", "--save-only"], check=False)

        # A unique machine-id is generated on first boot.
        self._write("/etc/machine-id", "")
        self._progress(1.0)

    def _create_swapfile(self, size: int):
        path = f"{self.target}/swapfile"
        self.run.run(["fallocate", "-l", str(size), path])
        os.chmod(path, 0o600)
        self.run.run(["mkswap", "-f", path])

    # ------------------------------------------------------------------ user
    def step_user(self):
        s, u = self.plan["system"], self.a.user
        self._enter_target()
        existing = {line.split(":")[0] for line in read_text(f"{self.target}/etc/group").splitlines() if line}
        groups = [g for g in USER_GROUPS if g in existing]
        if read_text(f"{self.target}/etc/passwd").find(f"\n{s['username']}:") >= 0:
            raise InstallError(f"The user name '{s['username']}' already exists on the system image.")
        self.chroot(["useradd", "--create-home", "--shell", "/bin/bash", "--comment", s["fullname"],
                     "--user-group", "--groups", ",".join(groups), s["username"]])
        if u.password is not None:
            self.chroot(["chpasswd"], input=f"{s['username']}:{u.password}\n", redact=True)
        else:
            self.chroot(["chpasswd", "--encrypted"], input=f"{s['username']}:{u.password_hash}\n", redact=True)
        self.chroot(["passwd", "--lock", "root"], check=False)
        if s["autologin"] and os.path.isdir(f"{self.target}/usr/share/sddm"):
            session = "nexus-horizon" if os.path.exists(
                f"{self.target}/usr/share/wayland-sessions/nexus-horizon.desktop") else ""
            self._write("/etc/sddm.conf.d/20-autologin.conf",
                        f"[Autologin]\nUser={s['username']}\n" + (f"Session={session}\n" if session else "")
                        + "Relogin=false\n")

    # ------------------------------------------------------------------ cleanup
    def step_cleanup(self):
        self._enter_target()
        out = self.chroot(["dpkg-query", "-W", "-f", "${Package}\\t${db:Status-Status}\\n"]).stdout
        installed = {line.split("\t")[0] for line in out.splitlines() if line.endswith("\tinstalled")}
        doomed = [p for p in LIVE_PACKAGES if p in installed]
        if doomed:
            self.rep.info("Removing live-session packages: " + ", ".join(doomed))
            self.chroot(["apt-get", "-y", "-q", "--purge", "remove"] + doomed)
            self.chroot(["apt-get", "-y", "-q", "--purge", "autoremove"])
        shutil.rmtree(f"{self.target}/etc/live", ignore_errors=True)
        for leftover in ("/etc/sddm.conf", "/var/lib/live"):
            path = f"{self.target}{leftover}"
            if os.path.isdir(path):
                shutil.rmtree(path, ignore_errors=True)
            elif os.path.exists(path) and "live" in read_text(path).lower():
                os.unlink(path)

    # ------------------------------------------------------------------ initramfs
    def step_initramfs(self):
        self._enter_target()
        kernels = sorted(p.name[len("vmlinuz-"):] for p in Path(f"{self.target}/boot").glob("vmlinuz-*"))
        if not kernels:
            raise InstallError("No kernel was found on the installed system.")
        for i, kver in enumerate(kernels):
            self.chroot(["update-initramfs", "-c", "-k", kver])
            self._progress((i + 1) / len(kernels))
            if not os.path.exists(f"{self.target}/boot/initrd.img-{kver}"):
                raise InstallError(f"The initial RAM disk for kernel {kver} was not created.")

    # ------------------------------------------------------------------ bootloader
    def step_bootloader(self):
        b, s = self.plan["bootloader"], self.plan["system"]
        self._enter_target()
        fw = self.plan["firmware"]
        esp = next((p for p in self.plan["partitions"] if p["mountpoint"] == "/boot/efi"), None)

        conf = ["# NEXUS OS boot loader installation record — written by the installer and",
                "# read by nexus-grub-install, which re-installs GRUB after boot-loader",
                "# package updates. See nexus-grub-install(8).",
                f"FIRMWARE={fw}"]
        if esp:
            conf += ["ESP_MOUNT=/boot/efi",
                     f"ESP_REMOVABLE={'yes' if b['removable'] else 'no'}",
                     f"ESP_NVRAM={'yes' if self.report['firmware']['efivars_writable'] else 'no'}"]
        if b["bios_disk"]:
            conf.append(f"BIOS_DISK={self._stable_disk_path(b['bios_disk'])}")
        elif self.plan["mode"] == "erase-disk" and self._part("bios_grub"):
            # The disk carries a BIOS boot partition: install the legacy loader as
            # well, so the disk still starts if the firmware is switched to CSM.
            conf.append(f"BIOS_DISK_OPTIONAL={self._stable_disk_path(self.plan['target_disk'])}")
        self._write("/etc/nexus/boot.conf", "\n".join(conf) + "\n")

        extra = []
        if s["extra_kernel_cmdline"]:
            extra.append(f'GRUB_CMDLINE_LINUX="$GRUB_CMDLINE_LINUX {s["extra_kernel_cmdline"]}"')
            if "console=ttyS" in s["extra_kernel_cmdline"]:
                extra += ['GRUB_TERMINAL="console serial"',
                          'GRUB_SERIAL_COMMAND="serial --unit=0 --speed=115200 --word=8 --parity=no --stop=1"']
        extra.append(f"GRUB_DISABLE_OS_PROBER={'false' if b['os_prober'] else 'true'}")
        self._write("/etc/default/grub.d/60-nexus-installer.cfg",
                    "# Settings chosen at installation time (NEXUS OS installer).\n" + "\n".join(extra) + "\n")

        if not os.path.exists(f"{self.target}/usr/sbin/nexus-grub-install"):
            raise InstallError("nexus-grub-install is missing from the installed system.",
                               "The installation medium is incomplete (package nexus-boot).")
        self.chroot(["nexus-grub-install", "--verbose"])
        self._progress(0.6)
        self.chroot(["update-grub"])
        if not os.path.exists(f"{self.target}/boot/grub/grub.cfg"):
            raise InstallError("The boot menu (grub.cfg) was not generated.")

    def _stable_disk_path(self, disk: str) -> str:
        """Prefer /dev/disk/by-id: kernel names (sda, nvme0n1) can change between boots."""
        real = os.path.realpath(disk)
        by_id = Path("/dev/disk/by-id")
        if by_id.is_dir():
            candidates = sorted(p for p in by_id.iterdir()
                                if os.path.realpath(p) == real and "-part" not in p.name)
            preferred = [p for p in candidates if not p.name.startswith(("wwn-", "nvme-eui.", "lvm-", "dm-"))]
            if preferred or candidates:
                return str((preferred or candidates)[0])
        return disk

    # ------------------------------------------------------------------ profiles
    def step_profiles(self):
        self._enter_target()
        resolv = f"{self.target}/etc/resolv.conf"
        backup = None
        try:
            if os.path.lexists(resolv):
                backup = resolv + ".nexus-installer"
                os.rename(resolv, backup)
            shutil.copyfile("/etc/resolv.conf", resolv)
            self.chroot(["apt-get", "-q", "update"])
            self._progress(0.15)
            pkgs = [f"nexus-arsenal-{name}" for name in self.plan["profiles"]["names"]]
            # The profile metapackages list their tools as Recommends (so a
            # single tool can be removed later without removing the profile):
            # recommended packages must be installed here.
            self.chroot(["apt-get", "-y", "-q", "--install-recommends", "install"] + pkgs)
            self.chroot(["apt-get", "-q", "clean"])
        except InstallError as exc:
            # Tool profiles are optional and need the network: do not fail the
            # whole installation over them; the user can install them later.
            self.rep.warning("The selected tool profiles could not be installed now: "
                             f"{str(exc).splitlines()[0]} They can be installed after the first start "
                             "from NEXUS Arsenal.")
        finally:
            if os.path.lexists(resolv):
                os.unlink(resolv)
            if backup:
                os.rename(backup, resolv)

    # ------------------------------------------------------------------ finish
    def step_finish(self):
        log_dst = Path(f"{self.target}/var/log/installer")
        log_dst.mkdir(parents=True, exist_ok=True)
        os.chmod(log_dst, 0o700)
        if self.rep.log_path and self.rep.log_path.exists():
            shutil.copyfile(self.rep.log_path, log_dst / "install.log")
            os.chmod(log_dst / "install.log", 0o600)
        safe_plan = {k: v for k, v in self.plan.items()}
        (log_dst / "plan.json").write_text(json.dumps(safe_plan, indent=2, sort_keys=True))
        self.run.run(["sync"])
        self.teardown(strict=True)

    # ------------------------------------------------------------------ teardown
    def teardown(self, strict: bool):
        self._leave_target()
        for where in reversed(self.mounted):
            rc = self.run.run(["umount", where], check=False).returncode
            if rc != 0:
                time.sleep(2)
                rc = self.run.run(["umount", where], check=False).returncode
            if rc != 0 and strict:
                raise InstallError(f"Could not unmount {where}.",
                                   "The system was installed, but do not power off until it is unmounted: "
                                   "run 'sync' and use the Restart button.")
        self.mounted.clear()
        if self.vg_active:
            self.run.run(["vgchange", "-an", self.plan["lvm"]["vg"]], check=False)
            self.vg_active = False
        if self.crypt_open:
            self.run.run(["cryptsetup", "close", self.plan["lvm"]["crypt_name"]], check=False)
            self.crypt_open = False

    def abort_cleanup(self):
        """Best-effort cleanup after a failure; never raises."""
        try:
            self.teardown(strict=False)
            if self.plan["mode"] == "free-space" and self.created_partitions:
                disk = self.plan["target_disk"]
                for number in reversed(self.created_partitions):
                    self.run.run(["sgdisk", f"--delete={number}", disk], check=False)
                self._settle(disk)
                self.rep.info("The partitions created by this attempt were removed again; existing "
                              "partitions were not modified.")
        except Exception as exc:  # noqa: BLE001 — cleanup must not mask the original error
            self.rep.log(f"cleanup error: {exc!r}")
