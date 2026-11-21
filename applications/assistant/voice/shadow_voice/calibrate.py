"""--calibrate SECONDS --expect TEXT: which speech model understands THIS user best,
and does "Shadow" wake the assistant in THIS user's voice?

Records SECONDS from the real microphone (or takes --input-wav), reports the
input level (peak / RMS dBFS, clipping, digital silence, noise floor), cuts the
speech with Silero VAD and runs EVERY installed STT model on the same audio.

* Speech recognition is scored on the COMMAND words only (TEXT without its
  leading wake word): a misheard "Shadow" is the wake check's business, not a
  word error. Each model gets "wer" (command words), "errors", "words".
* The wake word is checked the way the running engine decides it: the whole
  recording goes through the REAL listener (Silero VAD + the "Shadow" keyword
  spotter + the wake policy + that STT model), as --selftest does with
  synthetic speech, at --wake-sensitivity. "wake.ok" is true only when it
  produced final{id:"wake"} (the assistant would have taken the command). When
  it did not, the same audio is tried at sensitivity "high"; "wake.needs_high"
  is then true when only that setting makes the wake word work for this voice
  ("wake.sensitivity" is the one that worked, "wake.configured" the one asked).
* The recommendation is the model with the lowest command WER (Moonshine, ~3x
  faster, on a tie), unless a model within 12 points of it is the only one
  with which "Shadow" works; "wake.stt" names the model the wake check used.

Advice names concrete fixes (raise/lower the input gain, wrong input, ...).
Prints ONE JSON line. Exit 0 when a recommendation could be made, 1 when no
speech was heard, 2 when the microphone could not be opened.
"""
import time

import numpy as np

from . import FRAME, MIC_RATE
from . import audio
from .listener import Listener, make_vad
from .stt import Recognizer
from .wake import match_wake, normalize_words, word_errors

WAKE_MARGIN = 0.12      # a model this much worse in WER may still be chosen if only it hears "Shadow"


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


def command_errors(ref_command, text):
    """Word errors of a transcript against the command part of the test sentence: with the
    wake word stripped when the model heard it, or with a misheard first word ("Jado, ...")
    left out — that word is scored by the wake check."""
    cands = [text]
    woke, rest, _how = match_wake(text, kws_hit=True, sensitivity="high")
    if woke:
        cands.append(rest)
    words = (text or "").split()
    if len(words) > 1:
        cands.append(" ".join(words[1:]))
    n = len(normalize_words(ref_command))
    return min(word_errors(ref_command, c)[0] for c in cands), n


def wake_check(root, rec, pcm, sensitivity):
    """The recording through the real listener, as the running engine hears it."""
    events = []
    listener = Listener(root, rec, lambda ev, **f: events.append(dict(event=ev, **f)), lambda v: None,
                        wake_enabled=True, sensitivity=sensitivity, partials=False)
    sig = np.concatenate([pcm, np.zeros(int(1.5 * MIC_RATE), np.int16)])    # let the VAD close the utterance
    for i in range(0, len(sig) - FRAME + 1, FRAME):
        listener.feed(sig[i:i + FRAME])
    finals = [e for e in events if e["event"] == "final" and e.get("id") == "wake"]
    return {
        "ok": bool(finals),
        "sensitivity": sensitivity,
        "stt": rec.name,
        "via": [e.get("via") for e in events if e["event"] == "wake"],
        "command": finals[0].get("text", "") if finals else "",
        "heard": finals[0].get("heard", "") if finals else "",
        "match": finals[0].get("wake_match", "") if finals else "",
        "rejected": [e.get("heard", "") for e in events if e["event"] == "wake_rejected"],
        "kws_hits": listener.kws_hits,
    }


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
    exp_woke, exp_rest, _how = match_wake(args.expect)
    ref_command = exp_rest if exp_woke else args.expect
    sensitivity = getattr(args, "wake_sensitivity", "normal") or "normal"
    results, recs = {}, {}
    for name in root.stt_models:
        t = time.monotonic()
        rec = Recognizer(root, name, threads=args.stt_threads)
        load = time.monotonic() - t
        text, raw, ms = rec.transcribe(clip)
        errs, n = command_errors(ref_command, text)
        results[name] = {"text": text, "wer": round(errs / n, 3) if n else None, "errors": errs, "words": n,
                         "wake_word": match_wake(text, sensitivity=sensitivity)[2], "stt_ms": ms,
                         "load_s": round(load, 2)}
        recs[name] = rec
    out["models"] = results
    out["command"] = ref_command
    rec_name, wake = None, None
    if speech_s > 0 and results:
        order = sorted(results, key=lambda k: (results[k]["wer"] if results[k]["wer"] is not None else 9,
                                               0 if k == "moonshine" else 1))
        best = results[order[0]]["wer"] if results[order[0]]["wer"] is not None else 9
        close = [k for k in order if (results[k]["wer"] if results[k]["wer"] is not None else 9) <= best + WAKE_MARGIN]
        if exp_woke:
            tries = [sensitivity] + (["high"] if sensitivity != "high" else [])
            for sens in tries:
                for k in close:
                    w = wake_check(root, recs[k], pcm, sens)
                    if w["ok"]:
                        rec_name, wake = k, w
                        break
                if wake:
                    break
            if wake is None:
                rec_name = order[0]
                wake = wake_check(root, recs[rec_name], pcm, sensitivity)
                wake["heard"] = results[rec_name]["text"]
            wake["needs_high"] = wake["ok"] and wake["sensitivity"] == "high" and sensitivity != "high"
            wake["configured"] = sensitivity
        else:
            rec_name = order[0]
    recs.clear()
    out["recommended"] = rec_name
    out["wake"] = wake
    out["advice"] = advice_for(stats, speech_s)
    out["ok"] = rec_name is not None
    return out, (0 if rec_name else 1)
