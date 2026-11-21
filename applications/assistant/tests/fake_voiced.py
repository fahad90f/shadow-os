#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""fake_voiced.py — a deterministic stand-in for shadow-voiced (voice v2 spec §2.1/§2.2).

It speaks the helper's protocol (newline-delimited JSON on stdin/stdout) without
models, microphone or speaker, so the app's voice controller can be tested
headless and the lab can take screenshots with the natural voice "ready":

    fake_voiced.py --probe                 one JSON line, exit 0/3/4/5 (FAKE_VOICED_PROBE)
    fake_voiced.py --selftest              one JSON line, exit 0/1 (FAKE_VOICED_SELFTEST)
    fake_voiced.py --serve [--data DIR] [--input-wav FILE [--fast]] [--output-dir DIR]

Point the app at it with  SHADOW_VOICED=/path/to/fake_voiced.py.

What the user "says" comes from a scenario (JSON). It is read from --input-wav
when that file ends in .json (so `shadow-assistant --voice-session x.json` drives
it), else from $FAKE_VOICED_SCENARIO; without one nothing is ever heard and
every listen times out. Scenario keys (all optional):

    "utterances": [  # spoken to the wake word, "at" seconds after "ready"
        {"at": 0.5, "text": "what can you do", "heard": "Shadow, what can you do"},
        {"at": 4.0, "heard": "I saw my shadow", "reject": true}    # kws fired, transcript did not
    ],
    "answers": {     # what is heard by each listen{mode}, consumed in order
        "command":  [{"text": "list my files", "hold": true}],     # hold: until "finish"
        "yesno":    [{"text": "yes"}],                             # + "answer": "yes" like the real helper
        "followup": [{"timeout": true}]                            # also when the list is empty
    },
    "eof_at": 2.0,   # with --input-wav: seconds after "ready" the input ends ({"event":"eof"});
                     # later listens are refused at once (timeout, reason "no-input");
                     # default: last utterance + 3. Without --input-wav it never ends.
    "mic": "error",  # report a capture failure instead of "ok"
    "kws": true, "stt": ["moonshine", "whisper"], "capture": "pw-record"

Environment knobs:
    FAKE_VOICED_PROBE=ok|nodata|nodeps|nocapture|fail     --probe result (default ok)
    FAKE_VOICED_INSTALLED=FILE                            --probe says "nodata" until FILE exists
    FAKE_VOICED_SELFTEST=ok|fail                          --selftest result (default ok)
    FAKE_VOICED_CRASH=start|ready|listen|speak            exit 1 (with a traceback on stderr) there
    FAKE_VOICED_CRASH_TIMES=N                             ... only in the first N --serve runs
    FAKE_VOICED_STATE=FILE                                where that run count is kept
    FAKE_VOICED_LOAD_S=0.3                                "model loading" time before "ready"
    FAKE_VOICED_CPS=300                                   playback speed, characters per second
    FAKE_VOICED_TIME_SCALE=0.2                            factor for listen timeouts (5 s -> 1 s)
    FAKE_VOICED_LOG=FILE                                  append every command line received

Behaviour follows the spec: "state" is "speaking" from speak_start to speak_end
(also while it waits for more:true text), "idle"/"muted" otherwise unless a listen
runs; speak{more:false} with empty text just closes the id; stop ends the current
id with interrupted:true; mute cancels a listen; level events ~30 Hz while
listening (mic) or playing (tts). It never touches audio devices.
"""
import argparse
import heapq
import json
import math
import os
import re
import select
import sys
import tempfile
import time
import wave

VERSION = "fake-1.0"
VOICES = ["af_heart", "af_bella", "af_sarah", "bf_emma", "af_nicole", "af_sky", "am_adam"]
LEVEL_HZ = 30.0


def env(name, default=None):
    return os.environ.get("FAKE_VOICED_" + name, default)


def env_float(name, default):
    try:
        return float(env(name, default))
    except (TypeError, ValueError):
        return default


T0 = time.monotonic()


def now():
    return time.monotonic() - T0


def emit(event, **fields):
    line = json.dumps({"event": event, "t": round(now(), 3), **fields}, ensure_ascii=False)
    sys.stdout.write(line + "\n")
    sys.stdout.flush()


def crash(where, run):
    emit("error", message=f"fake crash at {where} (run {run})", fatal=True)
    sys.stderr.write("Traceback (most recent call last):\n"
                     '  File "/usr/lib/shadow-assistant/voice/shadow_voice/engine.py", line 42, in load\n'
                     f"RuntimeError: fake crash at {where} (run {run}, FAKE_VOICED_CRASH={where})\n")
    sys.stderr.flush()
    os._exit(1)


# ----------------------------------------------------------------------------- probe / selftest
def probe():
    kind = env("PROBE", "ok")
    marker = env("INSTALLED")
    if marker:  # "not installed" until the (fake) installer created this file
        kind = "ok" if os.path.exists(marker) else "nodata"
    base = {"ok": True, "data": "/fake/shadow-voice", "python": "%d.%d.%d" % sys.version_info[:3],
            "missing": [], "stt": ["moonshine", "whisper"], "tts": ["kokoro"], "kws": True,
            "capture": "pw-record", "playback": "pw-play", "reason": ""}
    if kind == "nodata":
        base.update(ok=False, data="", missing=["vad", "stt", "tts"], stt=[], tts=[], kws=False,
                    reason="Voice models are not installed.")
        code = 3
    elif kind == "nodeps":
        base.update(ok=False, missing=["sherpa_onnx", "numpy"], reason="Python modules are missing.")
        code = 4
    elif kind == "nocapture":
        base.update(ok=False, capture="", reason="No capture tool (pw-record, parec, arecord).")
        code = 5
    elif kind == "fail":
        sys.stderr.write("fake probe failure\n")
        return 1
    else:
        code = 0
    print(json.dumps(base), flush=True)
    return code


def selftest():
    ok = env("SELFTEST", "ok") == "ok"
    emit("selftest", ok=ok, heard=["open the terminal"] if ok else [], first_audio_ms=120,
         tts_rtf=0.2, tts_cpu_rtf=0.4, fake=True)
    return 0 if ok else 1


# ----------------------------------------------------------------------------- serve
class Timer:
    def __init__(self, fn):
        self.fn = fn
        self.cancelled = False

    def cancel(self):
        self.cancelled = True


class Helper:
    def __init__(self, args):
        self.args = args
        self.timers = []
        self.seq = 0
        self.ready = False
        self.backlog = []          # commands received before "ready"
        self.muted = False
        self.wake = True
        self.voice = "af_heart"
        self.state = None
        self.listen = None         # {"id","mode","timeout","answer","speech","timer"}
        self.speech = []           # queued chunks: [id, text, more]
        self.playing = None        # chunk being "played"
        self.open_ids = []         # ids that started and have not ended (in order)
        self.closed = set()        # ids whose more:false arrived
        self.durations = {}        # id -> seconds of "audio"
        self.level_timer = None
        self.mic_status = None
        self.input_done = False
        self.cps = max(10.0, env_float("CPS", 300.0))
        self.scale = max(0.01, env_float("TIME_SCALE", 0.2))
        self.log = env("LOG")
        self.scenario = self.load_scenario()
        self.answers = {k: list(v) for k, v in self.scenario.get("answers", {}).items()}
        self.crash_at = env("CRASH", "") or self.scenario.get("crash", "")
        self.run_no = self.count_run()
        if self.crash_at:
            limit = env("CRASH_TIMES")
            if limit is not None and self.run_no > int(limit):
                self.crash_at = ""

    # -- setup
    def load_scenario(self):
        path = None
        if self.args.input_wav and self.args.input_wav.endswith(".json"):
            path = self.args.input_wav
        elif env("SCENARIO"):
            path = env("SCENARIO")
        if not path:
            return {}
        with open(path, encoding="utf-8") as f:
            return json.load(f)

    def count_run(self):
        state = env("STATE") or os.path.join(tempfile.gettempdir(), "fake-voiced-%d.count" % os.getppid())
        try:
            with open(state, encoding="ascii") as f:
                n = int(f.read().strip() or 0)
        except (OSError, ValueError):
            n = 0
        n += 1
        try:
            with open(state, "w", encoding="ascii") as f:
                f.write(str(n))
        except OSError:
            pass
        return n

    # -- scheduling
    def later(self, delay, fn):
        t = Timer(fn)
        heapq.heappush(self.timers, (now() + max(0.0, delay), self.seq, t))
        self.seq += 1
        return t

    def set_state(self, value):
        if value != self.state:
            self.state = value
            emit("state", value=value)

    def resting(self):
        if self.open_ids or self.playing or self.speech:
            return "speaking"
        if self.listen:
            return "transcribing" if self.listen.get("transcribing") else "listening"
        return "muted" if self.muted else "idle"

    def settle(self):
        self.set_state(self.resting())
        self.update_levels()

    # -- levels: ~30 Hz while listening (mic) or playing (tts)
    def update_levels(self):
        active = bool(self.playing) or (self.listen is not None and not self.listen.get("transcribing"))
        if active and self.level_timer is None:
            self.level_timer = self.later(1.0 / LEVEL_HZ, self.level_tick)
        elif not active and self.level_timer is not None:
            self.level_timer.cancel()
            self.level_timer = None

    def level_tick(self):
        self.level_timer = None
        t = now()
        if self.playing:
            emit("level", source="tts", rms=round(0.35 + 0.3 * abs(math.sin(t * 9.0)), 3))
        elif self.listen is not None and not self.listen.get("transcribing"):
            speaking = self.listen.get("speech")
            rms = 0.25 + 0.35 * abs(math.sin(t * 7.0)) if speaking else 0.03 + 0.02 * abs(math.sin(t * 3.0))
            emit("level", source="mic", rms=round(rms, 3))
        self.update_levels()

    # -- startup
    def start(self):
        emit("hello", version=VERSION, pid=os.getpid())
        if self.crash_at == "start":
            crash("start", self.run_no)
        self.later(env_float("LOAD_S", 0.3), self.on_ready)

    def on_ready(self):
        if self.crash_at == "ready":
            self.later(0.2, lambda: crash("ready", self.run_no))
        sc = self.scenario
        emit("ready", load_s=env_float("LOAD_S", 0.3), stt="moonshine", tts="kokoro", voice=self.voice,
             voices=VOICES, kws=sc.get("kws", True), capture=sc.get("capture", "pw-record"),
             playback="pw-play", data="/fake/shadow-voice", fake=True)
        self.ready = True
        self.settle()
        self.mic_report()
        last = 0.0
        for u in sc.get("utterances", []):
            at = float(u.get("at", 0.5))
            last = max(last, at)
            self.later(at, lambda u=u: self.spontaneous(u))
        if self.args.input_wav:  # only a test input can end; a "microphone" never does
            self.later(float(sc.get("eof_at", last + 3.0)), self.input_ended)
        backlog, self.backlog = self.backlog, []
        for cmd in backlog:
            self.handle(cmd)

    def input_ended(self):
        # Like the real helper: the end of the test input is reported, the
        # helper keeps serving, and later listens are refused at once.
        self.input_done = True
        emit("eof")
        emit("mic", status="stopped", tool="input-wav", message="End of the input WAV")

    def mic_report(self):
        status = "stopped" if self.muted else ("error" if self.scenario.get("mic") == "error" else "ok")
        if status != self.mic_status:
            self.mic_status = status
            msg = "pw-record: no microphone found (fake)" if status == "error" else ""
            emit("mic", status=status, tool="pw-record", message=msg)

    # -- what the user says to the wake word
    def spontaneous(self, u, tries=0):
        if self.muted or not self.wake:
            return
        if self.state != "idle":
            if tries < 100:  # half duplex: wait until speaking/listening is over
                self.later(0.1, lambda: self.spontaneous(u, tries + 1))
            return
        heard = u.get("heard", u.get("text", ""))
        emit("vad", speech=True)
        emit("wake", via="kws")
        if u.get("reject"):
            emit("vad", speech=False)
            emit("wake_rejected", heard=heard)
            return
        text = u.get("text", "")
        words = text.split()
        if len(words) > 2:
            emit("partial", text=" ".join(words[: len(words) // 2]))
        self.listen = {"id": "wake", "mode": "command", "transcribing": True, "spontaneous": True}
        self.settle()

        def done():
            emit("vad", speech=False)
            emit("wake", via="stt")
            self.listen = None
            emit("final", text=text, heard=heard, stt_ms=95, id="wake", mode="command")
            self.settle()
        self.later(0.15, done)

    # -- commands
    def handle(self, cmd):
        name = cmd.get("cmd")
        if not self.ready and name not in ("quit", "ping", "config", "mute"):
            self.backlog.append(cmd)
            return
        fn = getattr(self, "cmd_" + str(name), None)
        if fn is None:
            emit("error", message=f"unknown command {name!r}", fatal=False)
            return
        fn(cmd)

    def cmd_ping(self, cmd):
        emit("pong")

    def cmd_quit(self, cmd):
        sys.exit(0)

    def cmd_config(self, cmd):
        applied = []
        for key in ("voice", "speed", "wake", "wake_sensitivity", "stt", "input_device", "muted", "follow_up_secs"):
            if key in cmd:
                applied.append(key)
        if "voice" in cmd:
            self.voice = cmd["voice"]
        if "wake" in cmd:
            self.wake = bool(cmd["wake"])
        emit("config_ok", applied=applied)
        if "muted" in cmd:
            self.apply_mute(bool(cmd["muted"]))

    def cmd_mute(self, cmd):
        self.apply_mute(bool(cmd.get("value")))

    def apply_mute(self, value):
        if value == self.muted:
            return
        self.muted = value
        if value and self.listen:
            self.end_listen()
        if self.ready:
            self.mic_report()
            self.settle()

    def cmd_listen(self, cmd):
        lid = str(cmd.get("id", "listen"))
        mode = cmd.get("mode", "command")
        timeout = float(cmd.get("timeout", 5.0 if mode == "followup" else 8.0))
        if self.muted:
            emit("error", message="the microphone is muted", fatal=False)
            emit("timeout", id=lid)
            return
        if self.crash_at == "listen":
            crash("listen", self.run_no)
        if self.input_done:
            emit("timeout", id=lid, reason="no-input")
            return
        if self.playing or self.speech or self.open_ids:
            self.interrupt()
        if self.listen:
            self.end_listen()
        queue = self.answers.get(mode, [])
        answer = queue.pop(0) if queue else {"timeout": True}
        self.listen = {"id": lid, "mode": mode, "answer": answer, "speech": False}
        self.settle()
        if answer.get("timeout") or not answer.get("text", "").strip():
            self.listen["timer"] = self.later(timeout * self.scale, self.listen_timeout)
            return
        self.later(float(answer.get("after", 0.2)), self.speech_starts)

    def speech_starts(self):
        lis = self.listen
        if not lis or lis.get("transcribing"):
            return
        lis["speech"] = True
        emit("vad", speech=True)
        words = lis["answer"]["text"].split()
        emit("partial", text=" ".join(words[: max(1, len(words) // 2)]))
        if not lis["answer"].get("hold"):
            self.later(0.3, self.transcribe)

    def transcribe(self):
        lis = self.listen
        if not lis or lis.get("transcribing"):
            return
        lis["transcribing"] = True
        emit("vad", speech=False)
        self.settle()

        def final():
            if self.listen is not lis:
                return
            self.listen = None
            text = lis["answer"]["text"]
            extra = {"answer": lis["answer"]["answer"]} if "answer" in lis["answer"] else {}
            emit("final", text=text, heard=text, stt_ms=110, id=lis["id"], mode=lis["mode"], **extra)
            self.settle()
        self.later(0.1, final)

    def listen_timeout(self):
        lis = self.listen
        if not lis or lis.get("speech"):
            return
        self.listen = None
        emit("timeout", id=lis["id"])
        self.settle()

    def end_listen(self):
        lis, self.listen = self.listen, None
        if lis and lis.get("timer"):
            lis["timer"].cancel()

    def cmd_finish(self, cmd):
        lis = self.listen
        if not lis or lis.get("transcribing"):
            return
        if lis.get("speech"):
            self.transcribe()
        else:
            if lis.get("timer"):
                lis["timer"].cancel()
            self.listen = None
            emit("timeout", id=lis["id"])
            self.settle()

    def cmd_cancel(self, cmd):
        if self.listen:
            self.end_listen()
            emit("vad", speech=False)
            self.settle()

    # -- speech
    def cmd_speak(self, cmd):
        if self.crash_at == "speak":
            crash("speak", self.run_no)
        sid = str(cmd.get("id", "s"))
        text = str(cmd.get("text", ""))
        more = bool(cmd.get("more", False))
        if self.listen and not self.listen.get("spontaneous"):
            self.end_listen()  # the app speaks: whatever was being listened for is over
        self.speech.append([sid, text, more])
        self.pump()

    def cmd_preview(self, cmd):
        voice = cmd.get("voice", self.voice)
        text = cmd.get("text") or f"Hi, this is {voice}."
        self.speech.append(["preview", text, False])
        self.pump()

    def cmd_stop(self, cmd):
        self.interrupt()
        self.settle()

    def interrupt(self):
        self.speech.clear()
        if self.playing:
            self.playing[3].cancel()
            self.playing = None
        for sid in self.open_ids:
            emit("speak_end", id=sid, interrupted=True)
        self.open_ids = []
        self.update_levels()

    def pump(self):
        if self.playing or not self.speech:
            self.settle()
            return
        sid, text, more = self.speech.pop(0)
        if sid not in self.open_ids:
            self.open_ids.append(sid)
            self.durations[sid] = 0.0
            emit("speak_start", id=sid)
            emit("first_audio", id=sid, latency_ms=80)
        if not more:
            self.closed.add(sid)
        if text.strip():
            for sentence in re.split(r"(?<=[.!?])\s+", text.strip()):
                if sentence:
                    emit("sentence", id=sid, text=sentence)
        seconds = len(text) / self.cps if text.strip() else 0.0
        self.durations[sid] = self.durations.get(sid, 0.0) + seconds
        timer = self.later(seconds, self.chunk_done)
        self.playing = [sid, text, more, timer]
        self.settle()

    def chunk_done(self):
        sid, _text, more, _timer = self.playing
        self.playing = None
        still_queued = any(c[0] == sid for c in self.speech)
        if sid in self.closed and not still_queued:
            self.closed.discard(sid)
            if sid in self.open_ids:
                self.open_ids.remove(sid)
            self.write_wav(sid)
            emit("speak_end", id=sid, interrupted=False)
        self.pump()

    def write_wav(self, sid):
        if not self.args.output_dir:
            return
        os.makedirs(self.args.output_dir, exist_ok=True)
        frames = int(24000 * min(2.0, max(0.05, self.durations.get(sid, 0.05))))
        path = os.path.join(self.args.output_dir, "speak-%s.wav" % re.sub(r"[^A-Za-z0-9_.-]", "_", sid))
        with wave.open(path, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(24000)
            w.writeframes(b"\x00\x00" * frames)

    # -- main loop
    def run(self):
        self.start()
        buf = b""
        while True:
            timeout = 0.5
            while self.timers:
                when, _seq, t = self.timers[0]
                if t.cancelled:
                    heapq.heappop(self.timers)
                    continue
                timeout = max(0.0, min(timeout, when - now()))
                break
            readable, _, _ = select.select([0], [], [], timeout)
            if readable:
                chunk = os.read(0, 65536)
                if not chunk:
                    return 0  # the app went away
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    line = line.strip()
                    if not line:
                        continue
                    if self.log:
                        with open(self.log, "ab") as f:
                            f.write(line + b"\n")
                    try:
                        cmd = json.loads(line)
                    except ValueError:
                        emit("error", message="bad json", fatal=False)
                        continue
                    if isinstance(cmd, dict):
                        self.handle(cmd)
            while self.timers and self.timers[0][0] <= now():
                _when, _seq, t = heapq.heappop(self.timers)
                if not t.cancelled:
                    t.fn()


def main():
    ap = argparse.ArgumentParser(description="fake shadow-voiced for tests")
    ap.add_argument("--probe", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--serve", action="store_true")
    ap.add_argument("--data")
    ap.add_argument("--input-wav")
    ap.add_argument("--fast", action="store_true")
    ap.add_argument("--output-dir")
    args = ap.parse_args()
    if args.probe:
        return probe()
    if args.selftest:
        return selftest()
    if args.serve:
        return Helper(args).run()
    ap.print_usage(sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
