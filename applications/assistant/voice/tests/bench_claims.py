"""Re-verify the measurements the engine's design depends on (not part of run-tests.sh).

  python3 -I tests/bench_claims.py oov                 sherpa-onnx's own Kokoro text path drops
                                                       out-of-lexicon words; the g2p front end does not
  python3 -I tests/bench_claims.py ttfa [THREADS...]   Kokoro fp32 first-chunk latency / real-time factor
  python3 -I tests/bench_claims.py stt MANIFEST.jsonl [N]
                                                       Moonshine base v2 vs Whisper base.en: WER + latency
                                                       (manifest lines: {"path": wav, "text": reference})

Uses the installed data root ($SHADOW_VOICE_DATA or the normal lookup). Prints JSON lines.
"""
import json
import os
import statistics
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from shadow_voice import datadir  # noqa: E402

ROOT, _ = datadir.find_data_root(os.environ.get("SHADOW_VOICE_DATA"))
if ROOT is None:
    sys.exit("no engine data root installed")
datadir.activate_runtime(ROOT)
import numpy as np  # noqa: E402
import sherpa_onnx as so  # noqa: E402
from shadow_voice import audio  # noqa: E402
from shadow_voice.stt import Recognizer  # noqa: E402
from shadow_voice.tts import KokoroEngine  # noqa: E402
from shadow_voice.wake import word_errors  # noqa: E402

OOV_SENTENCES = [
    "Your disk is 82% full and Downloads uses 14 GB.",
    "PipeWire and WirePlumber restarted on Debian.",
    "The CPU, the GPU and two USB ports are fine.",
    "I opened the URL and saved ~/Pictures/shot.png.",
    "Python 3.13 and sherpa-onnx 2.0.2 are installed.",
    "It is 10:30 AM and the battery is at 45%.",
    "She wanted the opened files renamed.",
]


def oov():
    tts = KokoroEngine(ROOT, threads=4)
    tts.warmup()
    rec = Recognizer(ROOT, ROOT.stt_default, threads=2)
    sid = tts.speakers.index("af_heart")
    for text in OOV_SENTENCES:
        g = so.GenerationConfig()
        g.sid, g.speed = sid, 1.0
        builtin = np.asarray(tts.tts.generate(text, g).samples, np.float32)       # sherpa-onnx's own front end
        ours = np.concatenate([tts.synth(s) for s in tts.sentences(text)])
        heard_b, _, _ = rec.transcribe(audio.resample(builtin, tts.sample_rate, 16000))
        heard_o, _, _ = rec.transcribe(audio.resample(ours, tts.sample_rate, 16000))
        spoken = " ".join(tts.sentences(text))
        eb, n = word_errors(spoken, heard_b)
        eo, _ = word_errors(spoken, heard_o)
        print(json.dumps({"text": text, "builtin_s": round(len(builtin) / tts.sample_rate, 2),
                          "g2p_s": round(len(ours) / tts.sample_rate, 2), "builtin_heard": heard_b,
                          "g2p_heard": heard_o, "builtin_wer": round(eb / n, 2), "g2p_wer": round(eo / n, 2)}),
              flush=True)


def ttfa(threads):
    replies = ["Okay, I opened the terminal for you. Anything else?", "Your disk is 82% full; Downloads uses 14 GB.",
               "Sure. I took a screenshot and saved it in your Pictures folder.",
               "It is 10:30 AM on Saturday, November 21st.", "Done. The volume is now at fifty percent."]
    for th in threads:
        t = time.monotonic()
        tts = KokoroEngine(ROOT, threads=th)
        load = time.monotonic() - t
        warm = tts.warmup()
        lat, rtf = [], []
        for _ in range(2):
            for r in replies:
                s = tts.sentences(r, first_short=True)[0]
                t = time.monotonic()
                x = tts.synth(s)
                d = time.monotonic() - t
                lat.append(d)
                full = tts.synth(" ".join(tts.sentences(r)))
                t = time.monotonic()
                tts.synth(" ".join(tts.sentences(r)))
                rtf.append((time.monotonic() - t) / (len(full) / tts.sample_rate))
        print(json.dumps({"threads": th, "load_s": round(load, 2), "warmup_s": round(warm, 2),
                          "first_chunk_ms_median": int(1000 * statistics.median(lat)),
                          "first_chunk_ms_min": int(1000 * min(lat)), "first_chunk_ms_max": int(1000 * max(lat)),
                          "rtf_median": round(statistics.median(rtf), 2), "loadavg": round(os.getloadavg()[0], 2)}),
              flush=True)
        del tts


def stt(manifest, limit):
    items = [json.loads(line) for line in open(manifest)]
    items = items[:limit] if limit else items
    for name in ROOT.stt_models:
        t = time.monotonic()
        rec = Recognizer(ROOT, name, threads=2)
        load = time.monotonic() - t
        errs = words = 0
        lat = []
        by_voice = {}
        for it in items:
            x, _ = audio.read_wav(it["path"])
            text, _, ms = rec.transcribe(x)
            e, n = word_errors(it["text"], text)
            errs += e
            words += n
            lat.append(ms)
            v = it.get("voice", "?").split("-")[0]
            be = by_voice.setdefault(v, [0, 0])
            be[0] += e
            be[1] += n
        print(json.dumps({"model": name, "utterances": len(items), "wer": round(errs / max(1, words), 3),
                          "stt_ms_median": statistics.median(lat), "stt_ms_p90": sorted(lat)[int(0.9 * len(lat))],
                          "load_s": round(load, 2),
                          "wer_by_voice": {k: round(a / max(1, b), 3) for k, (a, b) in sorted(by_voice.items())}}),
              flush=True)
        del rec


if __name__ == "__main__":
    what = sys.argv[1] if len(sys.argv) > 1 else "oov"
    if what == "oov":
        oov()
    elif what == "ttfa":
        ttfa([int(a) for a in sys.argv[2:]] or [2, 4])
    elif what == "stt":
        stt(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 0)
    else:
        sys.exit(__doc__)
