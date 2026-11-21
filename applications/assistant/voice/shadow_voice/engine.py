"""shadow-voiced --serve: the long-running engine process owned by the app (QProcess).

Threads (all daemon; the main thread only loads models and supervises shutdown):
  stdin      reads JSON commands; cheap dispatch only (queues and flags)
  capture    supervises the capture tool: start/stop on demand (muted, wake off),
             retries with backoff 1,2,4,8,15 s, falls back to the next tool, detects a
             silent microphone, emits mic levels, feeds frames to `listen`
  listen     owns VAD/KWS/STT (listener.Listener): frames + listen/finish/cancel
  tts-synth / tts-play   the speaker (tts.Speaker)

Shutdown happens on quit, stdin EOF (the app died), SIGTERM/SIGINT, a broken stdout
or a fatal error: children (capture/playback tools) are killed first, then we exit.
Each child also carries a parent-death signal, so even SIGKILL leaves no orphans.
"""
import logging
import os
import queue
import signal
import sys
import threading
import time

from . import FRAME, MIC_RATE, VERSION
from . import audio
from .audio import CaptureError, ProcessCapture, WavCapture, frame_stats, level_from_rms
from .listener import Listener, ListenRequest
from .protocol import as_bool, as_float, parse_command
from .stt import Recognizer
from .tts import KokoroEngine, Speaker

log = logging.getLogger("shadow-voiced")

BACKOFF = (1, 2, 4, 8, 15)
SILENT_REPORT_S = 10.0
SILENT_MESSAGE = "The microphone is silent (muted, or the wrong input is selected)"
HALF_DUPLEX_TAIL = 0.4
CAPTURE_IDLE_GRACE = 2.0
QUEUE_FRAMES = int(30 * MIC_RATE / FRAME)
LISTEN_MODES = ("command", "yesno", "followup")
SENSITIVITIES = ("low", "normal", "high")


class StateTracker:
    """The helper-side state (spec: idle / listening / transcribing / speaking / muted)."""

    def __init__(self, emit, tail=HALF_DUPLEX_TAIL):
        self.emit = emit
        self.tail = tail
        self.lock = threading.Lock()
        self.muted = False
        self.speaking = False
        self.listener = "idle"
        self.speech_end = -1e9
        self.value = None

    def _derive(self):
        if self.speaking:
            return "speaking"
        if self.listener in ("transcribing", "listening"):
            return self.listener
        return "muted" if self.muted else "idle"

    def _update(self):
        v = self._derive()
        if v != self.value:
            self.value = v
            self.emit("state", value=v)

    def set_speaking(self, b):
        with self.lock:
            if b != self.speaking:
                self.speaking = b
                if not b:
                    self.speech_end = time.monotonic()
                self._update()

    def set_listener(self, v):
        with self.lock:
            self.listener = v
            self._update()

    def set_muted(self, b):
        with self.lock:
            self.muted = b
            self._update()

    def is_deaf(self, now=None):
        now = time.monotonic() if now is None else now
        with self.lock:
            return self.speaking or (now - self.speech_end) < self.tail


class Engine:
    def __init__(self, args, root, emitter):
        self.args = args
        self.root = root
        self.emitter = emitter
        self.emit = self._emit
        self.state = StateTracker(self.emit)
        self.shutdown = threading.Event()
        self.exit_code = 0
        self.ready = threading.Event()
        self.pending_cmds = []
        self.cmd_lock = threading.Lock()
        self.proc_q = queue.Queue(maxsize=QUEUE_FRAMES)
        self.capture_wakeup = threading.Condition()
        self.capture_stop = threading.Event()
        self.capture_source = None
        self.capture_tool = None
        self.capture_status = None
        self.input_done = False
        self.wav_state = {}
        # configuration (spec 2.2 config command)
        self.muted = bool(args.muted)
        self.wake_enabled = not args.no_wake
        self.sensitivity = args.wake_sensitivity
        self.stt_choice = args.stt
        self.input_device = args.input_device or ""
        self.follow_up_secs = 5.0
        self.listen_id = None                  # a listen is active/pending (capture must run)
        self.listen_since = 0.0
        self.last_needed = time.monotonic()
        self.listener = None
        self.speaker = None
        self.tts = None
        self.recognizers = {}
        self._overflow_warned = False
        self._level_t = 0.0
        emitter.on_broken = lambda: self.request_exit(0, "stdout closed")

    # ------------------------------------------------------------------ events
    def _emit(self, event, **fields):
        obj = self.emitter.emit(event, **fields)
        if event in ("final", "timeout") and fields.get("id") == self.listen_id:
            self.listen_id = None
            self._capture_changed()
        if event == "final":
            log.info("final id=%s mode=%s stt_ms=%s%s", fields.get("id"), fields.get("mode"), fields.get("stt_ms"),
                     f" text={fields.get('text')!r}" if self.args.debug else "")
        elif event in ("wake", "timeout", "wake_rejected", "mic", "error", "speak_start", "speak_end"):
            log.info("%s %s", event, {k: v for k, v in fields.items() if k != "heard" or self.args.debug})
        return obj

    def request_exit(self, code, why=""):
        if not self.shutdown.is_set():
            log.info("shutting down (%s), exit code %s", why or "requested", code)
            self.exit_code = code
            self.shutdown.set()
            with self.capture_wakeup:
                self.capture_wakeup.notify_all()

    # ------------------------------------------------------------------ startup
    def stt_name(self, choice=None):
        choice = choice or self.stt_choice
        models = self.root.stt_models
        if choice in models:
            return choice
        return self.root.stt_default or models[0]

    def load(self):
        t0 = time.monotonic()
        a = self.args
        name = self.stt_name()
        errors = []

        def load_stt():
            try:
                self.recognizers[name] = Recognizer(self.root, name, threads=a.stt_threads)
            except Exception as e:      # reported by the main thread
                errors.append(f"speech-to-text model '{name}': {e}")

        th = threading.Thread(target=load_stt, name="load-stt", daemon=True)
        th.start()
        self.tts = KokoroEngine(self.root, voice=a.voice, speed=a.speed, threads=a.tts_threads, espeak=a.espeak)
        warm = self.tts.warmup()
        th.join()
        if errors:
            raise RuntimeError(errors[0])
        rec = self.recognizers[name]
        self.listener = Listener(self.root, rec, self.emit, self.state.set_listener,
                                 wake_enabled=self.wake_enabled, sensitivity=self.sensitivity,
                                 partials=not a.no_partials, max_utterance=a.max_utterance,
                                 end_silence=a.end_silence, vad_threshold=a.vad_threshold,
                                 is_deaf=self.state.is_deaf)
        self.speaker = Speaker(self.tts, self.emit, self.state, realtime=not a.fast,
                               output_dir=a.output_dir, playback=a.playback, device=a.output_device or "")
        log.info("models loaded in %.2f s (tts %.2f s + warm-up %.2f s, stt %s %.2f s)",
                 time.monotonic() - t0, self.tts.load_s, warm, name, rec.load_s)
        return time.monotonic() - t0

    def run(self):
        self.emit("hello", version=VERSION, pid=os.getpid())
        signal.signal(signal.SIGTERM, lambda *_: self.request_exit(0, "SIGTERM"))
        signal.signal(signal.SIGINT, lambda *_: self.request_exit(130, "SIGINT"))
        signal.signal(signal.SIGHUP, lambda *_: self.request_exit(0, "SIGHUP"))
        threading.Thread(target=self._stdin_loop, name="stdin", daemon=True).start()
        try:
            load_s = self.load()
        except Exception as e:
            # the error event goes out BEFORE the traceback is logged (the app reads both)
            self.emitter.emit("error", message=f"Cannot load the voice models: {e}. Re-run shadow-voice-setup.",
                              fatal=True)
            log.exception("loading the voice models failed")
            return 2
        if self.shutdown.is_set():
            return self.exit_code
        self.speaker.start()
        threading.Thread(target=self._listen_loop, name="listen", daemon=True).start()
        if self.args.input_wav:
            cap = "input-wav"
        elif self.args.capture_command:
            cap = "capture-command"
        else:
            cap = (audio.capture_order(self.input_device, self.args.capture) or [None])[0]
        play = "output-dir" if self.args.output_dir else (self.speaker._tool_order() or [None])[0]
        self.emit("ready", load_s=round(load_s, 2), stt=self.stt_name(), stt_models=self.root.stt_models,
                  tts="kokoro", voice=self.tts.voice, voices=self.tts.voices, kws=self.root.has_kws,
                  capture=cap, playback=play, data=self.root.path, version=VERSION,
                  sherpa_onnx=getattr(sys.modules.get("sherpa_onnx"), "__version__", "?"))
        self.state.set_muted(self.muted)
        self.state.set_listener("idle")
        # Replay what arrived while loading, in order, BEFORE new commands are let through
        # (the stdin thread waits on cmd_lock meanwhile) and before the microphone opens,
        # so an early {"cmd":"mute"} or config is honoured from the first frame on.
        with self.cmd_lock:
            for msg in self.pending_cmds:
                try:
                    self._dispatch(msg)
                except Exception as e:          # same rule as the stdin loop: report, keep running
                    self.emit("error", message=f"command {msg.get('cmd')!r} failed: {e}", fatal=False)
                    log.exception("queued command %r failed", msg.get("cmd"))
            self.pending_cmds = []
            self.ready.set()
        threading.Thread(target=self._capture_loop, name="capture", daemon=True).start()
        while not self.shutdown.wait(0.5):
            pass
        self._cleanup()
        return self.exit_code

    def _cleanup(self):
        deadline = threading.Timer(4.0, lambda: os._exit(self.exit_code or 0))
        deadline.daemon = True
        deadline.start()
        if self.speaker:
            self.speaker.shutdown()
        self.capture_stop.set()
        src = self.capture_source
        if src is not None:
            src.close()
        deadline.cancel()

    # ------------------------------------------------------------------ commands
    def _stdin_loop(self):
        fd = sys.stdin.fileno()
        buf = b""
        while not self.shutdown.is_set():
            try:
                chunk = os.read(fd, 65536)
            except InterruptedError:
                continue
            except OSError as e:
                log.warning("stdin read failed: %s", e)
                chunk = b""
            if not chunk:
                self.request_exit(0, "stdin closed (the app went away)")
                return
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                msg, err = parse_command(line)
                if err:
                    self.emit("error", message=err, fatal=False)
                elif msg is not None:
                    try:
                        self.handle(msg)
                    except Exception as e:          # a bad command must never kill the engine
                        self.emit("error", message=f"command {msg.get('cmd')!r} failed: {e}", fatal=False)
                        log.exception("command %r failed", msg.get("cmd"))
            if len(buf) > (1 << 20):
                self.emit("error", message="command line too long; discarded", fatal=False)
                buf = b""

    def handle(self, msg):
        cmd = msg.get("cmd")
        if cmd == "ping":
            self.emit("pong")
            return
        if cmd == "quit":
            self.request_exit(0, "quit")
            return
        with self.cmd_lock:
            if not self.ready.is_set():
                self.pending_cmds.append(msg)
                return
        self._dispatch(msg)

    def _dispatch(self, msg):
        cmd = msg.get("cmd")
        fn = {"config": self._cmd_config, "listen": self._cmd_listen, "finish": self._cmd_finish,
              "cancel": self._cmd_cancel, "speak": self._cmd_speak, "stop": self._cmd_stop,
              "preview": self._cmd_preview, "mute": self._cmd_mute}.get(cmd)
        if cmd == "_raise" and os.environ.get("SHADOW_VOICED_TEST_HOOKS") == "1":
            # test hook (only with SHADOW_VOICED_TEST_HOOKS=1): an unexpected exception in the
            # listen thread, to verify the fatal-error path end to end
            self._post("raise")
            return
        if fn is None:
            self.emit("error", message=f"unknown command {cmd!r}", fatal=False)
            return
        fn(msg)

    def _post(self, *item):
        """Control message for the listen thread (ordered with the audio frames)."""
        try:
            self.proc_q.put(("cmd",) + item, timeout=2.0)
        except queue.Full:
            log.error("listen queue full; dropping %s", item[0])

    def _cmd_config(self, msg):
        applied, rejected, ignored = {}, {}, []
        for key, val in msg.items():
            if key == "cmd":
                continue
            if key == "voice":
                if isinstance(val, str) and val in self.tts.voices:
                    self.speaker.set_voice(voice=val)
                    self.tts.voice = val
                    applied[key] = val
                else:
                    rejected[key] = f"unknown voice {val!r} (have: {', '.join(self.tts.voices)})"
            elif key == "speed":
                v = as_float(val, 0.7, 1.4)
                if v is None:
                    rejected[key] = "expected a number between 0.7 and 1.4"
                else:
                    self.speaker.set_voice(speed=v)
                    self.tts.speed = v
                    applied[key] = v
            elif key == "wake":
                b = as_bool(val)
                if b is None:
                    rejected[key] = "expected true or false"
                else:
                    self.wake_enabled = b
                    self._post("wake", b)
                    self._capture_changed()
                    applied[key] = b
            elif key == "wake_sensitivity":
                if val in SENSITIVITIES:
                    self.sensitivity = val
                    self._post("sensitivity", val)
                    applied[key] = val
                else:
                    rejected[key] = "expected low, normal or high"
            elif key == "stt":
                if val == "auto" or val in self.root.stt_models:
                    self.stt_choice = val
                    applied[key] = val
                    self._switch_stt(self.stt_name(val))
                elif val in ("moonshine", "whisper"):
                    rejected[key] = f"the {val} model is not installed (run: shadow-voice-setup --stt {val})"
                else:
                    rejected[key] = "expected auto, moonshine or whisper"
            elif key == "input_device":
                if isinstance(val, str):
                    dev = val.strip()
                    if dev != self.input_device:
                        self.input_device = dev
                        self._restart_capture("input device changed")
                    applied[key] = dev
                else:
                    rejected[key] = "expected a device name string"
            elif key == "muted":
                b = as_bool(val)
                if b is None:
                    rejected[key] = "expected true or false"
                else:
                    self._set_muted(b)
                    applied[key] = b
            elif key == "follow_up_secs":
                v = as_float(val, 0, 60)
                if v is None:
                    rejected[key] = "expected a number of seconds"
                else:
                    self.follow_up_secs = v
                    applied[key] = v
            else:
                ignored.append(key)
        reply = {"applied": applied}
        if rejected:
            reply["rejected"] = rejected
            for k, why in rejected.items():
                self.emit("error", message=f"config {k}: {why}", fatal=False)
        if ignored:
            reply["ignored"] = ignored
        self.emit("config_ok", **reply)

    def _switch_stt(self, name):
        if self.listener is None or self.listener.stt.name == name:
            return

        def load():
            try:
                rec = self.recognizers.get(name) or Recognizer(self.root, name, threads=self.args.stt_threads)
            except Exception as e:
                self.emit("error", message=f"Cannot load the {name} speech model: {e}", fatal=False)
                log.exception("loading STT %s failed", name)
                return
            self.recognizers = {name: rec}         # free the other model (~150 MB)
            self._post("stt", rec)
            log.info("speech-to-text switched to %s (%.2f s)", name, rec.load_s)

        threading.Thread(target=load, name="load-stt", daemon=True).start()

    def _cmd_listen(self, msg):
        lid = msg.get("id")
        lid = str(lid) if lid not in (None, "") else "listen"
        mode = msg.get("mode") or "command"
        if mode not in LISTEN_MODES:
            self.emit("error", message=f"listen: unknown mode {mode!r} (command, yesno, followup)", fatal=False)
            mode = "command"
        default = self.follow_up_secs if mode == "followup" and self.follow_up_secs else (5.0 if mode == "followup" else 8.0)
        timeout = as_float(msg.get("timeout"), 1.0, 60.0) if msg.get("timeout") is not None else default
        if timeout is None:
            timeout = default
        if self.muted:
            self.emit("error", message="Cannot listen: the microphone is muted", fatal=False)
            self.emit("timeout", id=lid, reason="muted")
            return
        if self.input_done:
            self.emit("timeout", id=lid, reason="no-input")
            return
        self.listen_id = lid
        self.listen_since = time.monotonic()
        self._capture_changed()
        self._post("listen", ListenRequest(lid, mode, timeout))

    def _cmd_finish(self, _msg):
        self._post("finish")

    def _cmd_cancel(self, _msg):
        self.listen_id = None
        self._post("cancel")
        self._capture_changed()

    def _cmd_speak(self, msg):
        sid = msg.get("id")
        sid = str(sid) if sid not in (None, "") else "speak"
        text = msg.get("text")
        if not isinstance(text, str):
            text = "" if text is None else str(text)
        more = as_bool(msg.get("more"), False)
        self.speaker.speak(sid, text, more=bool(more))

    def _cmd_stop(self, _msg):
        self.speaker.stop()

    def _cmd_preview(self, msg):
        voice = msg.get("voice") or self.tts.voice
        if voice not in self.tts.voices:
            self.emit("error", message=f"preview: unknown voice {voice!r}", fatal=False)
            self.emit("speak_start", id="preview")
            self.emit("speak_end", id="preview", interrupted=True)
            return
        text = msg.get("text") if isinstance(msg.get("text"), str) and msg.get("text").strip() else None
        self.speaker.preview(voice, text)

    def _cmd_mute(self, msg):
        b = as_bool(msg.get("value"), True)
        self._set_muted(True if b is None else b)

    def _set_muted(self, b):
        if b == self.muted:
            return
        self.muted = b
        if b:
            self.listen_id = None
            self._post("cancel")
        self.state.set_muted(b)
        self._capture_changed()

    # ------------------------------------------------------------------ capture
    def capture_needed(self):
        if self.muted or self.shutdown.is_set() or self.input_done:
            return False
        return self.wake_enabled or self.listen_id is not None

    def _capture_changed(self):
        if self.capture_needed():
            self.last_needed = time.monotonic()
        with self.capture_wakeup:
            self.capture_wakeup.notify_all()

    def _restart_capture(self, why):
        log.info("restarting capture: %s", why)
        self.capture_stop.set()
        with self.capture_wakeup:
            self.capture_wakeup.notify_all()

    def _mic(self, status, tool, message):
        self.capture_status = status
        self.emit("mic", status=status, tool=tool, message=message)

    def _open_source(self, tools, attempt):
        a = self.args
        if a.input_wav:
            return WavCapture(a.input_wav, realtime=not a.fast, shared=self.wav_state)
        if a.capture_command:
            argv = ["/bin/sh", "-c", a.capture_command]
            src = ProcessCapture("capture-command", argv)
            return src
        tool = tools[attempt % len(tools)]
        note = audio.device_note(tool, self.input_device)
        if not note and audio.device_exists(tool, self.input_device) is False:
            note = f"input device '{self.input_device}' was not found; {tool} uses the default input"
        if note:
            log.warning(note)
        src = ProcessCapture(tool, audio.capture_argv(tool, self.input_device))
        src.note = note
        return src

    def _capture_loop(self):
        step = 0
        attempt = 0
        while not self.shutdown.is_set():
            if not self._wait_needed():
                continue
            tools = audio.capture_order(self.input_device, self.args.capture)
            if not tools and not (self.args.input_wav or self.args.capture_command):
                if self.capture_status != "error":
                    self._mic("error", None, "No microphone capture tool: install pipewire-bin "
                                             "(or pulseaudio-utils / alsa-utils)")
                self._sleep_interruptible(BACKOFF[-1])
                continue
            self.capture_stop.clear()
            try:
                src = self._open_source(tools, attempt)
            except (OSError, ValueError) as e:
                tool = tools[attempt % len(tools)] if tools else "capture"
                wait = BACKOFF[min(step, len(BACKOFF) - 1)]
                self._mic("error", tool, f"Cannot start {tool}: {e}. Retrying in {wait} s.")
                attempt += 1
                step += 1
                self._sleep_interruptible(wait)
                continue
            self.capture_source = src
            self.capture_tool = src.tool
            # A new capture session: half an utterance from before the gap (mute, mic closed
            # while not needed, a crashed recorder) must never be attributed to a later listen.
            self._post("reset")
            outcome, ran_s, detail = self._read_source(src)
            self.capture_source = None
            code = src.close()
            if outcome == "stopped":
                if self.capture_status not in (None, "stopped"):
                    why = "muted" if self.muted else ("shutting down" if self.shutdown.is_set() else
                                                      ("input device changed" if self.capture_needed() else
                                                       "not needed while the wake word is off"))
                    self._mic("stopped", src.tool, f"Microphone closed: {why}")
                continue
            if outcome == "eof" and isinstance(src, WavCapture):
                self.input_done = True
                self.emit("eof")                      # test mode: the input WAV has been heard completely
                self._mic("stopped", src.tool, "End of the input WAV")
                continue
            # the capture died, stalled or never delivered audio
            if ran_s > 30:
                step = 0
            if src.frames == 0:
                attempt += 1                     # this tool does not work here: try the next one
            wait = BACKOFF[min(step, len(BACKOFF) - 1)]
            step += 1
            reason = detail or (f"{src.tool} exited with code {code}" if code not in (None, 0) else f"{src.tool} stopped")
            tail = src.stderr_tail() if hasattr(src, "stderr_tail") else ""
            if tail and tail not in reason:
                reason += f": {tail}"
            self._mic("error", src.tool, f"Microphone capture failed ({reason}). Retrying in {wait} s.")
            self._sleep_interruptible(wait)

    def _wait_needed(self):
        """Block until capture is needed (mic unmuted, wake word on or a listen pending)."""
        with self.capture_wakeup:
            while not self.shutdown.is_set() and not self.capture_needed():
                self.capture_wakeup.wait(0.5)
        return not self.shutdown.is_set()

    def _sleep_interruptible(self, seconds):
        end = time.monotonic() + seconds
        with self.capture_wakeup:
            while not self.shutdown.is_set():
                left = end - time.monotonic()
                if left <= 0 or self.capture_stop.is_set() or not self.capture_needed():
                    break
                self.capture_wakeup.wait(min(left, 0.5))

    def _read_source(self, src):
        """Pump frames until stopped / EOF / failure. Returns (outcome, seconds, detail)."""
        t0 = time.monotonic()
        silent_s = 0.0
        silent_reported = False
        got_audio = False
        fast_wav = isinstance(src, WavCapture) and self.args.fast
        while True:
            if self.shutdown.is_set() or self.capture_stop.is_set():
                return "stopped", time.monotonic() - t0, ""
            if not self.capture_needed():
                # not needed any more: keep the mic a little longer in case a listen follows at once
                if time.monotonic() - self.last_needed > CAPTURE_IDLE_GRACE:
                    return "stopped", time.monotonic() - t0, ""
            else:
                self.last_needed = time.monotonic()
            try:
                frame = src.read_frame(self.capture_stop)
            except CaptureError as e:
                return "failed", time.monotonic() - t0, str(e)
            if frame is None:
                if self.shutdown.is_set() or self.capture_stop.is_set():
                    return "stopped", time.monotonic() - t0, ""
                return "eof", time.monotonic() - t0, ""
            if not got_audio:
                got_audio = True
                note = getattr(src, "note", "")
                dev = self.input_device or "default input"
                self._mic("ok", src.tool, f"Listening with {src.tool} ({dev})" if not note else
                          f"Listening with {src.tool}: {note}")
            rms, ac, _peak = frame_stats(frame)
            if ac < audio.SILENT_AC:
                silent_s += FRAME / MIC_RATE
                if silent_s >= SILENT_REPORT_S and not silent_reported:
                    silent_reported = True
                    self._mic("error", src.tool, SILENT_MESSAGE)
            else:
                if silent_reported:
                    self._mic("ok", src.tool, "Microphone sound is back")
                silent_reported = False
                silent_s = 0.0
            deaf = self.state.is_deaf()
            now = time.monotonic()
            if not deaf and not self.muted and now - self._level_t >= 0.025:
                self._level_t = now
                self.emit("level", source="mic", rms=level_from_rms(rms))
            item = ("frame", frame, deaf)
            if fast_wav:
                while not self.shutdown.is_set() and not self.capture_stop.is_set():
                    try:
                        self.proc_q.put(item, timeout=0.2)
                        break
                    except queue.Full:
                        continue
            else:
                try:
                    self.proc_q.put_nowait(item)
                except queue.Full:
                    try:
                        self.proc_q.get_nowait()     # drop the oldest audio, keep up with real time
                        self.proc_q.put_nowait(item)
                    except (queue.Empty, queue.Full):
                        pass
                    if not self._overflow_warned:
                        self._overflow_warned = True
                        log.warning("speech processing is falling behind; dropping old audio")

    # ------------------------------------------------------------------ listen thread
    def _listen_loop(self):
        L = self.listener
        last_frame = time.monotonic()
        last_tick = last_frame
        while not self.shutdown.is_set():
            try:
                item = self.proc_q.get(timeout=0.1)
            except queue.Empty:
                item = None
            now = time.monotonic()
            if item is None:
                if now - last_frame > 0.25:
                    L.advance(now - last_tick)
                else:
                    L.tick()
                last_tick = now
                continue
            if item[0] == "frame":
                last_frame = last_tick = now
                L.feed(item[1], deaf=item[2])
                continue
            _, name, *rest = item
            if name == "listen":
                L.listen(rest[0])
            elif name == "finish":
                L.finish()
            elif name == "cancel":
                L.cancel()
            elif name == "wake":
                L.set_wake(rest[0])
            elif name == "sensitivity":
                L.set_sensitivity(rest[0])
            elif name == "stt":
                L.stt = rest[0]
            elif name == "reset":
                L.reset_audio()
            elif name == "raise":
                raise RuntimeError("test hook: simulated internal error")
