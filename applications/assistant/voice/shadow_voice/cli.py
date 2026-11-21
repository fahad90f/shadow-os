"""Command line of shadow-voiced (see README.md for the protocol).

  shadow-voiced --serve [--data DIR] [--input-wav WAV [--fast]] [--output-dir DIR]
  shadow-voiced --probe                       fast check, one JSON line, exit 0/3/4/5
  shadow-voiced --selftest                    TTS -> VAD/wake/STT round trip, one JSON line
  shadow-voiced --transcribe WAV [--stt M]    one JSON line
  shadow-voiced --say TEXT --out WAV [--voice V] [--speed S]
  shadow-voiced --calibrate SECONDS --expect TEXT [--input-wav WAV]
"""
import argparse
import os
import sys
import time

from . import VERSION, datadir, logsetup
from .protocol import Emitter


def _cpus():
    """Physical cores we may run on (hyper-threads do not speed up ONNX Runtime's matmuls)."""
    try:
        cpus = sorted(os.sched_getaffinity(0))
    except (AttributeError, OSError):
        cpus = list(range(os.cpu_count() or 2))
    cores = set()
    for c in cpus:
        try:
            base = f"/sys/devices/system/cpu/cpu{c}/topology/"
            with open(base + "physical_package_id") as f1, open(base + "core_id") as f2:
                cores.add((f1.read().strip(), f2.read().strip()))
        except OSError:
            return max(1, len(cpus))
    return max(1, len(cores))


def build_parser():
    ap = argparse.ArgumentParser(prog="shadow-voiced", description="Offline voice engine for Shadow Assistant.",
                                 epilog=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--serve", action="store_true", help="run the engine (JSON lines on stdin/stdout)")
    mode.add_argument("--probe", action="store_true", help="check data, Python deps and tools; load nothing")
    mode.add_argument("--selftest", action="store_true", help="end-to-end check without audio devices")
    mode.add_argument("--transcribe", metavar="WAV", help="transcribe a WAV file")
    mode.add_argument("--say", metavar="TEXT", help="synthesise TEXT into --out WAV")
    mode.add_argument("--calibrate", metavar="SECONDS", type=float,
                      help="record the microphone and compare every STT model against --expect")
    mode.add_argument("--version", action="version", version=f"shadow-voiced {VERSION}")
    ap.add_argument("--data", metavar="DIR", help="engine data root (default: lookup order, see README)")
    ap.add_argument("--expect", metavar="TEXT", help="what the user says during --calibrate")
    ap.add_argument("--out", metavar="WAV", help="output file for --say")
    ap.add_argument("--input-wav", metavar="WAV", help="test: use this WAV instead of the microphone")
    ap.add_argument("--fast", action="store_true", help="test: do not pace --input-wav/--output-dir in real time")
    ap.add_argument("--output-dir", metavar="DIR", help="test: write speech to DIR/speak-<id>.wav instead of playing")
    ap.add_argument("--voice", default="af_heart", help="Kokoro voice id (default af_heart)")
    ap.add_argument("--speed", type=float, default=1.0, help="speech rate 0.7-1.4")
    ap.add_argument("--stt", default="auto", choices=["auto", "moonshine", "whisper"])
    ap.add_argument("--wake-sensitivity", default="normal", choices=["low", "normal", "high"])
    ap.add_argument("--no-wake", action="store_true", help="start with the wake word off")
    ap.add_argument("--muted", action="store_true", help="start muted (microphone closed)")
    ap.add_argument("--input-device", default="", help="capture device ('' = default)")
    ap.add_argument("--output-device", default="", help="playback device ('' = default)")
    ap.add_argument("--capture", default="auto", choices=["auto", "pw-record", "parec", "arecord"])
    ap.add_argument("--playback", default="auto", choices=["auto", "pw-play", "pacat", "aplay"])
    ap.add_argument("--capture-command", metavar="SH",
                    help="test: shell command that writes 16 kHz s16le mono to stdout (replaces the capture tool)")
    ap.add_argument("--tts-threads", type=int, default=min(4, _cpus()))
    ap.add_argument("--stt-threads", type=int, default=min(2, _cpus()))
    ap.add_argument("--espeak", default="espeak-ng", help="espeak-ng binary (g2p fallback)")
    ap.add_argument("--max-utterance", type=float, default=15.0)
    ap.add_argument("--end-silence", type=float, default=0.6, help="silence (s) that ends an utterance")
    ap.add_argument("--vad-threshold", type=float, default=0.5)
    ap.add_argument("--no-partials", action="store_true")
    ap.add_argument("--debug", action="store_true", help="verbose log (includes transcripts)")
    ap.add_argument("--no-log-file", action="store_true", help="log to stderr only")
    return ap


def main(argv=None):
    args = build_parser().parse_args(argv)
    args.speed = min(1.4, max(0.7, args.speed))

    if args.probe:
        emitter = Emitter.take_stdout()
        logsetup.setup(args.debug, tee=False)
        result, code = datadir.probe(args.data)
        emitter.write_obj(result)
        return code

    emitter = Emitter.take_stdout()
    logsetup.setup(args.debug, tee=not args.no_log_file,
                   tag=f"shadow-voiced/{os.getpid()}{'' if args.serve else ' ' + _mode(args)}")
    import logging
    log = logging.getLogger("shadow-voiced")
    holder = {"engine": None}

    def emit_error(message, fatal):
        emitter.emit("error", message=message, fatal=fatal)

    def on_fatal(code):
        eng = holder["engine"]
        if eng is not None:
            eng.request_exit(code, "fatal error")
        else:
            sys.stderr.flush()
            os._exit(code)

    logsetup.install_excepthooks(emit_error, on_fatal)
    log.info("shadow-voiced %s %s (python %s, pid %d)", VERSION, _mode(args), sys.version.split()[0], os.getpid())

    root, searched = datadir.find_data_root(args.data)
    if root is None:
        result, code = datadir.probe(args.data)
        return _fail(emitter, args, result["reason"], code)
    try:
        info = datadir.activate_runtime(root)
    except datadir.RuntimeDepsError as e:
        return _fail(emitter, args, str(e), datadir.EXIT_NO_PYDEPS)
    log.info("data %s (numpy %s, sherpa-onnx %s)", root.path, info["numpy"], info["sherpa_onnx"])

    if args.serve:
        from .engine import Engine
        eng = Engine(args, root, emitter)
        holder["engine"] = eng
        code = eng.run()
        log.info("exit %s", code)
        logging.shutdown()
        return code
    if args.selftest:
        from . import selftest
        out, code = selftest.run(root, args)
    elif args.transcribe:
        out, code = _transcribe(root, args)
    elif args.say:
        out, code = _say(root, args)
    else:
        if not args.expect:
            emitter.write_obj({"ok": False, "error": "--calibrate needs --expect TEXT"})
            return 2
        from . import calibrate
        out, code = calibrate.run(root, args)
    emitter.write_obj(out)
    return code


def _mode(args):
    for m in ("serve", "selftest", "transcribe", "say", "calibrate"):
        if getattr(args, m, None):
            return "--" + m
    return ""


def _fail(emitter, args, reason, code):
    if args.serve:
        emitter.emit("hello", version=VERSION, pid=os.getpid())
        emitter.emit("error", message=reason, fatal=True)
    else:
        emitter.write_obj({"ok": False, "error": reason, "exit": code})
    return code


def _transcribe(root, args):
    from . import audio
    from .stt import Recognizer
    from .wake import match_wake
    name = args.stt if args.stt in root.stt_models else root.stt_default
    try:
        x, sr = audio.read_wav(args.transcribe)
    except (OSError, ValueError, EOFError) as e:
        return {"ok": False, "error": f"cannot read {args.transcribe}: {e}"}, 2
    rec = Recognizer(root, name, threads=args.stt_threads)
    text, raw, ms = rec.transcribe(x)
    woke, rest, how = match_wake(text, sensitivity=args.wake_sensitivity)
    return {"ok": True, "stt": name, "text": text, "raw": raw, "stt_ms": ms, "seconds": round(len(x) / 16000, 2),
            "source_rate": sr, "wake": woke, "command": rest if woke else "", "load_s": round(rec.load_s, 2)}, 0


def _say(root, args):
    from . import audio
    from .tts import KokoroEngine
    if not args.out:
        return {"ok": False, "error": "--say needs --out FILE.wav"}, 2
    tts = KokoroEngine(root, voice=args.voice, speed=args.speed, threads=args.tts_threads, espeak=args.espeak)
    if args.voice not in tts.voices:
        return {"ok": False, "error": f"unknown voice {args.voice!r}", "voices": tts.voices}, 2
    t = time.monotonic()
    pieces, first_ms, sentences = [], None, []
    import numpy as np
    for s in tts.sentences(args.say, first_short=True):
        x = tts.synth(s)
        if first_ms is None:
            first_ms = int(round(1000 * (time.monotonic() - t)))
        sentences.append(s)
        pieces.append(x)
        pieces.append(np.zeros(int(0.12 * tts.sample_rate), np.float32))
    y = np.concatenate(pieces) if pieces else np.zeros(0, np.float32)
    audio.write_wav(args.out, audio.to_pcm16(y), tts.sample_rate)
    secs = len(y) / tts.sample_rate
    return {"ok": True, "out": args.out, "voice": tts.voice, "seconds": round(secs, 2), "sample_rate": tts.sample_rate,
            "first_audio_ms": first_ms, "synth_s": round(time.monotonic() - t, 2), "sentences": sentences,
            "load_s": round(tts.load_s, 2)}, 0
