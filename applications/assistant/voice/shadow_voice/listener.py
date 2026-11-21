"""VAD + keyword spotter + STT: the listening state machine (one thread owns it).

Frames (32 ms, 16 kHz) arrive with a `deaf` flag computed when they were captured:
true while the helper speaks and for 0.4 s after (half duplex — we never react to
our own voice). Deaf frames are dropped and the detectors are reset.

Idle (wake word armed):
  * the keyword spotter runs on every frame; a hit emits wake{via:"kws"} at once
    (the UI can light up) and the state becomes "listening";
  * Silero VAD cuts utterances (+0.3 s pre-roll so the "Sh" of "Shadow" is kept);
    every utterance is transcribed locally and checked with wake.match_wake:
    "Shadow, open the terminal"  -> wake{via:"stt"} + final{id:"wake"}
    "Shadow."                    -> wake{via:"stt"}, then the next utterance within
                                    8 s is the command (final{id:"wake"}) or timeout
    anything else                -> dropped unheard (wake_rejected if the KWS had fired)
  Utterances not addressed to Shadow never change the visible state.
listen{id, mode, timeout}: the next utterance is taken without the wake word; the
timeout counts until speech must START. A listen that arrives while the helper is
speaking waits until the speech (and its 0.4 s tail) is over.

Time is audio time (frames processed); when no audio arrives at all (capture
died, end of a test WAV) it advances with the wall clock, so timeouts always fire.
"""
import collections
import logging
import time

import numpy as np
import sherpa_onnx as so

from . import FRAME, MIC_RATE
from .wake import dedupe, kws_threshold, match_wake, yes_no

log = logging.getLogger("shadow-voiced")

PREROLL_S = 0.3
KWS_STANDDOWN_S = 2.0          # KWS fired but nobody spoke: back to idle after this
WAKE_COMMAND_TIMEOUT = 8.0     # after a bare "Shadow."
HISTORY_S = 20.0


class ListenRequest:
    __slots__ = ("id", "mode", "timeout", "deadline")

    def __init__(self, lid, mode="command", timeout=8.0):
        self.id, self.mode, self.timeout, self.deadline = lid, mode, timeout, None


def make_vad(root, threshold=0.5, end_silence=0.6, max_utterance=15.0):
    c = so.VadModelConfig()
    c.silero_vad.model = root.file("models/vad/silero_vad.onnx")
    c.silero_vad.threshold = threshold
    c.silero_vad.min_silence_duration = end_silence
    c.silero_vad.min_speech_duration = 0.2
    c.silero_vad.max_speech_duration = max_utterance
    c.silero_vad.window_size = FRAME
    c.sample_rate = MIC_RATE
    c.num_threads = 1
    return so.VoiceActivityDetector(c, buffer_size_in_seconds=60)


def make_kws(root, sensitivity):
    k = "models/kws/"
    return so.KeywordSpotter(
        tokens=root.file(k + "tokens.txt"), encoder=root.file(k + "encoder.int8.onnx"),
        decoder=root.file(k + "decoder.int8.onnx"), joiner=root.file(k + "joiner.int8.onnx"),
        keywords_file=root.file(k + "keywords.txt"), num_threads=1,
        keywords_threshold=kws_threshold(sensitivity), keywords_score=1.0, num_trailing_blanks=1)


class Listener:
    def __init__(self, root, recognizer, emit, set_state, wake_enabled=True, sensitivity="normal",
                 partials=True, max_utterance=15.0, end_silence=0.6, vad_threshold=0.5, is_deaf=None):
        self.root = root
        self.stt = recognizer
        self.emit = emit
        self.set_state = set_state                  # fn("idle"|"listening"|"transcribing")
        self.is_deaf = is_deaf or (lambda: False)   # wall-clock speaking/tail check (for pending listens)
        self.wake_enabled = wake_enabled
        self.sensitivity = sensitivity
        self.partials = partials
        self.vad = make_vad(root, vad_threshold, end_silence, max_utterance)
        self.kws = make_kws(root, sensitivity) if root.has_kws else None
        self.kstream = self.kws.create_stream() if self.kws else None
        self.audio_t = 0.0
        self.active = None          # ListenRequest being served (a listen, or the command after "Shadow.")
        self.pending = None         # listen requested while speaking
        self.kws_hit_t = None
        self.kws_hits = 0
        self.speech = False
        self.was_deaf = False
        self.finishing = False
        self._hist = collections.deque()          # (first sample index, frame float32)
        self._fed = 0
        self._cur = []
        self._pre = collections.deque(maxlen=int(PREROLL_S * MIC_RATE / FRAME) + 1)
        self._next_partial = 0.0
        self._last_partial = ""
        self.finals = []            # (id, text) — for selftest/tests

    # ---------------------------------------------------------------- state helpers
    def _visible_state(self):
        if self.active is not None or self.kws_hit_t is not None:
            return "listening"
        return "idle"

    def _reset_stream(self):
        self.vad.reset()
        if self.kws:
            self.kws.reset_stream(self.kstream)
        self.speech = False
        self._cur = []
        self._pre.clear()
        self._hist.clear()
        self._fed = 0

    # ---------------------------------------------------------------- commands
    def listen(self, req):
        if self.is_deaf():
            self.pending = req
            log.info("listen %r waits until the speech is over", req.id)
            return
        self._activate(req)

    def _activate(self, req):
        req.deadline = self.audio_t + req.timeout
        self.active = req
        self.kws_hit_t = None
        self._last_partial = ""
        self.set_state("listening")

    def finish(self):
        if self.pending is not None:
            req, self.pending = self.pending, None
            self.emit("timeout", id=req.id)
            self.set_state(self._visible_state())
            return
        if self.active is None:
            log.info("finish ignored: nothing is being listened to")
            return
        if self.speech or self._cur:
            # cut the utterance now and transcribe what we have
            self.finishing = True
            self.vad.flush()
            self._drain_segments()
            if self.active is not None and self.finishing:
                # flush produced nothing usable (too short)
                req, self.active = self.active, None
                self.emit("timeout", id=req.id)
            self.finishing = False
            self._reset_stream()
        else:
            req, self.active = self.active, None
            self.emit("timeout", id=req.id)
        self.set_state(self._visible_state())

    def cancel(self):
        self.active = None
        self.pending = None
        self.kws_hit_t = None
        self._reset_stream()
        self.set_state("idle")

    def reset_audio(self):
        """The audio stream restarts (new capture session): forget any half utterance."""
        if self.speech:
            self.emit("vad", speech=False)
        self._reset_stream()
        self.kws_hit_t = None
        self.set_state(self._visible_state())

    def set_wake(self, enabled):
        self.wake_enabled = enabled
        if not enabled and self.kws_hit_t is not None:
            self.kws_hit_t = None
            self.set_state(self._visible_state())

    def set_sensitivity(self, sensitivity):
        if sensitivity == self.sensitivity:
            return
        self.sensitivity = sensitivity
        if self.root.has_kws:
            self.kws = make_kws(self.root, sensitivity)
            self.kstream = self.kws.create_stream()

    # ---------------------------------------------------------------- time
    def advance(self, seconds):
        """No audio is arriving: let time pass anyway (timeouts must fire)."""
        self.audio_t += seconds
        self.tick()

    def tick(self):
        if self.pending is not None and not self.is_deaf():
            req, self.pending = self.pending, None
            self._activate(req)
        if self.active is not None and not self.speech and self.active.deadline is not None \
                and self.audio_t > self.active.deadline:
            req, self.active = self.active, None
            self.emit("timeout", id=req.id)
            self.set_state(self._visible_state())
        if self.kws_hit_t is not None and not self.speech and self.active is None \
                and self.audio_t - self.kws_hit_t > KWS_STANDDOWN_S:
            self.kws_hit_t = None                 # the spotter fired but nobody spoke
            self.emit("wake_rejected", heard="")
            self.set_state(self._visible_state())

    # ---------------------------------------------------------------- audio
    def feed(self, pcm16, deaf=False):
        self.audio_t += len(pcm16) / MIC_RATE
        if deaf:
            if not self.was_deaf:
                self.was_deaf = True
                if self.speech:
                    self.emit("vad", speech=False)
                self._reset_stream()
            return
        if self.was_deaf:
            self.was_deaf = False
            self._reset_stream()
        if self.pending is not None:
            req, self.pending = self.pending, None
            self._activate(req)
        x = pcm16.astype(np.float32) / 32768.0
        # keyword spotter: only while idle (wake only from idle)
        if self.kws and self.wake_enabled and self.active is None:
            self.kstream.accept_waveform(MIC_RATE, x)
            while self.kws.is_ready(self.kstream):
                self.kws.decode_stream(self.kstream)
                if self.kws.get_result(self.kstream):
                    self.kws.reset_stream(self.kstream)
                    self.kws_hits += 1
                    if self.kws_hit_t is None:
                        self.emit("wake", via="kws")
                    self.kws_hit_t = self.audio_t
                    self.set_state("listening")
        # VAD
        self._hist.append((self._fed, x))
        self._fed += len(x)
        while len(self._hist) > HISTORY_S * MIC_RATE / FRAME:
            self._hist.popleft()
        self.vad.accept_waveform(x)
        speech = self.vad.is_speech_detected()
        if speech != self.speech:
            self.speech = speech
            self.emit("vad", speech=speech)
            if speech:
                self._cur = list(self._pre)
                self._next_partial = self.audio_t + 0.6
        if speech:
            self._cur.append(x)
            self._maybe_partial()
        else:
            self._pre.append(x)
        self._drain_segments()
        self.tick()

    def _extract(self, a, b):
        parts = []
        for s0, f in self._hist:
            lo, hi = max(a, s0), min(b, s0 + len(f))
            if lo < hi:
                parts.append(f[lo - s0:hi - s0])
        return np.concatenate(parts) if parts else np.zeros(0, np.float32)

    def _drain_segments(self):
        while not self.vad.empty():
            seg = self.vad.front
            start, n = seg.start, len(seg.samples)
            self.vad.pop()
            x = self._extract(start - int(PREROLL_S * MIC_RATE), start + n + int(0.1 * MIC_RATE))
            if len(x) < len(seg.samples):            # history too short (very long utterance): use the segment
                x = np.asarray(seg.samples, dtype=np.float32)
            self._cur = []
            self.on_utterance(x)

    def _addressed(self):
        return self.active is not None or self.kws_hit_t is not None

    def _maybe_partial(self):
        if not self.partials or not self._addressed() or self.audio_t < self._next_partial:
            return
        if len(self._cur) * FRAME < 0.8 * MIC_RATE:
            return
        t0 = time.monotonic()
        try:
            text, _raw, _ms = self.stt.transcribe(np.concatenate(self._cur))
        except Exception as e:
            log.warning("partial transcription failed: %s", e)
            self._next_partial = self.audio_t + 2.0
            return
        cost = time.monotonic() - t0
        # pace partials by what decoding costs on this CPU, so the audio thread stays real-time
        self._next_partial = self.audio_t + max(0.5, 2.5 * cost)
        text = dedupe(text)
        woke, rest, _how = match_wake(text, kws_hit=self.kws_hit_t is not None, sensitivity=self.sensitivity)
        if self.active is None and self.kws_hit_t is not None and not woke and len(text.split()) >= 2:
            # the spotter fired, but the words do not start with "Shadow": stand down early
            self.kws_hit_t = None
            self.emit("wake_rejected", heard=text)
            self.set_state(self._visible_state())
            return
        shown = rest if woke else (text if self.active is not None else "")
        if shown and shown != self._last_partial:
            self._last_partial = shown
            self.emit("partial", text=shown)

    # ---------------------------------------------------------------- utterances
    def on_utterance(self, x):
        dur = round(len(x) / MIC_RATE, 2)
        if self.active is not None:
            self._serve_listen(x, dur)
            return
        if not self.wake_enabled:
            return                                      # nobody asked: dropped unheard
        kws = self.kws_hit_t is not None
        self.kws_hit_t = None
        if kws:
            self.set_state("transcribing")              # we believe this is addressed to us
        try:
            text, _raw, ms = self.stt.transcribe(x)
        except Exception as e:
            self.emit("error", message=f"Speech recognition failed: {e}", fatal=False)
            log.exception("transcription failed")
            self.set_state(self._visible_state())
            return
        woke, rest, how = match_wake(text, kws_hit=kws, sensitivity=self.sensitivity)
        if not woke:
            if kws:
                self.emit("wake_rejected", heard=text)
            self.set_state(self._visible_state())
            return
        self.emit("wake", via="stt")
        if rest:
            self._final("wake", "command", rest, text, ms, dur, how)
            self.set_state(self._visible_state())
        else:
            self._activate(ListenRequest("wake", "command", WAKE_COMMAND_TIMEOUT))

    def _serve_listen(self, x, dur):
        req = self.active
        self.set_state("transcribing")
        try:
            text, _raw, ms = self.stt.transcribe(x)
        except Exception as e:
            self.emit("error", message=f"Speech recognition failed: {e}", fatal=False)
            log.exception("transcription failed")
            self.active = None
            self.finishing = False
            self.emit("timeout", id=req.id)
            self.set_state(self._visible_state())
            return
        woke, rest, how = match_wake(text, kws_hit=False, sensitivity=self.sensitivity)
        if woke and not rest and not self.finishing:
            req.deadline = self.audio_t + req.timeout     # "Shadow" again: still listening
            self.set_state("listening")
            return
        command = rest if woke else text
        if not command:
            if self.finishing or self.audio_t >= req.deadline:
                self.active = None
                self.finishing = False
                self.emit("timeout", id=req.id)
                self.set_state(self._visible_state())
            else:
                self.set_state("listening")               # a cough or noise: keep listening
            return
        self.active = None
        self.finishing = False
        self._final(req.id, req.mode, command, text, ms, dur, how if woke else "")
        self.set_state(self._visible_state())

    def _final(self, lid, mode, text, heard, ms, dur, how):
        fields = dict(text=text, heard=heard, stt_ms=ms, id=lid, mode=mode, seconds=dur, stt=self.stt.name)
        if how:
            fields["wake_match"] = how
        if mode == "yesno":
            fields["answer"] = yes_no(text)
        self.finals.append((lid, text))
        self.emit("final", **fields)
