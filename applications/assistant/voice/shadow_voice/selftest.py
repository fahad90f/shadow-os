"""--selftest: prove the installed engine works end to end without audio devices.

1. load Kokoro (+ warm-up) and the default STT model; measure first-audio latency
   and the real-time factor of synthesis;
2. the g2p front end must resolve every word of a sentence full of things the bare
   lexicon drops (numbers, units, CamelCase, acronyms, a path, a version);
3. synthesise "Shadow, open the terminal." and run it through the REAL listener
   (Silero VAD + keyword spotter + wake policy + STT): it must produce
   final{id:"wake", text:"open the terminal"};
4. every other installed STT model transcribes the same audio;
5. report which capture/playback tools exist (nothing is opened).

Prints ONE JSON line; exit 0 when every required check passed, 1 otherwise.
"""
import logging
import time

import numpy as np

from . import FRAME, MIC_RATE, VERSION
from . import audio
from .listener import Listener
from .stt import Recognizer
from .tts import KokoroEngine
from .wake import word_errors

log = logging.getLogger("shadow-voiced")

G2P_PROBE = ("PipeWire on Debian uses 82% of 16 GB RAM; the CPU and USB ports are fine. "
             "Python 3.13 saved ~/Pictures/shot.png at 10:30.")
PHRASE = "Shadow, open the terminal."
EXPECT = "open the terminal"


def _check(checks, name, ok, detail, required=True):
    checks.append({"name": name, "ok": bool(ok), "required": required, "detail": detail})


def run(root, args):
    out = {"selftest": True, "version": VERSION, "data": root.path, "ok": False}
    checks = []
    t0 = time.monotonic()
    tts = KokoroEngine(root, voice=args.voice, speed=1.0, threads=args.tts_threads, espeak=args.espeak)
    out["tts_load_s"] = round(tts.load_s, 2)
    warm = tts.warmup()
    out["tts_warmup_s"] = round(warm, 2)
    stt_name = args.stt if args.stt in root.stt_models else root.stt_default
    rec = Recognizer(root, stt_name, threads=args.stt_threads)
    out["load_s"] = round(time.monotonic() - t0, 2)

    # first audio latency of a short first sentence (what a reply's first clause costs)
    t = time.monotonic()
    first = tts.synth(tts.sentences("Hi, I'm Shadow.")[0])
    out["first_audio_ms"] = int(round(1000 * (time.monotonic() - t)))
    _check(checks, "tts", len(first) > 0.3 * tts.sample_rate, f"{len(first) / tts.sample_rate:.2f} s of audio")

    # g2p: no word may be dropped
    g = tts.g2p_for(tts.voice)
    dropped, oov = [], []
    for s in g.sentences(G2P_PROBE):
        toks, need = g.phonemize(s)
        oov.extend(need)
        for w in need:
            if not g._espeak_many([w])[0]:
                dropped.append(w)
    _check(checks, "g2p", not dropped,
           f"espeak-ng resolved {len(oov)} out-of-lexicon words" + (f"; DROPPED: {dropped}" if dropped else ""),
           required=bool(g.espeak))

    # round trip through the real listener
    t = time.monotonic()
    c = time.process_time()
    x = tts.synth(tts.sentences(PHRASE)[0])
    synth_s, cpu_s = time.monotonic() - t, time.process_time() - c
    secs = len(x) / tts.sample_rate
    out["tts_rtf"] = round(synth_s / secs, 3)
    out["tts_cpu_rtf"] = round(cpu_s / secs, 3)
    y = audio.resample(x, tts.sample_rate, MIC_RATE)
    y = y * (10 ** (-24 / 20) / (float(np.sqrt(np.mean(y * y))) + 1e-9))
    rng = np.random.default_rng(1)
    pad = lambda s: rng.normal(0, 10 ** (-60 / 20), int(s * MIC_RATE)).astype(np.float32)
    sig = np.concatenate([pad(0.8), y, pad(1.5)])
    events = []
    emit = lambda ev, **f: events.append(dict(event=ev, **f))
    L = Listener(root, rec, emit, lambda v: None, wake_enabled=True, sensitivity="normal", partials=False)
    pcm = audio.to_pcm16(sig)
    t = time.monotonic()
    for i in range(0, len(pcm) - FRAME + 1, FRAME):
        L.feed(pcm[i:i + FRAME])
    proc_s = time.monotonic() - t
    finals = [e for e in events if e["event"] == "final"]
    wakes = [e["via"] for e in events if e["event"] == "wake"]
    heard = finals[0]["text"] if finals else ""
    errs, n = word_errors(EXPECT, heard)
    out["heard"] = heard
    out["wake"] = wakes
    out["stt"] = {stt_name: {"text": heard, "stt_ms": finals[0]["stt_ms"] if finals else None}}
    out["listener_rtf"] = round(proc_s / (len(pcm) / MIC_RATE), 3)
    _check(checks, "wake+stt", bool(finals) and finals[0]["id"] == "wake" and errs == 0,
           f"heard {heard!r} via {wakes}")
    _check(checks, "kws", "kws" in wakes or not root.has_kws, f"keyword spotter hits: {L.kws_hits}",
           required=False)

    # the other installed STT models on the same audio
    for other in root.stt_models:
        if other == stt_name:
            continue
        try:
            r2 = Recognizer(root, other, threads=args.stt_threads)
            text, _raw, ms = r2.transcribe(np.concatenate([y, pad(0.3)]))
            e2, _ = word_errors(PHRASE, text)
            out["stt"][other] = {"text": text, "stt_ms": ms, "load_s": round(r2.load_s, 2)}
            _check(checks, f"stt-{other}", e2 <= 1, f"heard {text!r}", required=False)
            del r2
        except Exception as e:
            _check(checks, f"stt-{other}", False, str(e), required=False)

    out["capture"] = audio.available(audio.CAPTURE_TOOLS)
    out["playback"] = audio.available(audio.PLAYBACK_TOOLS)
    _check(checks, "capture-tool", bool(out["capture"]), ", ".join(out["capture"]) or "none (install pipewire-bin)",
           required=False)
    _check(checks, "playback-tool", bool(out["playback"]), ", ".join(out["playback"]) or "none (install pipewire-bin)",
           required=False)
    out["checks"] = checks
    out["ok"] = all(c["ok"] for c in checks if c["required"])
    out["total_s"] = round(time.monotonic() - t0, 2)
    return out, (0 if out["ok"] else 1)
