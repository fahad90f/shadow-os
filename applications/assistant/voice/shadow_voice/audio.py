"""Audio in/out through the desktop's own tools, plus WAV helpers and level meters.

Capture  : pw-record -> parec -> arecord   (16 kHz s16le mono raw on a pipe)
Playback : pw-play   -> pacat -> aplay     (24 kHz s16le mono raw on a pipe)

Why external tools and not a Python audio library: they are what Debian ships with
PipeWire (pipewire-bin), PulseAudio (pulseaudio-utils) and ALSA (alsa-utils), they
honour the desktop's default devices, routing and mic privacy indicator, and a
wedged audio server can only ever hang a child process we can kill, never the
helper itself.

Every child is started through `setpriv --pdeathsig TERM` (util-linux, Essential
in Debian), so it dies with the helper even when the helper is SIGKILLed. The
death signal belongs to the THREAD that forked the child, so children are only
spawned from long-lived threads (capture supervisor, speaker).
"""
import logging
import os
import re
import select
import shutil
import signal
import subprocess
import threading
import time
import wave

import numpy as np

from . import FRAME, MIC_RATE

log = logging.getLogger("shadow-voiced")

CAPTURE_TOOLS = ("pw-record", "parec", "arecord")
PLAYBACK_TOOLS = ("pw-play", "pacat", "aplay")
APP_NAME = "Shadow Assistant"

# An ALSA PCM name ("hw:1,0", "plughw:CARD=PCH,DEV=0", "default", "sysdefault:CARD=x", "pulse")
# as opposed to a PipeWire/PulseAudio node name ("alsa_input.pci-0000_00_1f.3.analog-stereo").
_ALSA_PCM = re.compile(r"^(?:hw|plughw|sysdefault|front|dsnoop|dmix|default|pulse|pipewire|null|iec958|surround\d+)"
                       r"(?::.*)?$")


def is_alsa_pcm(name):
    return bool(name) and bool(_ALSA_PCM.match(name))


def available(tools, path=None):
    return [t for t in tools if shutil.which(t, path=path)]


def capture_order(device="", forced="auto", path=None):
    """Capture tools to try, best first. An ALSA PCM name can only be opened by arecord."""
    tools = available(CAPTURE_TOOLS, path) if forced in ("", "auto") else available([forced], path)
    if device and is_alsa_pcm(device) and "arecord" in tools:
        tools = ["arecord"] + [t for t in tools if t != "arecord"]
    return tools


def capture_argv(tool, device="", rate=MIC_RATE):
    """argv for 16 kHz s16le mono on stdout. input_device is mapped per tool:
    pw-record --target NODE, parec --device=SOURCE, arecord -D PCM."""
    dev = device or ""
    if tool == "pw-record":
        a = ["pw-record", "--raw", "--rate", str(rate), "--channels", "1", "--format", "s16", "--latency", "20ms",
             "-P", '{ application.name = "%s" media.role = "Communication" node.description = "%s (voice)" }'
             % (APP_NAME, APP_NAME)]
        if dev and not is_alsa_pcm(dev):
            a += ["--target", dev]
        return a + ["-"]
    if tool == "parec":
        a = ["parec", "--raw", "--rate=%d" % rate, "--channels=1", "--format=s16le", "--latency-msec=20",
             "--client-name=" + APP_NAME, "--stream-name=voice input"]
        if dev and not is_alsa_pcm(dev):
            a.append("--device=" + dev)
        return a
    if tool == "arecord":
        a = ["arecord", "-q", "-t", "raw", "-f", "S16_LE", "-r", str(rate), "-c", "1", "-B", "100000"]
        if dev and is_alsa_pcm(dev):
            a += ["-D", dev]
        return a + ["-"]
    raise ValueError(f"unknown capture tool {tool}")


def device_note(tool, device):
    """Why a configured input_device cannot be honoured by this tool ('' when it is)."""
    if not device:
        return ""
    if tool == "arecord" and not is_alsa_pcm(device):
        return f"arecord cannot open the PipeWire/PulseAudio source '{device}'; using the default ALSA input"
    if tool in ("pw-record", "parec") and is_alsa_pcm(device):
        return f"{tool} cannot open the ALSA device '{device}'; using the default input"
    return ""


def device_exists(tool, device):
    """Does the PipeWire node / PulseAudio source `device` exist? True, False or None (cannot tell).

    pw-record and parec quietly fall back to the default input when a --target/--device is
    unknown; asking first lets the helper say so instead of claiming to use the device."""
    if not device or is_alsa_pcm(device):
        return None
    try:
        if tool == "pw-record" and shutil.which("pw-dump"):
            import json
            r = subprocess.run(["pw-dump"], capture_output=True, text=True, timeout=3)
            if r.returncode != 0:
                return None
            for obj in json.loads(r.stdout or "[]"):
                if obj.get("type") != "PipeWire:Interface:Node":
                    continue
                props = (obj.get("info") or {}).get("props") or {}
                if device in (props.get("node.name"), str(props.get("object.serial")), str(obj.get("id"))):
                    return True
            return False
        if tool == "parec" and shutil.which("pactl"):
            r = subprocess.run(["pactl", "list", "short", "sources"], capture_output=True, text=True, timeout=3)
            if r.returncode != 0:
                return None
            names = {ln.split("\t")[1] for ln in r.stdout.splitlines() if ln.count("\t") >= 1}
            ids = {ln.split("\t")[0] for ln in r.stdout.splitlines() if ln.strip()}
            return device in names or device in ids
    except (OSError, subprocess.SubprocessError, ValueError) as e:
        log.info("cannot list audio devices: %s", e)
    return None


def playback_argv(tool, rate, device=""):
    if tool == "pw-play":
        a = ["pw-play", "--raw", "--rate", str(rate), "--channels", "1", "--format", "s16", "--latency", "40ms",
             "-P", '{ application.name = "%s" media.role = "Assistant" node.description = "%s (speech)" }'
             % (APP_NAME, APP_NAME)]
        if device and not is_alsa_pcm(device):
            a += ["--target", device]
        return a + ["-"]
    if tool == "pacat":
        a = ["pacat", "--playback", "--raw", "--rate=%d" % rate, "--channels=1", "--format=s16le",
             "--latency-msec=40", "--client-name=" + APP_NAME, "--stream-name=speech"]
        if device and not is_alsa_pcm(device):
            a.append("--device=" + device)
        return a
    if tool == "aplay":
        a = ["aplay", "-q", "-t", "raw", "-f", "S16_LE", "-r", str(rate), "-c", "1"]
        if device and is_alsa_pcm(device):
            a += ["-D", device]
        return a + ["-"]
    raise ValueError(f"unknown playback tool {tool}")


_SETPRIV = shutil.which("setpriv")


def spawn(argv, stdin=None, stdout=None, stderr=None, env=None):
    """Popen with a parent-death signal (see module doc); never inherits our protocol fd."""
    full = ([_SETPRIV, "--pdeathsig", "TERM", "--"] + argv) if _SETPRIV else argv
    return subprocess.Popen(full, stdin=stdin, stdout=stdout, stderr=stderr, env=env, close_fds=True)


def stop_process(p, grace=0.5):
    """SIGTERM, then SIGKILL after `grace` seconds; always reaps. Returns the exit code."""
    if p is None:
        return None
    if p.poll() is None:
        try:
            p.send_signal(signal.SIGTERM)
            p.wait(grace)
        except subprocess.TimeoutExpired:
            p.kill()
            try:
                p.wait(2)
            except subprocess.TimeoutExpired:
                log.error("child %s did not die after SIGKILL", p.pid)
        except OSError:
            pass
    return p.returncode


# --------------------------------------------------------------------------- levels
def dbfs(rms):
    return 20.0 * np.log10(max(float(rms), 1e-9))


def level_from_rms(rms):
    """Linear RMS (0..1 full scale) -> perceptual 0..1 for the UI: -60 dBFS -> 0, -12 dBFS -> 1."""
    return round(float(min(1.0, max(0.0, (dbfs(rms) + 60.0) / 48.0))), 3)


def frame_stats(pcm16):
    """(rms, ac_rms, peak) of an int16 frame, all relative to full scale."""
    x = pcm16.astype(np.float32) / 32768.0
    if not len(x):
        return 0.0, 0.0, 0.0
    rms = float(np.sqrt(np.mean(x * x)))
    ac = float(np.std(x))
    peak = float(np.max(np.abs(x)))
    return rms, ac, peak


# Below this AC level (about two LSB of 16-bit audio) a "microphone" delivers digital silence:
# a muted source, a disconnected/wrong input, or a privacy switch. Real mics in a quiet room
# sit around -75..-50 dBFS.
SILENT_DBFS = -84.0
SILENT_AC = 10 ** (SILENT_DBFS / 20.0)


# --------------------------------------------------------------------------- WAV
def resample(x, sr_in, sr_out):
    """Band-limited (FFT) resampling of a float32 mono signal."""
    if sr_in == sr_out or len(x) == 0:
        return x.astype(np.float32, copy=False)
    n = int(round(len(x) * sr_out / sr_in))
    X = np.fft.rfft(x.astype(np.float64))
    Y = np.zeros(n // 2 + 1, dtype=np.complex128)
    m = min(len(X), len(Y))
    Y[:m] = X[:m]
    return (np.fft.irfft(Y, n) * (n / len(x))).astype(np.float32)


def read_wav(path, rate=MIC_RATE):
    """Any PCM WAV (8/16/24/32-bit, any rate, mono/stereo) -> float32 mono at `rate`."""
    with wave.open(path, "rb") as w:
        sr, ch, sw, n = w.getframerate(), w.getnchannels(), w.getsampwidth(), w.getnframes()
        raw = w.readframes(n)
    if sw == 2:
        x = np.frombuffer(raw, "<i2").astype(np.float32) / 32768.0
    elif sw == 1:
        x = (np.frombuffer(raw, np.uint8).astype(np.float32) - 128.0) / 128.0
    elif sw == 3:
        b = np.frombuffer(raw, np.uint8).reshape(-1, 3)
        v = (b[:, 0].astype(np.int32) | (b[:, 1].astype(np.int32) << 8) | (b[:, 2].astype(np.int32) << 16))
        v = np.where(v >= 1 << 23, v - (1 << 24), v)
        x = v.astype(np.float32) / float(1 << 23)
    elif sw == 4:
        x = np.frombuffer(raw, "<i4").astype(np.float32) / float(1 << 31)
    else:
        raise ValueError(f"{path}: unsupported sample width {sw}")
    if ch > 1:
        x = x.reshape(-1, ch).mean(axis=1)
    return resample(x, sr, rate), sr


def to_pcm16(x):
    return (np.clip(x, -1.0, 1.0) * 32767.0).astype("<i2")


def write_wav(path, pcm16, rate):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(np.asarray(pcm16, "<i2").tobytes())


# --------------------------------------------------------------------------- capture sources
class CaptureError(Exception):
    pass


class ProcessCapture:
    """A running capture tool. read_frame() returns FRAME int16 samples, None at EOF,
    or raises CaptureError when no audio arrived for `stall_s` seconds."""

    def __init__(self, tool, argv, stall_s=3.0, env=None):
        self.tool = tool
        self.argv = argv
        self.stall_s = stall_s
        self.p = spawn(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        self.fd = self.p.stdout.fileno()
        self.err_fd = self.p.stderr.fileno()
        os.set_blocking(self.err_fd, False)
        self._buf = bytearray()
        self._err = bytearray()
        self.started = time.monotonic()
        self.last_data = None
        self.frames = 0

    def _drain_stderr(self):
        try:
            while True:
                chunk = os.read(self.err_fd, 4096)
                if not chunk:
                    break
                self._err += chunk
                del self._err[:-2000]
        except (BlockingIOError, OSError):
            pass

    def stderr_tail(self):
        self._drain_stderr()
        lines = [ln.strip() for ln in self._err.decode("utf-8", "replace").splitlines() if ln.strip()]
        return " | ".join(lines[-3:])[:300]

    def read_frame(self, stop_event=None):
        need = FRAME * 2
        while len(self._buf) < need:
            if stop_event is not None and stop_event.is_set():
                return None
            ready, _, _ = select.select([self.fd, self.err_fd], [], [], 0.25)
            if self.err_fd in ready:
                self._drain_stderr()
            if self.fd in ready:
                try:
                    chunk = os.read(self.fd, 8192)
                except OSError:
                    chunk = b""
                if not chunk:
                    return None
                self._buf += chunk
                self.last_data = time.monotonic()
                continue
            ref = self.last_data or self.started
            if time.monotonic() - ref > self.stall_s:
                raise CaptureError(f"no audio from {self.tool} for {self.stall_s:.0f} s")
        frame = np.frombuffer(bytes(self._buf[:need]), "<i2").copy()
        del self._buf[:need]
        self.frames += 1
        return frame

    def close(self):
        code = stop_process(self.p)
        for f in (self.p.stdout, self.p.stderr):
            try:
                f.close()
            except OSError:
                pass
        return code

    @property
    def exit_code(self):
        return self.p.poll()


class WavCapture:
    """--input-wav: a WAV file plays the role of the microphone (test mode).

    realtime: frames are paced at 16 kHz against a virtual clock that keeps running
    while capture is stopped (muted), like the real world would; fast: frames are
    delivered as fast as they are consumed and the position only advances on reads.
    At the end of the file read_frame() returns None once (EOF) and the source is done."""

    tool = "input-wav"

    def __init__(self, path, realtime, shared):
        self.path = path
        self.realtime = realtime
        self.shared = shared            # dict that survives restarts: pcm, t0, pos
        if "pcm" not in shared:
            x, _ = read_wav(path)
            shared["pcm"] = to_pcm16(x)
            shared["pos"] = 0
            shared["t0"] = None
        if realtime:
            if shared["t0"] is None:
                shared["t0"] = time.monotonic()
            shared["pos"] = max(shared["pos"], int((time.monotonic() - shared["t0"]) * MIC_RATE))
        self.frames = 0
        self.last_data = None

    def read_frame(self, stop_event=None):
        s = self.shared
        pcm = s["pcm"]
        if s["pos"] >= len(pcm):
            return None
        if self.realtime:
            due = s["t0"] + (s["pos"] + FRAME) / MIC_RATE
            while True:
                d = due - time.monotonic()
                if d <= 0:
                    break
                if stop_event is not None and stop_event.wait(min(d, 0.1)):
                    return None
        f = pcm[s["pos"]:s["pos"] + FRAME]
        s["pos"] += FRAME
        if len(f) < FRAME:
            f = np.concatenate([f, np.zeros(FRAME - len(f), "<i2")])
        self.frames += 1
        self.last_data = time.monotonic()
        return f

    def stderr_tail(self):
        return ""

    def close(self):
        return 0

    @property
    def exit_code(self):
        return 0


# --------------------------------------------------------------------------- playback sinks
class PlaybackError(Exception):
    pass


class ProcessPlayer:
    """One playback tool process fed raw s16le mono at `rate`."""

    def __init__(self, tool, rate, device="", argv=None):
        self.tool = tool
        self.rate = rate
        self.p = spawn(argv or playback_argv(tool, rate, device), stdin=subprocess.PIPE,
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        os.set_blocking(self.p.stderr.fileno(), False)
        self._err = b""

    def stderr_tail(self):
        try:
            while True:
                c = os.read(self.p.stderr.fileno(), 4096)
                if not c:
                    break
                self._err = (self._err + c)[-2000:]
        except (BlockingIOError, OSError, ValueError):
            pass
        lines = [ln.strip() for ln in self._err.decode("utf-8", "replace").splitlines() if ln.strip()]
        return " | ".join(lines[-3:])[:300]

    def write(self, pcm16):
        if self.p.poll() is not None:
            raise PlaybackError(f"{self.tool} exited with code {self.p.returncode}: {self.stderr_tail()}")
        try:
            self.p.stdin.write(pcm16.tobytes())
            self.p.stdin.flush()
        except (BrokenPipeError, OSError, ValueError) as e:
            raise PlaybackError(f"{self.tool} stopped: {e} {self.stderr_tail()}".strip()) from e

    def drain(self, timeout=10.0):
        """Close stdin and wait until the tool has played everything (it exits at EOF)."""
        try:
            self.p.stdin.close()
        except (BrokenPipeError, OSError, ValueError):
            pass
        try:
            self.p.wait(timeout)
        except subprocess.TimeoutExpired:
            log.warning("%s did not finish within %.0f s after end of speech; killing it", self.tool, timeout)
            stop_process(self.p)
        self._close_err()
        return self.p.returncode

    def kill(self):
        try:
            self.p.stdin.close()
        except (BrokenPipeError, OSError, ValueError):
            pass
        if self.p.poll() is None:
            try:
                self.p.kill()
            except OSError:
                pass
        try:
            self.p.wait(2)
        except subprocess.TimeoutExpired:
            pass
        self._close_err()

    def _close_err(self):
        try:
            self.p.stderr.close()
        except (OSError, ValueError):
            pass


class WavPlayer:
    """--output-dir: speech is written to DIR/speak-<id>.wav instead of a speaker.

    kill() may come from another thread (stop) while write() runs: both take the lock,
    and a write after kill() raises PlaybackError like a dead player process would."""

    tool = "output-dir"

    def __init__(self, path, rate):
        self.path = path
        self.rate = rate
        self._lock = threading.Lock()
        self.w = wave.open(path, "wb")
        self.w.setnchannels(1)
        self.w.setsampwidth(2)
        self.w.setframerate(rate)

    def write(self, pcm16):
        with self._lock:
            if self.w is None:
                raise PlaybackError("output WAV closed")
            try:
                self.w.writeframes(pcm16.tobytes())
            except (OSError, ValueError) as e:
                raise PlaybackError(f"cannot write {self.path}: {e}") from e

    def _close(self):
        with self._lock:
            w, self.w = self.w, None
        if w is not None:
            try:
                w.close()
            except (OSError, ValueError) as e:
                log.warning("closing %s: %s", self.path, e)

    def drain(self, timeout=0):
        self._close()
        return 0

    def kill(self):
        self._close()

    def stderr_tail(self):
        return ""
