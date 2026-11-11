"""QEMU harness for Shadow OS boot, live-session and installer tests.

Standard library only, so it runs on any build host and in CI.

A VM is described by how it boots (UEFI, UEFI with Secure Boot enforced, or
legacy BIOS on amd64), what media and disks are attached, and — for the
serial-console automation — an SMBIOS system serial that the boot menu on the
SHADOW media recognises (see system/boot/grub/live-grub.cfg.in). Everything the
guest prints on its first serial port is captured to a log file and is also
available for pattern matching through :meth:`VM.expect`.

Acceleration: KVM is used when /dev/kvm is usable, otherwise TCG (pure
emulation). Tests record which one was used, because a pass under emulation is
weaker evidence about timing than a pass under KVM, and neither says anything
about real hardware.
"""

from __future__ import annotations

import atexit
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

FIRMWARE = {
    # arch → mode → (code image, vars template)
    "amd64": {
        "uefi": ("/usr/share/OVMF/OVMF_CODE_4M.fd", "/usr/share/OVMF/OVMF_VARS_4M.fd"),
        # Secure Boot enforcing, Microsoft keys pre-enrolled (what a retail PC ships with)
        "uefi-secureboot": ("/usr/share/OVMF/OVMF_CODE_4M.ms.fd", "/usr/share/OVMF/OVMF_VARS_4M.ms.fd"),
        "bios": (None, None),
    },
    "arm64": {
        "uefi": ("/usr/share/AAVMF/AAVMF_CODE.fd", "/usr/share/AAVMF/AAVMF_VARS.fd"),
        "uefi-secureboot": ("/usr/share/AAVMF/AAVMF_CODE.ms.fd", "/usr/share/AAVMF/AAVMF_VARS.ms.fd"),
    },
}

AUTOTEST_SERIAL = "SHADOW-AUTOTEST"
AUTOTEST_SERIAL_INSTALLED = "SHADOW-AUTOTEST-INSTALLED"


class TestFailure(AssertionError):
    pass


def host_arch() -> str:
    m = os.uname().machine
    return {"x86_64": "amd64", "aarch64": "arm64"}.get(m, m)


def kvm_usable(arch: str) -> bool:
    return arch == host_arch() and os.access("/dev/kvm", os.R_OK | os.W_OK)


class VM:
    def __init__(self, *, arch: str = "amd64", firmware: str = "uefi", memory_mb: int = 4096,
                 cpus: int | None = None, name: str = "vm", workdir: str | Path | None = None,
                 smbios_serial: str | None = AUTOTEST_SERIAL, graphics: bool = False,
                 vars_file: str | Path | None = None):
        if firmware not in FIRMWARE.get(arch, {}):
            raise ValueError(f"firmware mode {firmware!r} not available for {arch}")
        self.arch = arch
        self.firmware = firmware
        self.memory_mb = memory_mb
        self.cpus = cpus or min(4, os.cpu_count() or 1)
        self.name = name
        self.smbios_serial = smbios_serial
        self.graphics = graphics
        self._own_workdir = workdir is None
        self.workdir = Path(workdir) if workdir else Path(tempfile.mkdtemp(prefix=f"shadow-{name}-"))
        self.workdir.mkdir(parents=True, exist_ok=True)
        self.serial_log = self.workdir / f"{name}.serial.log"
        self.qemu_log = self.workdir / f"{name}.qemu.log"
        self._serial_path = self.workdir / f"{name}.serial.sock"
        self._qmp_path = self.workdir / f"{name}.qmp.sock"
        self._vars_file = Path(vars_file) if vars_file else None
        self._drives: list[list[str]] = []
        self._extra: list[str] = []
        self._proc: subprocess.Popen | None = None
        self._serial: socket.socket | None = None
        self._qmp: socket.socket | None = None
        self._qmp_buf = b""
        self._buf = ""
        self.accel = "kvm" if kvm_usable(arch) else "tcg"
        self._usb_added = False

    # ---- configuration ---------------------------------------------------
    def add_cdrom(self, iso: str | Path):
        """Attach an image as optical media (El Torito boot path)."""
        if self.arch == "amd64":
            self._drives.append(["-drive", f"file={iso},media=cdrom,readonly=on,if=none,id=cd0",
                                 "-device", "ide-cd,drive=cd0,bus=ide.1"])
        else:
            self._ensure_usb()
            self._drives.append(["-drive", f"file={iso},media=cdrom,readonly=on,if=none,id=cd0,format=raw",
                                 "-device", "usb-storage,drive=cd0,removable=on"])

    def add_usb_stick(self, image: str | Path, *, readonly: bool = True):
        """Attach an image as a USB mass-storage disk (what 'dd to a stick' gives)."""
        self._ensure_usb()
        idx = len(self._drives)
        ro = "on" if readonly else "off"
        self._drives.append(["-drive", f"file={image},if=none,id=usb{idx},format=raw,readonly={ro}",
                             "-device", f"usb-storage,drive=usb{idx},removable=on"])

    def add_disk(self, image: str | Path, *, fmt: str = "qcow2", bus: str = "virtio"):
        idx = len(self._drives)
        if bus == "virtio":
            self._drives.append(["-drive", f"file={image},if=none,id=hd{idx},format={fmt},discard=unmap",
                                 "-device", f"virtio-blk-pci,drive=hd{idx}"])
        elif bus == "nvme":
            self._drives.append(["-drive", f"file={image},if=none,id=hd{idx},format={fmt}",
                                 "-device", f"nvme,drive=hd{idx},serial=SHADOWNVME{idx}"])
        elif bus == "sata":
            self._drives.append(["-drive", f"file={image},if=none,id=hd{idx},format={fmt}",
                                 "-device", f"ide-hd,drive=hd{idx}"])
        else:
            raise ValueError(bus)

    def add_args(self, *args: str):
        self._extra.extend(args)

    def _ensure_usb(self):
        # The controller itself is emitted first in _command(), before any
        # device that needs a USB bus.
        self._usb_added = True

    # ---- lifecycle -----------------------------------------------------------
    def _command(self) -> list[str]:
        qemu = {"amd64": "qemu-system-x86_64", "arm64": "qemu-system-aarch64"}[self.arch]
        if not shutil.which(qemu):
            raise TestFailure(f"{qemu} is not installed")
        cmd = [qemu, "-name", self.name, "-m", str(self.memory_mb), "-smp", str(self.cpus),
               "-nodefaults", "-no-user-config"]
        if self.arch == "amd64":
            cmd += ["-machine", "q35" + (",smm=on" if self.firmware == "uefi-secureboot" else "")]
            # Without KVM the CPU is emulated (TCG). TCG mis-handles fault
            # suppression of AVX2 masked loads, which crashes Qt's and Mesa's
            # software rasterisers in the guest; a model without AVX2 avoids
            # the emulator bug. Real CPUs and KVM guests are not affected.
            cmd += ["-cpu", "host" if self.accel == "kvm" else "Nehalem"]
            if self.firmware == "uefi-secureboot":
                cmd += ["-global", "driver=cfi.pflash01,property=secure,value=on"]
        else:
            cmd += ["-machine", "virt,gic-version=3"]
            cmd += ["-cpu", "host" if self.accel == "kvm" else "max"]
        cmd += ["-accel", "kvm" if self.accel == "kvm" else "tcg,thread=multi"]
        code, vars_tpl = FIRMWARE[self.arch][self.firmware]
        if code:
            for f in (code, vars_tpl):
                if not os.path.exists(f):
                    raise TestFailure(f"firmware image {f} not found (install ovmf / qemu-efi-aarch64)")
            if self._vars_file is None:
                self._vars_file = self.workdir / f"{self.name}.efivars.fd"
            if not self._vars_file.exists():
                shutil.copyfile(vars_tpl, self._vars_file)
                if self.arch == "arm64":
                    # AAVMF flash images must be exactly 64 MiB
                    with open(self._vars_file, "ab") as fh:
                        fh.truncate(64 * 1024 * 1024)
            code_path = code
            if self.arch == "arm64":
                padded = self.workdir / "aavmf-code.fd"
                if not padded.exists():
                    shutil.copyfile(code, padded)
                    with open(padded, "ab") as fh:
                        fh.truncate(64 * 1024 * 1024)
                code_path = str(padded)
            cmd += ["-drive", f"if=pflash,format=raw,unit=0,readonly=on,file={code_path}",
                    "-drive", f"if=pflash,format=raw,unit=1,file={self._vars_file}"]
        if self._usb_added or self.graphics:
            cmd += ["-device", "qemu-xhci,id=xhci"]
        if self.smbios_serial:
            cmd += ["-smbios", f"type=1,serial={self.smbios_serial}"]
        cmd += ["-chardev", f"socket,id=ser0,path={self._serial_path},server=on,wait=off,logfile={self.serial_log},logappend=on",
                "-serial", "chardev:ser0",
                "-qmp", f"unix:{self._qmp_path},server=on,wait=off",
                "-device", "virtio-rng-pci",
                "-netdev", "user,id=net0", "-device", "virtio-net-pci,netdev=net0"]
        if self.graphics:
            cmd += ["-device", "VGA,vgamem_mb=64" if self.arch == "amd64" else "virtio-gpu-pci"]
            cmd += ["-device", "usb-kbd", "-device", "usb-tablet", "-display", "none"]
        else:
            # Legacy BIOS boot loaders drive the screen through VGA BIOS calls;
            # a PC with no display adapter at all is not a configuration that
            # exists outside of emulators, and GRUB stalls on it.
            if self.firmware == "bios":
                cmd += ["-device", "VGA"]
            cmd += ["-display", "none"]
        cmd += self._extra
        for d in self._drives:
            cmd += d
        return cmd

    def start(self):
        for p in (self._serial_path, self._qmp_path):
            if p.exists():
                p.unlink()
        cmd = self._command()
        # Whatever happens to the test, never leave an emulator running: it
        # would keep the disk images locked for the next run.
        atexit.register(self.stop)
        (self.workdir / f"{self.name}.cmdline").write_text(" ".join(cmd) + "\n")
        self._proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL,
                                      stdout=open(self.qemu_log, "ab"), stderr=subprocess.STDOUT)
        deadline = time.time() + 30
        while time.time() < deadline:
            if self._proc.poll() is not None:
                raise TestFailure(f"qemu exited immediately:\n{self.qemu_log.read_text()[-2000:]}")
            if self._serial_path.exists() and self._qmp_path.exists():
                break
            time.sleep(0.1)
        else:
            raise TestFailure("qemu did not create its control sockets")
        self._serial = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._serial.connect(str(self._serial_path))
        self._serial.setblocking(False)
        self._qmp = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._qmp.connect(str(self._qmp_path))
        self._qmp.settimeout(30)
        self._qmp_read()                      # greeting
        self.qmp("qmp_capabilities")
        return self

    def alive(self) -> bool:
        return self._proc is not None and self._proc.poll() is None

    def stop(self):
        if self._proc and self._proc.poll() is None:
            try:
                self.qmp("quit")
            except Exception:
                pass
            try:
                self._proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                self._proc.kill()
                self._proc.wait()
        for s in (self._serial, self._qmp):
            if s:
                try:
                    s.close()
                except OSError:
                    pass
        self._serial = self._qmp = None

    def wait_for_exit(self, timeout: float) -> bool:
        """Wait for the guest to power off / QEMU to exit. Returns True if it did."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            self._pump()
            if not self.alive():
                return True
            time.sleep(0.5)
        return False

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.stop()

    # ---- QMP -----------------------------------------------------------------
    def _qmp_read(self) -> dict:
        while b"\n" not in self._qmp_buf:
            chunk = self._qmp.recv(65536)
            if not chunk:
                raise TestFailure("QMP connection closed")
            self._qmp_buf += chunk
        line, self._qmp_buf = self._qmp_buf.split(b"\n", 1)
        return json.loads(line)

    def qmp(self, command: str, **arguments) -> dict:
        msg = {"execute": command}
        if arguments:
            msg["arguments"] = arguments
        self._qmp.sendall(json.dumps(msg).encode() + b"\n")
        while True:
            resp = self._qmp_read()
            if "return" in resp:
                return resp["return"]
            if "error" in resp:
                raise TestFailure(f"QMP {command} failed: {resp['error']}")

    def screenshot(self, path: str | Path) -> Path:
        """Capture the emulated display as PNG (needs graphics=True)."""
        path = Path(path)
        ppm = path.with_suffix(".ppm")
        self.qmp("screendump", filename=str(ppm))
        time.sleep(0.5)
        if shutil.which("convert"):
            subprocess.run(["convert", str(ppm), str(path)], check=True)
            ppm.unlink()
            return path
        return ppm

    def send_key(self, *keys: str, hold_ms: int = 100):
        """Press a key combination, e.g. send_key('ctrl', 'alt', 'f2')."""
        self.qmp("send-key", keys=[{"type": "qcode", "data": k} for k in keys], **{"hold-time": hold_ms})

    def type_text(self, text: str):
        keymap = {" ": "spc", "\n": "ret", "-": "minus", "=": "equal", ".": "dot", "/": "slash",
                  ",": "comma", ";": "semicolon", "'": "apostrophe", "\\": "backslash",
                  "[": "bracket_left", "]": "bracket_right", "`": "grave_accent", "\t": "tab"}
        shifted = {"_": "minus", "+": "equal", ":": "semicolon", '"': "apostrophe", "?": "slash",
                   "!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7", "*": "8",
                   "(": "9", ")": "0", "<": "comma", ">": "dot", "|": "backslash", "~": "grave_accent",
                   "{": "bracket_left", "}": "bracket_right"}
        for ch in text:
            if ch in keymap:
                self.send_key(keymap[ch])
            elif ch in shifted:
                self.send_key("shift", shifted[ch])
            elif ch.isupper():
                self.send_key("shift", ch.lower())
            else:
                self.send_key(ch)
            time.sleep(0.05)

    def powerdown(self):
        self.qmp("system_powerdown")

    # ---- serial console ---------------------------------------------------------
    def _pump(self):
        if not self._serial:
            return
        while True:
            try:
                data = self._serial.recv(65536)
            except BlockingIOError:
                return
            except OSError:
                return
            if not data:
                return
            self._buf += data.decode("utf-8", "replace")
            if len(self._buf) > 4_000_000:
                self._buf = self._buf[-2_000_000:]

    def send(self, text: str):
        self._serial.setblocking(True)
        try:
            self._serial.sendall(text.encode())
        finally:
            self._serial.setblocking(False)

    def sendline(self, text: str = ""):
        self.send(text + "\r")

    def expect(self, pattern: str, timeout: float = 120, *, fail: str | None = None) -> re.Match:
        """Wait until *pattern* appears on the serial console.

        Output before and including the match is consumed. *fail* is an
        optional pattern that aborts the wait immediately (e.g. a kernel
        panic), so broken boots fail fast instead of timing out.
        """
        rx = re.compile(pattern, re.S)
        frx = re.compile(fail, re.S) if fail else None
        deadline = time.time() + timeout
        while True:
            self._pump()
            m = rx.search(self._buf)
            if m:
                self._buf = self._buf[m.end():]
                return m
            if frx:
                fm = frx.search(self._buf)
                if fm:
                    raise TestFailure(f"[{self.name}] failure pattern {fail!r} matched: "
                                      f"...{self._buf[max(0, fm.start() - 600):fm.end() + 300]}")
            if not self.alive():
                raise TestFailure(f"[{self.name}] QEMU exited while waiting for {pattern!r}; "
                                  f"last output:\n{self.tail()}")
            if time.time() > deadline:
                raise TestFailure(f"[{self.name}] timed out after {timeout:.0f}s waiting for {pattern!r}; "
                                  f"last output:\n{self.tail()}")
            time.sleep(0.2)

    def tail(self, chars: int = 2500) -> str:
        try:
            data = self.serial_log.read_bytes()[-chars:]
            return data.decode("utf-8", "replace")
        except OSError:
            return self._buf[-chars:]

    # ---- shell helpers (need a logged-in shell on the serial console) -------------
    def login(self, user: str, password: str, timeout: float = 600):
        self.expect(r"login: ", timeout, fail=PANIC)
        time.sleep(1)
        self.sendline(user)
        # The prompt is localised on an installed system ("Passwort:", "Mot de passe :" …).
        self.expect(r"(?i)(pass|contrase|parol|senha|wachtwoord|has[lł]o)[^\n:]*: ?$", 60)
        time.sleep(0.5)
        self.sendline(password)
        self.expect(r"[$#] ", 120)
        # A predictable prompt and no line wrapping/colour in command output.
        self.sendline("export PS1='NXSH> ' TERM=dumb; stty cols 500 -echo; bind 'set enable-bracketed-paste off' 2>/dev/null")
        self.expect(r"NXSH> $", 60)

    def run(self, command: str, timeout: float = 300, *, check: bool = True) -> tuple[int, str]:
        """Run a shell command in the logged-in serial shell → (exit status, output)."""
        marker = f"__NX{int(time.time() * 1000) % 100000000}__"
        self._pump()
        self._buf = ""
        self.sendline(f"{{ {command} ; }} 2>&1; echo {marker}$?")
        m = self.expect(rf"(.*?){marker}(\d+)\r?\n", timeout)
        out = m.group(1).replace("\r\n", "\n")
        rc = int(m.group(2))
        self.expect(r"NXSH> $", 60)
        if check and rc != 0:
            raise TestFailure(f"[{self.name}] command failed ({rc}): {command}\n{out[-3000:]}")
        return rc, out

    def shell_poweroff(self, timeout: float = 240) -> bool:
        """Power the guest off from the logged-in shell and wait for QEMU to exit.

        The shell never prints another prompt, so this cannot go through run().
        """
        self.sendline("sudo -n systemctl poweroff")
        return self.wait_for_exit(timeout)

    def shell_poweroff_as(self, password: str, timeout: float = 240) -> bool:
        """Power off an installed system, where sudo asks for the user's password."""
        self.sendline(f"echo '{password}' | sudo -S -p '' systemctl poweroff")
        return self.wait_for_exit(timeout)

    def sudo(self, command: str, timeout: float = 300, *, check: bool = True) -> tuple[int, str]:
        quoted = command.replace("'", "'\"'\"'")
        return self.run(f"sudo -n sh -c '{quoted}'", timeout, check=check)


def _noop():
    pass


PANIC = (r"Kernel panic|---\[ end Kernel panic|Unable to mount root fs|"
         r"emergency mode|Failed to start .*Switch Root|initramfs\) $")


def make_disk(path: str | Path, size: str = "32G") -> Path:
    path = Path(path)
    if path.exists():
        path.unlink()
    subprocess.run(["qemu-img", "create", "-q", "-f", "qcow2", str(path), size], check=True)
    return path


def report(name: str, ok: bool, detail: str = ""):
    mark = "PASS" if ok else "FAIL"
    print(f"{mark}  {name}" + (f"  — {detail}" if detail else ""), flush=True)


def main_guard(fn):
    """Run a test entry point, print a one-line verdict, exit non-zero on failure."""
    try:
        fn()
    except TestFailure as exc:
        print(f"FAIL  {exc}", file=sys.stderr, flush=True)
        sys.exit(1)
