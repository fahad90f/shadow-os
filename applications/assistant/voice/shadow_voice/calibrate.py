"""--calibrate SECONDS --expect TEXT: which speech model understands THIS user best?

Records SECONDS from the real microphone (or takes --input-wav), reports the
input level (peak / RMS dBFS, clipping, digital silence, noise floor), cuts the
speech with Silero VAD, runs EVERY installed STT model on the same audio and
scores each against --expect (word error rate). The recommendation is the model
with the lowest WER; on a tie the faster Moonshine. Advice names concrete fixes
(raise/lower the input gain, wrong input selected, ...).

Prints ONE JSON line. Exit 0 when a recommendation could be made, 1 when no
speech was heard, 2 when the microphone could not be opened.
"""
import time

import numpy as np

from . import FRAME, MIC_RATE
from . import audio
from .listener import make_vad
from .stt import Recognizer
from .wake import match_wake, word_errors


def record(seconds, device="", forced="auto", command=None):
    """(int16 samples, tool) — raises audio.CaptureError when nothing works."""
    errors = []
    tools = ["capture-command"] if command else audio.capture_order(device, forced)
    if not tools:
        raise audio.CaptureError("no capture tool (install pipewire-bin, pulseaudio-utils or alsa-utils)")
    for tool in tools:
        argv = ["/bin/sh", "-c", command] if command else audio.capture_argv(tool, device)
        try:
            src = audio.ProcessCapture(tool, argv)
        except OSError as e:
            errors.append(f"{tool}: {e}")
            continue
        frames = []
        need = int(seconds * MIC_RATE / FRAME)
        try:
            while len(frames) < need:
                f = src.read_frame()
                if f is None:
                    break
                frames.append(f)
        except audio.CaptureError as e:
            errors.append(f"{tool}: {e}")
        tail = src.stderr_tail()
        code = src.close()
        if len(frames) >= need * 0.9:
            return np.concatenate(frames), tool
        errors.append(f"{tool}: exited with code {code}" + (f" ({tail})" if tail else ""))
    raise audio.CaptureError("; ".join(errors))


def level_stats(pcm16):
    x = pcm16.astype(np.float32) / 32768.0
    n = len(x) // FRAME
    frames = x[:n * FRAME].reshape(n, FRAME) if n else np.zeros((0, FRAME), np.float32)
    frame_rms = np.sqrt(np.mean(frames ** 2, axis=1)) if n else np.zeros(0)
    peak = float(np.max(np.abs(x))) if len(x) else 0.0
    clipped = int(np.sum(np.abs(pcm16.astype(np.int32)) >= 32766))
    ac = float(np.std(x)) if len(x) else 0.0
    return {
        "seconds": round(len(x) / MIC_RATE, 2),
        "peak_dbfs": round(audio.dbfs(peak), 1),
        "rms_dbfs": round(audio.dbfs(float(np.sqrt(np.mean(x * x))) if len(x) else 0.0), 1),
        "noise_floor_dbfs": round(audio.dbfs(float(np.percentile(frame_rms, 10))) if n else -180.0, 1),
        "loud_dbfs": round(audio.dbfs(float(np.percentile(frame_rms, 95))) if n else -180.0, 1),
        "clipped_samples": clipped,
        "clipping": clipped > 0.0005 * max(1, len(x)),
        "silence": ac < audio.SILENT_AC,
    }


def speech_segments(root, x):
    vad = make_vad(root)
    segs = []
    for i in range(0, len(x) - FRAME + 1, FRAME):
        vad.accept_waveform(x[i:i + FRAME])
        while not vad.empty():
            segs.append((vad.front.start, np.asarray(vad.front.samples, np.float32)))
            vad.pop()
    vad.flush()
    while not vad.empty():
        segs.append((vad.front.start, np.asarray(vad.front.samples, np.float32)))
        vad.pop()
    return segs


def advice_for(stats, speech_s):
    out = []
    if stats["silence"]:
        out.append("The microphone delivers digital silence: it is muted or the wrong input is selected "
                   "(check the input device and its mute switch in the sound settings).")
    elif stats["peak_dbfs"] < -30:
        out.append(f"The input is very quiet (peak {stats['peak_dbfs']} dBFS): raise the microphone volume.")
    if stats["clipping"]:
        out.append("The input is clipping (distorted): lower the microphone volume or input boost.")
    if not stats["silence"] and stats["noise_floor_dbfs"] > -40:
        out.append(f"High background noise (floor {stats['noise_floor_dbfs']} dBFS): move closer to the microphone "
                   "or reduce the noise.")
    if not stats["silence"] and speech_s == 0:
        out.append("No speech was detected: speak the sentence while recording.")
    return out


def run(root, args):
    out = {"calibrate": True, "expect": args.expect, "data": root.path}
    try:
        if args.input_wav:
            x, _ = audio.read_wav(args.input_wav)
            pcm = audio.to_pcm16(x)
            out["source"] = "input-wav"
        else:
            pcm, tool = record(args.calibrate, args.input_device or "", args.capture, args.capture_command)
            out["source"] = tool
    except (audio.CaptureError, OSError, ValueError) as e:
        out.update(ok=False, error=f"Cannot record from the microphone: {e}")
        return out, 2
    x = pcm.astype(np.float32) / 32768.0
    stats = level_stats(pcm)
    out["mic"] = stats
    segs = speech_segments(root, x)
    speech_s = sum(len(s) for _, s in segs) / MIC_RATE
    out["speech_s"] = round(speech_s, 2)
    if segs:
        a = max(0, segs[0][0] - int(0.3 * MIC_RATE))
        b = min(len(x), segs[-1][0] + len(segs[-1][1]) + int(0.2 * MIC_RATE))
        clip = x[a:b]
    else:
        clip = x
    results = {}
    for name in root.stt_models:
        t = time.monotonic()
        rec = Recognizer(root, name, threads=args.stt_threads)
        load = time.monotonic() - t
        text, raw, ms = rec.transcribe(clip)
        _woke, rest, _how = match_wake(text)
        e, n = word_errors(args.expect, text)
        e2, _ = word_errors(args.expect, rest)
        errs = min(e, e2)          # the user may or may not say the wake word in the calibration sentence
        results[name] = {"text": text, "wer": round(errs / n, 3) if n else None, "errors": errs, "words": n,
                         "stt_ms": ms, "load_s": round(load, 2)}
        del rec
    out["models"] = results
    rec_name = None
    if speech_s > 0 and results:
        rec_name = min(results, key=lambda k: (results[k]["wer"] if results[k]["wer"] is not None else 9,
                                               0 if k == "moonshine" else 1))
    out["recommended"] = rec_name
    out["advice"] = advice_for(stats, speech_s)
    out["ok"] = rec_name is not None
    return out, (0 if rec_name else 1)
