# SPDX-License-Identifier: GPL-3.0-or-later
"""Command execution, logging and progress events."""
from __future__ import annotations

import json
import os
import shlex
import subprocess
import sys
import time
from pathlib import Path

LOG_DIR = Path("/var/log/nexus-installer")


class InstallError(Exception):
    """A failure the user should see. 'hint' says what they can do about it."""

    def __init__(self, message: str, hint: str = ""):
        super().__init__(message)
        self.hint = hint


class Reporter:
    """Writes the detailed log file and emits JSON-lines progress events."""

    def __init__(self, events=sys.stdout, log_path: Path | None = None):
        self._events = events
        self._log = None
        self.log_path = log_path
        if log_path is not None:
            log_path.parent.mkdir(parents=True, exist_ok=True)
            self._log = open(log_path, "a", buffering=1, encoding="utf-8", errors="replace")
            os.chmod(log_path, 0o600)

    def log(self, text: str):
        if self._log:
            stamp = time.strftime("%Y-%m-%dT%H:%M:%S")
            for line in text.rstrip("\n").split("\n"):
                self._log.write(f"{stamp} {line}\n")

    def event(self, kind: str, **fields):
        fields = {"event": kind, **fields}
        self.log("EVENT " + json.dumps(fields, sort_keys=True))
        if self._events is not None:
            self._events.write(json.dumps(fields) + "\n")
            self._events.flush()

    def info(self, message: str):
        self.event("log", level="info", message=message)

    def warning(self, message: str):
        self.event("log", level="warning", message=message)

    def close(self):
        if self._log:
            self._log.close()
            self._log = None


class Runner:
    """Runs external commands with an argument vector (never a shell).

    Secrets are only ever passed on stdin; 'redact' marks commands whose
    input must not be written to the log.
    """

    def __init__(self, reporter: Reporter):
        self.reporter = reporter

    def run(self, argv: list[str], *, input: str | bytes | None = None, check: bool = True,
            env: dict | None = None, timeout: float | None = None, redact: bool = False,
            cwd: str | None = None) -> subprocess.CompletedProcess:
        self.reporter.log("RUN " + " ".join(shlex.quote(a) for a in argv)
                          + (" <stdin: redacted>" if redact and input is not None else ""))
        full_env = dict(os.environ)
        full_env.update({"LC_ALL": "C.UTF-8", "LANGUAGE": "C", "DEBIAN_FRONTEND": "noninteractive"})
        if env:
            full_env.update(env)
        if isinstance(input, str):
            input = input.encode()
        try:
            proc = subprocess.run(argv, input=input, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  env=full_env, timeout=timeout, cwd=cwd)
        except FileNotFoundError:
            raise InstallError(f"Required program not found: {argv[0]}",
                               "The installation media is incomplete; verify its checksum.")
        except subprocess.TimeoutExpired:
            raise InstallError(f"Command timed out after {timeout:.0f}s: {argv[0]}")
        out = proc.stdout.decode("utf-8", "replace")
        if out.strip():
            self.reporter.log(out)
        self.reporter.log(f"EXIT {proc.returncode}")
        proc.stdout = out
        if check and proc.returncode != 0:
            tail = "\n".join(out.strip().splitlines()[-12:])
            raise InstallError(f"{argv[0]} failed (exit status {proc.returncode}).\n{tail}")
        return proc

    def output(self, argv: list[str], **kw) -> str:
        return self.run(argv, **kw).stdout

    def stream(self, argv: list[str], on_line, *, env: dict | None = None) -> int:
        """Run a long command, feeding each output line to on_line(line)."""
        self.reporter.log("RUN " + " ".join(shlex.quote(a) for a in argv))
        full_env = dict(os.environ)
        full_env.update({"LC_ALL": "C.UTF-8", "LANGUAGE": "C", "DEBIAN_FRONTEND": "noninteractive"})
        if env:
            full_env.update(env)
        try:
            proc = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=full_env)
        except FileNotFoundError:
            raise InstallError(f"Required program not found: {argv[0]}")
        tail: list[str] = []
        assert proc.stdout is not None
        buf = b""
        while True:
            chunk = proc.stdout.read1(4096)
            if not chunk:
                break
            buf += chunk
            # progress bars use carriage returns; treat them as line ends
            parts = buf.replace(b"\r", b"\n").split(b"\n")
            buf = parts.pop()
            for raw in parts:
                line = raw.decode("utf-8", "replace").rstrip()
                if not line:
                    continue
                tail.append(line)
                del tail[:-40]
                on_line(line)
        rc = proc.wait()
        self.reporter.log("\n".join(tail[-40:]))
        self.reporter.log(f"EXIT {rc}")
        if rc != 0:
            raise InstallError(f"{argv[0]} failed (exit status {rc}).\n" + "\n".join(tail[-12:]))
        return rc


def human_size(n: int) -> str:
    value = float(n)
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if value < 1024 or unit == "TiB":
            return f"{value:.0f} {unit}" if unit == "B" else f"{value:.1f} {unit}"
        value /= 1024
    return f"{n} B"


def read_text(path: str | Path, default: str = "") -> str:
    try:
        return Path(path).read_text(encoding="utf-8", errors="replace").strip()
    except OSError:
        return default
