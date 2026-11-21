"""Test harness for shadow-voiced: locate the helper and an engine data root, generate
spoken test fixtures with the engine's own TTS, and drive `--serve` over its protocol.

Environment:
  SHADOW_VOICED       helper to test (default: ../shadow-voiced next to this directory)
  SHADOW_VOICE_DATA   data root (default: the helper's normal lookup order)
  VOICE_TEST_TMP      where fixtures are generated (default: a fresh temp dir; never the repo)

Fixtures are synthesised once per test run (Kokoro voices -> 16 kHz, -26 dBFS speech,
light noise at -62 dBFS between phrases) and cached in the temp dir.
"""
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
VOICE_DIR = os.path.dirname(HERE)
VOICED = os.environ.get("SHADOW_VOICED") or os.path.join(VOICE_DIR, "shadow-voiced")
SETUP = os.environ.get("SHADOW_VOICE_SETUP") or os.path.join(VOICE_DIR, "shadow-voice-setup")
sys.path.insert(0, VOICE_DIR)

from shadow_voice import datadir  # noqa: E402

_ROOT, _ = datadir.find_data_root(os.environ.get("SHADOW_VOICE_DATA"))
DATA = _ROOT.path if _ROOT else None
TMP = os.environ.get("VOICE_TEST_TMP") or tempfile.mkdtemp(prefix="shadow-voice-tests-")
os.makedirs(TMP, exist_ok=True)

_np = None
_engine = None
_lock = threading.Lock()


def need_data(test):
    """Decorator: skip when no engine data root is installed."""
    return unittest.skipUnless(DATA, "no shadow-voice data root installed (run shadow-voice-setup)")(test)


def runtime():
    """numpy + the shadow_voice heavy modules from the data root's runtime/."""
    global _np
    if _np is None:
        datadir.activate_runtime(_ROOT)
        import numpy
        _np = numpy
    return _np


def tts():
    global _engine
    with _lock:
        if _engine is None:
            runtime()
            from shadow_voice.tts import KokoroEngine
            _engine = KokoroEngine(_ROOT, threads=2)
        return _engine


def phrase(text, voice="af_heart", speed=1.0):
    """Speech for `text` as float32 16 kHz at -26 dBFS (cached)."""
    np = runtime()
    from shadow_voice import audio
    key = f"{voice}-{speed}-" + "".join(c if c.isalnum() else "_" for c in text)[:60]
    path = os.path.join(TMP, f"phrase-{key}.npy")
    if os.path.exists(path):
        return np.load(path)
    e = tts()
    pieces = [e.synth(s, voice=voice, speed=speed) for s in e.sentences(text, voice=voice)]
    x = np.concatenate(pieces)
    y = audio.resample(x, e.sample_rate, 16000)
    y = y * (10 ** (-26 / 20) / (float(np.sqrt(np.mean(y * y))) + 1e-9))
    np.save(path, y)
    return y


def session(name, parts, noise_db=-62.0):
    """Write a 16 kHz mono WAV from [(seconds of silence) | (text, voice)], return (path, timeline).

    timeline: [(start_s, end_s, text)] for every spoken part."""
    np = runtime()
    from shadow_voice import audio
    path = os.path.join(TMP, f"session-{name}.wav")
    rng = np.random.default_rng(abs(hash(name)) % (2 ** 32))
    chunks, timeline, t = [], [], 0.0
    for p in parts:
        if isinstance(p, (int, float)):
            n = int(p * 16000)
            chunks.append(rng.normal(0, 10 ** (noise_db / 20), n).astype(np.float32))
            t += p
        else:
            text, voice = p if isinstance(p, tuple) else (p, "af_heart")
            y = phrase(text, voice)
            y = y + rng.normal(0, 10 ** (noise_db / 20), len(y)).astype(np.float32)
            chunks.append(y)
            timeline.append((round(t, 2), round(t + len(y) / 16000, 2), text))
            t += len(y) / 16000
    audio.write_wav(path, audio.to_pcm16(np.concatenate(chunks)), 16000)
    return path, timeline


def wav_seconds(path):
    with wave.open(path) as w:
        return w.getnframes() / w.getframerate()


class Helper:
    """A running `shadow-voiced --serve`; collects every event (with arrival time)."""

    def __init__(self, *args, env=None, data=None, log_name="helper"):
        self.argv = [VOICED, "--serve", "--no-log-file"] + (["--data", data or DATA] if (data or DATA) else []) \
            + list(args)
        self.err_path = os.path.join(TMP, f"{log_name}-{int(time.time() * 1000)}.err")
        self._err = open(self.err_path, "wb")
        self.p = subprocess.Popen(self.argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self._err,
                                  env=env)
        self.events = []
        self.cv = threading.Condition()
        self.eof = False
        self.t0 = time.monotonic()
        self._reader = threading.Thread(target=self._read, daemon=True)
        self._reader.start()

    def _read(self):
        for line in self.p.stdout:
            try:
                ev = json.loads(line)
            except ValueError:
                ev = {"event": "_garbage", "line": line.decode("utf-8", "replace")}
            ev["_wall"] = time.monotonic() - self.t0
            with self.cv:
                self.events.append(ev)
                self.cv.notify_all()
        with self.cv:
            self.eof = True
            self.cv.notify_all()

    def send(self, **cmd):
        self.p.stdin.write((json.dumps(cmd) + "\n").encode())
        self.p.stdin.flush()

    def wait(self, event, timeout=30.0, start=0, **match):
        """First event named `event` at index >= start whose fields match; raises on timeout."""
        end = time.monotonic() + timeout
        with self.cv:
            while True:
                for i in range(start, len(self.events)):
                    ev = self.events[i]
                    if ev["event"] == event and all(ev.get(k) == v for k, v in match.items()):
                        ev["_index"] = i
                        return ev
                left = end - time.monotonic()
                if left <= 0 or self.eof:
                    names = [e["event"] for e in self.events[start:] if e["event"] != "level"]
                    raise AssertionError(f"no {event} {match or ''} within {timeout} s; got {names[-40:]}; "
                                         f"stderr: {self.stderr_tail()}")
                self.cv.wait(min(left, 0.2))

    def ready(self, timeout=60):
        return self.wait("ready", timeout=timeout)

    def named(self, event, start=0, **match):
        with self.cv:
            return [e for e in self.events[start:] if e["event"] == event
                    and all(e.get(k) == v for k, v in match.items())]

    def mark(self):
        with self.cv:
            return len(self.events)

    def quiet_for(self, seconds, event, start=None, **match):
        """Assert that no matching event arrives during `seconds`."""
        s = self.mark() if start is None else start
        time.sleep(seconds)
        got = self.named(event, s, **match)
        if got:
            raise AssertionError(f"unexpected {event}: {got}")

    def stderr_tail(self, n=12):
        try:
            with open(self.err_path, "rb") as f:
                return " | ".join(f.read().decode("utf-8", "replace").splitlines()[-n:])
        except OSError:
            return ""

    def close(self, timeout=10):
        if self.p.poll() is None:
            try:
                self.send(cmd="quit")
            except (BrokenPipeError, OSError):
                pass
            try:
                self.p.wait(timeout)
            except subprocess.TimeoutExpired:
                self.p.kill()
                self.p.wait(5)
        self._reader.join(5)
        for f in (self.p.stdin, self.p.stdout):
            try:
                f.close()
            except (BrokenPipeError, OSError):
                pass
        self._err.close()
        return self.p.returncode

    def kill(self, sig=signal.SIGKILL):
        self.p.send_signal(sig)


def processes_with(marker, exclude=()):
    """PIDs whose command line contains `marker` (used to find fake capture children).
    `exclude`: PIDs to ignore (the helper itself has the marker in its --capture-command)."""
    pids = []
    for d in os.listdir("/proc"):
        if not d.isdigit() or int(d) in exclude or int(d) == os.getpid():
            continue
        try:
            with open(f"/proc/{d}/cmdline", "rb") as f:
                if marker.encode() in f.read():
                    pids.append(int(d))
        except OSError:
            pass
    return pids


def fake_mic(marker, script):
    """A --capture-command running a Python one-off that writes 16 kHz s16le to stdout."""
    path = os.path.join(TMP, f"fake-mic-{marker}.py")
    with open(path, "w") as f:
        f.write(f"# {marker}\n" + script)
    return f"exec {shutil.which('python3') or '/usr/bin/python3'} -I {path} {marker}"
