# SPDX-License-Identifier: GPL-3.0-or-later
"""Output protocol, log file and command execution."""
from __future__ import annotations

import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
from pathlib import Path

# Test overrides (a root shell's own choice; pkexec does not pass them on).
STATE_DIR = Path(os.environ.get("SHADOW_RECOVERY_STATE_DIR") or "/run/shadow-recovery")
LOG_DIR = Path(os.environ.get("SHADOW_RECOVERY_LOG_DIR") or "/var/log/shadow-recovery")

SAFE_ENV = {"LC_ALL": "C.UTF-8", "LANGUAGE": "C", "DEBIAN_FRONTEND": "noninteractive",
            "PATH": "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"}

TARGET_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")


class RecoveryError(Exception):
    """A failure the user should see; 'hint' says what they can do about it."""

    def __init__(self, message: str, hint: str = "", code: str = "failed"):
        super().__init__(message)
        self.hint = hint
        self.code = code


class Output:
    """JSON-lines events on stdout plus the log file."""

    def __init__(self, pretty: bool = False):
        self.pretty = pretty
        self._log = None
        self.log_path: Path | None = None

    def open_log(self):
        """Called by every command that changes something."""
        if self._log is not None:
            return
        try:
            LOG_DIR.mkdir(parents=True, exist_ok=True)
            path = LOG_DIR / time.strftime("recovery-%Y%m%d.log")
            fd = os.open(path, os.O_WRONLY | os.O_APPEND | os.O_CREAT | os.O_NOFOLLOW, 0o600)
            self._log = os.fdopen(fd, "a", buffering=1, encoding="utf-8", errors="replace")
            self.log_path = path
        except OSError as exc:
            raise RecoveryError(f"The log file in {LOG_DIR} cannot be written: {exc}",
                                "Nothing was changed. Recovery only works with its log.")
        self.log("==== shadow-recovery-engine " + " ".join(shlex.quote(a) for a in sys.argv[1:]))

    def log(self, text: str):
        if self._log is None:
            return
        stamp = time.strftime("%Y-%m-%dT%H:%M:%S")
        for line in text.rstrip("\n").split("\n"):
            self._log.write(f"{stamp} {line}\n")

    def emit(self, event: str, **fields):
        fields = {"event": event, **fields}
        if event != "output":
            self.log("EVENT " + json.dumps(fields, sort_keys=True))
        if self.pretty and event == "result":
            sys.stdout.write(json.dumps(fields, indent=2) + "\n")
        else:
            sys.stdout.write(json.dumps(fields) + "\n")
        sys.stdout.flush()

    def result(self, ok: bool = True, **fields):
        if self.log_path is not None:
            fields.setdefault("log", str(self.log_path))
        self.emit("result", ok=ok, **fields)


def have(program: str) -> bool:
    return shutil.which(program, path=SAFE_ENV["PATH"]) is not None


def _env(extra: dict | None = None) -> dict:
    env = {k: v for k, v in os.environ.items() if k in ("TERM", "HOME")}
    env.update(SAFE_ENV)
    if extra:
        env.update(extra)
    return env


def run(out: Output | None, argv: list[str], *, input: bytes | None = None, timeout: float | None = 300,
        redact: bool = False) -> tuple[int, str]:
    """Run a command to completion; returns (exit status, combined output).
    A missing program is exit status 127."""
    if out:
        out.log("RUN " + " ".join(shlex.quote(a) for a in argv) + (" <stdin: redacted>" if redact else ""))
    try:
        proc = subprocess.run(argv, input=input, stdin=None if input is not None else subprocess.DEVNULL,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=_env(), timeout=timeout)
        code, text = proc.returncode, proc.stdout.decode("utf-8", "replace")
    except FileNotFoundError:
        code, text = 127, f"{argv[0]}: program not found"
    except subprocess.TimeoutExpired:
        code, text = 124, f"{argv[0]}: timed out after {timeout:.0f} s"
    if out:
        if text.strip():
            out.log(text)
        out.log(f"EXIT {code}")
    return code, text


def stream(out: Output, argv: list[str], *, env: dict | None = None) -> int:
    """Run a long command, passing every output line on as an "output" event."""
    out.log("RUN " + " ".join(shlex.quote(a) for a in argv))
    try:
        proc = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                env=_env(env))
    except FileNotFoundError:
        out.emit("output", line=f"{argv[0]}: program not found")
        out.log("EXIT 127")
        return 127
    assert proc.stdout is not None
    buffer = b""
    while True:
        chunk = proc.stdout.read1(4096)
        if not chunk:
            break
        buffer += chunk
        parts = buffer.replace(b"\r", b"\n").split(b"\n")       # progress bars use carriage returns
        buffer = parts.pop()
        for raw in parts:
            line = raw.decode("utf-8", "replace").rstrip()
            if line:
                out.log(line)
                out.emit("output", line=line)
    if buffer.strip():
        line = buffer.decode("utf-8", "replace").rstrip()
        out.log(line)
        out.emit("output", line=line)
    code = proc.wait()
    out.log(f"EXIT {code}")
    return code


def read_secret(what: str) -> bytes:
    """One secret from stdin: everything up to the first newline (or EOF)."""
    if sys.stdin is None or sys.stdin.isatty():
        raise RecoveryError(f"The {what} must be supplied on standard input.", code="usage")
    data = sys.stdin.buffer.readline(4096)
    secret = data.rstrip(b"\r\n")
    if not secret:
        raise RecoveryError(f"No {what} was supplied on standard input.", code="usage")
    if b"\0" in secret:
        raise RecoveryError(f"The {what} contains a NUL byte.", code="usage")
    return secret


def read_text(path: str | Path, default: str = "") -> str:
    try:
        return Path(path).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return default
