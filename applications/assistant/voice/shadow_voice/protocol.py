"""Newline-delimited JSON on stdout (spec 2.2).

The protocol owns a PRIVATE duplicate of the original stdout; fd 1 itself is then
pointed at stderr (see Emitter.take_stdout). Native libraries (onnxruntime,
sherpa-onnx) and stray print() calls can therefore never corrupt the event
stream: whatever they write ends up in the log instead.

Every event carries "event" and "t" (seconds since the helper started).
"""
import json
import os
import sys
import threading
import time

T0 = time.monotonic()
MAX_COMMAND_BYTES = 1 << 20      # a command line longer than 1 MiB is rejected, not parsed


class Emitter:
    def __init__(self, fd=None):
        self._fd = fd
        self._lock = threading.Lock()
        self.broken = False              # the reader of our stdout went away
        self.on_broken = None            # callback (engine shutdown)
        self.listeners = []              # in-process observers (selftest/tests): fn(dict)
        self.quiet = False               # selftest: collect events without printing them

    @classmethod
    def take_stdout(cls):
        """Keep the real stdout for the protocol; send fd 1 (and sys.stdout) to stderr."""
        sys.stdout.flush()
        fd = os.dup(1)
        os.set_inheritable(fd, False)
        os.dup2(2, 1)
        sys.stdout = sys.stderr
        return cls(fd)

    def emit(self, event, **fields):
        obj = {"event": event, "t": round(time.monotonic() - T0, 3)}
        obj.update(fields)
        for fn in self.listeners:
            fn(obj)
        if self.quiet:
            return obj
        self.write_obj(obj)
        return obj

    def write_obj(self, obj):
        line = (json.dumps(obj, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8", "replace")
        with self._lock:
            if self.broken:
                return
            try:
                if self._fd is None:
                    sys.__stdout__.buffer.write(line)
                    sys.__stdout__.flush()
                else:
                    view = memoryview(line)
                    while view:
                        n = os.write(self._fd, view)
                        view = view[n:]
            except (BrokenPipeError, OSError):
                self.broken = True
                cb = self.on_broken
                if cb:
                    cb()


def parse_command(raw):
    """bytes -> (dict, None) or (None, error message). Never raises."""
    if len(raw) > MAX_COMMAND_BYTES:
        return None, f"command line too long ({len(raw)} bytes)"
    text = raw.decode("utf-8", "replace").strip()
    if not text:
        return None, None
    try:
        msg = json.loads(text)
    except ValueError as e:
        return None, f"bad command (not JSON: {e})"
    if not isinstance(msg, dict) or not isinstance(msg.get("cmd"), str):
        return None, 'bad command (expected a JSON object with a "cmd" string)'
    return msg, None


def as_bool(v, default=None):
    """JSON bool (also "true"/"false"/0/1) -> bool; None when invalid."""
    if isinstance(v, bool):
        return v
    if isinstance(v, (int, float)) and v in (0, 1):
        return bool(v)
    if isinstance(v, str) and v.strip().lower() in ("true", "false", "1", "0", "yes", "no", "on", "off"):
        return v.strip().lower() in ("true", "1", "yes", "on")
    return default


def as_float(v, lo=None, hi=None):
    """Number (or numeric string) clamped to [lo, hi]; None when invalid."""
    if isinstance(v, bool):
        return None
    try:
        x = float(v)
    except (TypeError, ValueError):
        return None
    if x != x or x in (float("inf"), float("-inf")):
        return None
    if lo is not None:
        x = max(lo, x)
    if hi is not None:
        x = min(hi, x)
    return x
