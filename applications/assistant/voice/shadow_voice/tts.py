"""Kokoro-82M speech synthesis (sherpa-onnx) and the speaker.

KokoroEngine turns one normalised sentence into float32 audio at 24 kHz using the
phoneme tokens from g2p.py (sherpa-onnx 2.x would otherwise drop every word that
is missing from its lexicon).

Speaker implements the `speak` / `stop` / `preview` protocol (spec 2.2):

  * speak{id, text, more}: each chunk is split into sentences; a producer thread
    synthesises ahead (at most 3 sentences) while a consumer thread plays, so
    there are no gaps between sentences. `more:true` keeps the id open; while
    nothing is left to say the player is closed and the helper's state falls back
    to idle (the reply is waiting for the assistant, not talking). speak_end comes
    after the audio of the more:false chunk has really finished playing. A chunk
    for a NEW id while the previous id is still open ends the previous id first.
  * stop: barge-in. Queue cleared, player killed at once, speak_end{interrupted}
    for every id that had been accepted; later chunks of an interrupted, still
    open id are dropped until its more:false chunk (it was cut off on purpose).
  * Every accepted id gets exactly one speak_start and one speak_end, even when it
    contains nothing speakable (emoji only), so the app can never wait forever.

Playback pacing stays ~120 ms ahead of the audio clock: tts level events (30 Hz)
line up with what is heard and a stop is audible within ~0.2 s.
"""
import collections
import logging
import os
import queue
import re
import threading
import time

import numpy as np
import sherpa_onnx as so

from .audio import PlaybackError, ProcessPlayer, WavPlayer, available, level_from_rms, PLAYBACK_TOOLS
from .g2p import KokoroG2P

log = logging.getLogger("shadow-voiced")

DEFAULT_VOICE = "af_heart"
PREVIEW_TEXT = "Hi, I'm Shadow. This is how I sound. How can I help you today?"
_FALLBACK_SPEAKERS = ("af_alloy,af_aoede,af_bella,af_heart,af_jessica,af_kore,af_nicole,af_nova,af_river,af_sarah,"
                      "af_sky,am_adam,am_echo,am_eric,am_fenrir,am_liam,am_michael,am_onyx,am_puck,am_santa,"
                      "bf_alice,bf_emma,bf_isabella,bf_lily,bm_daniel,bm_fable,bm_george,bm_lewis").split(",")
LEAD_S = 0.12          # how far writes run ahead of the playback clock
MAX_TOKENS = 500       # Kokoro's context is 510 phoneme tokens


def read_speaker_names(model_path):
    """Speaker names from the model's ONNX metadata ("speaker_names" sits near the end of the file).

    Parsed from the raw protobuf tail instead of a hard-coded table, so the voice
    ids always match the voices.bin/model that is installed."""
    try:
        size = os.path.getsize(model_path)
        with open(model_path, "rb") as f:
            f.seek(max(0, size - 65536))
            tail = f.read()
    except OSError:
        return None
    key = b"speaker_names"
    i = tail.rfind(key)
    if i < 0:
        return None
    j = i + len(key)
    if j >= len(tail) or tail[j] != 0x12:            # field 2 (value), wire type 2 (length-delimited)
        return None
    j += 1
    n, shift = 0, 0
    while j < len(tail):
        b = tail[j]
        j += 1
        n |= (b & 0x7F) << shift
        shift += 7
        if not b & 0x80:
            break
    names = tail[j:j + n].decode("utf-8", "replace").split(",")
    names = [s.strip() for s in names if re.fullmatch(r"[a-z]{2}_[a-z]+", s.strip())]
    return names or None


class KokoroEngine:
    def __init__(self, root, voice=DEFAULT_VOICE, speed=1.0, threads=2, espeak="espeak-ng"):
        d = "models/tts/kokoro/"
        self.dir = root.file(d)
        t = time.monotonic()
        cfg = so.OfflineTtsConfig(
            model=so.OfflineTtsModelConfig(
                kokoro=so.OfflineTtsKokoroModelConfig(
                    model=root.file(d + "model.onnx"), voices=root.file(d + "voices.bin"),
                    tokens=root.file(d + "tokens.txt"),
                    # still required by sherpa-onnx's config check; our phoneme tokens bypass it
                    lexicon=root.file(d + "lexicon-us-en.txt")),
                num_threads=threads, debug=False, provider="cpu"),
            max_num_sentences=1)
        self.tts = so.OfflineTts(cfg)
        self.sample_rate = self.tts.sample_rate
        self.speakers = read_speaker_names(root.file(d + "model.onnx")) or list(_FALLBACK_SPEAKERS)
        # English voices only: the g2p front end speaks English.
        self.voices = [v for v in self.speakers if v[:2] in ("af", "am", "bf", "bm")]
        self.espeak = espeak
        self._g2p = {}
        self._g2p_lock = threading.Lock()
        self.lock = threading.Lock()          # one synthesis at a time (one ORT session)
        self.voice = voice if voice in self.voices else DEFAULT_VOICE
        self.speed = speed
        self.g2p_for(self.voice)
        self.load_s = time.monotonic() - t

    def g2p_for(self, voice):
        british = voice.startswith("b")
        with self._g2p_lock:
            if british not in self._g2p:
                lex = "lexicon-gb-en.txt" if british else "lexicon-us-en.txt"
                self._g2p[british] = KokoroG2P(os.path.join(self.dir, lex), os.path.join(self.dir, "tokens.txt"),
                                               british=british, espeak=self.espeak)
            return self._g2p[british]

    def sentences(self, text, voice=None, first_short=False):
        return self.g2p_for(voice or self.voice).sentences(text, first_short=first_short)

    def tokens(self, sentence, voice=None):
        toks, _ = self.g2p_for(voice or self.voice).phonemize(sentence)
        return toks

    def synth(self, sentence, voice=None, speed=None):
        """One normalised sentence -> float32 mono at self.sample_rate."""
        voice = voice or self.voice
        toks = self.tokens(sentence, voice)
        if not toks:
            return np.zeros(0, np.float32)
        pieces = [toks]
        if len(toks) > MAX_TOKENS:          # very long sentence without commas: cut at word boundaries
            pieces, cur = [], []
            for tk in toks:
                cur.append(tk)
                if len(cur) >= MAX_TOKENS - 20 and tk == " ":
                    pieces.append(cur)
                    cur = []
            if cur:
                pieces.append(cur)
        out = []
        for p in pieces:
            g = so.GenerationConfig()
            g.sid = self.speakers.index(voice) if voice in self.speakers else self.speakers.index(DEFAULT_VOICE)
            g.speed = float(speed or self.speed)
            g.tokens = [p]
            with self.lock:
                audio = self.tts.generate("", g)
            out.append(np.asarray(audio.samples, dtype=np.float32))
        return np.concatenate(out) if out else np.zeros(0, np.float32)

    def warmup(self):
        """The first inferences allocate ONNX Runtime arenas for each input length (~0.5 s
        each): pay that before 'ready' with a short and a typical first-clause length."""
        t = time.monotonic()
        self.synth("Ready.")
        self.synth("Sure, I can help you with that right away.")
        return time.monotonic() - t


class _Chunk:
    __slots__ = ("id", "text", "more", "voice", "speed", "t_cmd", "gen", "first")

    def __init__(self, sid, text, more, voice, speed, t_cmd, gen, first):
        self.id, self.text, self.more, self.voice, self.speed = sid, text, more, voice, speed
        self.t_cmd, self.gen, self.first = t_cmd, gen, first


class _Item:
    __slots__ = ("kind", "id", "text", "pcm", "more", "gen", "t_cmd")

    def __init__(self, kind, chunk, text="", pcm=None):
        self.kind, self.id, self.text, self.pcm = kind, chunk.id, text, pcm
        self.more, self.gen, self.t_cmd = chunk.more, chunk.gen, chunk.t_cmd


class Speaker:
    def __init__(self, engine, emit, state, realtime=True, output_dir=None, playback="auto", device="",
                 on_error=None):
        self.engine = engine
        self.emit = emit
        self.state = state
        self.realtime = realtime
        self.output_dir = output_dir
        self.forced = playback
        self.device = device
        self.on_error = on_error
        self.rate = engine.sample_rate
        self.voice = engine.voice
        self.speed = engine.speed
        self.lock = threading.RLock()
        self.cv = threading.Condition(self.lock)
        self.chunks = collections.deque()
        self.audio_q = queue.Queue(maxsize=3)
        self.gen = 0
        self.open_ids = collections.OrderedDict()   # id -> {"started": bool, "more": bool}
        self.abandoned = set()
        self.producer_busy = False
        self.stopping = threading.Event()
        self.player = None
        self.player_id = None
        self.player_lock = threading.Lock()
        self.tool = None                            # last playback tool that worked
        self.failed_tools = {}                      # tool -> time of last failure
        self._clock = None
        self._played = 0
        self._last_end = 0.0
        self._threads = [threading.Thread(target=self._produce, name="tts-synth", daemon=True),
                         threading.Thread(target=self._consume, name="tts-play", daemon=True)]

    def start(self):
        for t in self._threads:
            t.start()

    # ------------------------------------------------------------------ commands
    def speak(self, sid, text, more=False, voice=None, speed=None):
        with self.cv:
            if sid in self.abandoned:
                if not more:
                    self.abandoned.discard(sid)
                log.info("speak %r dropped: this reply was interrupted (stop)", sid)
                return
            first = sid not in self.open_ids
            if first:
                self.open_ids[sid] = {"started": False, "more": more}
            else:
                self.open_ids[sid]["more"] = more
            self.chunks.append(_Chunk(sid, text or "", bool(more), voice or self.voice,
                                      speed or self.speed, time.monotonic(), self.gen, first))
            self.cv.notify_all()

    def stop(self, reason="stop"):
        """Barge-in: silence now, close every accepted id with speak_end{interrupted:true}."""
        with self.cv:
            self.gen += 1
            ended = list(self.open_ids.items())
            self.open_ids.clear()
            self.chunks.clear()
            for sid, st in ended:
                if st["more"]:
                    self.abandoned.add(sid)
            while len(self.abandoned) > 64:
                self.abandoned.pop()
            self._kill_player()
            for sid, _st in ended:
                self.emit("speak_end", id=sid, interrupted=True)
            self._last_end = time.monotonic()
            self.state.set_speaking(False)
            self.cv.notify_all()
        try:
            while True:
                self.audio_q.get_nowait()
        except queue.Empty:
            pass
        if ended:
            log.info("speech stopped (%s): %s", reason, ", ".join(s for s, _ in ended))
        return [s for s, _ in ended]

    def preview(self, voice, text=None):
        self.stop("preview")
        self.speak("preview", text or PREVIEW_TEXT, more=False, voice=voice)

    def set_voice(self, voice=None, speed=None):
        if voice:
            self.voice = voice
        if speed:
            self.speed = speed

    def busy(self):
        with self.lock:
            return bool(self.open_ids) or bool(self.chunks) or self.producer_busy

    def shutdown(self):
        self.stopping.set()
        with self.cv:
            self.gen += 1
            self.chunks.clear()
            self._kill_player()
            self.cv.notify_all()

    # ------------------------------------------------------------------ producer
    def _produce(self):
        while not self.stopping.is_set():
            with self.cv:
                while not self.chunks and not self.stopping.is_set():
                    self.producer_busy = False
                    self.cv.wait()
                if self.stopping.is_set():
                    return
                chunk = self.chunks.popleft()
                self.producer_busy = True
                if chunk.gen != self.gen:
                    continue
            try:
                sentences = self.engine.sentences(chunk.text, chunk.voice, first_short=chunk.first)
            except Exception as e:     # a normalisation bug must not kill the speaker
                self._error(f"Could not prepare the text for speech: {e}")
                log.exception("text normalisation failed")
                sentences = []
            for s in sentences:
                if chunk.gen != self.gen or self.stopping.is_set():
                    break
                try:
                    pcm = self.engine.synth(s, chunk.voice, chunk.speed)
                except Exception as e:
                    self._error(f"Speech synthesis failed: {e}")
                    log.exception("synthesis failed")
                    continue
                if len(pcm):
                    self._put(_Item("audio", chunk, s, pcm))
            self._put(_Item("chunk_end", chunk))

    def _put(self, item):
        while not self.stopping.is_set() and item.gen == self.gen:
            try:
                self.audio_q.put(item, timeout=0.1)
                return
            except queue.Full:
                continue

    # ------------------------------------------------------------------ consumer
    def _consume(self):
        cur = None             # id that has had speak_start and no speak_end yet
        t_ref = None
        first_audio_sent = False
        gen_seen = self.gen
        while not self.stopping.is_set():
            try:
                item = self.audio_q.get(timeout=0.1)
            except queue.Empty:
                item = None
            if gen_seen != self.gen:                     # a stop happened: stop() emitted the speak_ends
                gen_seen = self.gen
                cur, t_ref, first_audio_sent = None, None, False
                self._close_player(kill=True)
                if not self.busy():
                    self.state.set_speaking(False)
            if item is None:
                with self.lock:
                    idle = not self.chunks and not self.producer_busy and self.audio_q.empty()
                if idle and self.player is not None and not isinstance(self.player, WavPlayer):
                    self._close_player()                 # waiting for more text: stop talking
                    self.state.set_speaking(False)
                elif idle and cur is not None and isinstance(self.player, WavPlayer):
                    self.state.set_speaking(False)
                continue
            if item.gen != self.gen:
                continue
            if cur is not None and item.id != cur:
                self._finish(cur, item.gen)              # implicit end: a new reply started
                cur = None
            if cur is None:
                with self.lock:
                    if item.gen != self.gen:
                        continue
                    st = self.open_ids.get(item.id)
                    if st is not None:
                        st["started"] = True
                    cur, first_audio_sent = item.id, False
                    # latency from the command, or from the end of the reply it had to wait for
                    t_ref = max(item.t_cmd, self._last_end)
                    self.emit("speak_start", id=cur)
                    self.state.set_speaking(True)
            if item.kind == "audio":
                with self.lock:
                    if item.gen == self.gen:
                        self.state.set_speaking(True)
                ok = self._play_sentence(item, first_audio_sent, t_ref)
                if ok:
                    first_audio_sent = True
                elif item.gen == self.gen:
                    # playback impossible: end this id now instead of pretending it was spoken
                    with self.lock:
                        if item.gen == self.gen and item.id in self.open_ids:
                            if self.open_ids[item.id]["more"]:
                                self.abandoned.add(item.id)
                            del self.open_ids[item.id]
                            self.emit("speak_end", id=item.id, interrupted=True)
                    self._drop_queued(item.id)
                    cur = None
                    self.state.set_speaking(False)
            elif item.kind == "chunk_end" and not item.more:
                self._finish(cur, item.gen)
                cur = None

    def _drop_queued(self, sid):
        kept = []
        try:
            while True:
                it = self.audio_q.get_nowait()
                if it.id != sid:
                    kept.append(it)
        except queue.Empty:
            pass
        for it in kept:
            self.audio_q.put_nowait(it)

    def _finish(self, sid, gen):
        """Drain the player, then speak_end (not interrupted)."""
        if sid is None:
            return
        if not isinstance(self.player, WavPlayer) or self.player_id == sid:
            self._close_player()
        with self.lock:
            if gen != self.gen or sid not in self.open_ids:
                return
            del self.open_ids[sid]
            self.emit("speak_end", id=sid, interrupted=False)
            self._last_end = time.monotonic()
            more_waiting = bool(self.chunks) or not self.audio_q.empty()
        if not more_waiting:
            self.state.set_speaking(False)

    # ------------------------------------------------------------------ playback
    def _error(self, message):
        self.emit("error", message=message, fatal=False)
        if self.on_error:
            self.on_error(message)

    def _wav_path(self, sid):
        safe = re.sub(r"[^A-Za-z0-9._-]+", "_", str(sid))[:80] or "speech"
        os.makedirs(self.output_dir, exist_ok=True)
        path = os.path.join(self.output_dir, f"speak-{safe}.wav")
        n = 2
        while os.path.exists(path):
            path = os.path.join(self.output_dir, f"speak-{safe}-{n}.wav")
            n += 1
        return path

    def _tool_order(self):
        if self.forced not in ("", "auto", None):
            return available([self.forced])
        tools = available(PLAYBACK_TOOLS)
        now = time.monotonic()
        # recently failed tools go last, the last tool that worked goes first
        tools.sort(key=lambda t: (now - self.failed_tools.get(t, -1e9) < 60, t != self.tool))
        return tools

    def _open_player(self, sid, exclude=()):
        if self.output_dir:
            return WavPlayer(self._wav_path(sid), self.rate)
        errors = []
        for tool in self._tool_order():
            if tool in exclude:
                continue
            try:
                return ProcessPlayer(tool, self.rate, self.device)
            except OSError as e:
                errors.append(f"{tool}: {e}")
                self.failed_tools[tool] = time.monotonic()
        raise PlaybackError("no audio output tool works" + (f" ({'; '.join(errors)})" if errors else
                                                            " (install pipewire-bin, pulseaudio-utils or alsa-utils)"))

    def _kill_player(self):
        with self.player_lock:
            p = self.player
        if p is not None:
            p.kill()

    def _close_player(self, kill=False):
        with self.player_lock:
            p, self.player, self.player_id = self.player, None, None
        self._clock = None
        if p is None:
            return
        if kill:
            p.kill()
            return
        code = p.drain()
        if code not in (0, None) and not isinstance(p, WavPlayer):
            tail = p.stderr_tail()
            self.failed_tools[p.tool] = time.monotonic()
            self._error(f"Speech playback failed: {p.tool} exited with code {code}" + (f": {tail}" if tail else ""))
        elif not isinstance(p, WavPlayer):
            self.tool = p.tool

    def _play_sentence(self, item, first_audio_sent, t_ref):
        """Play one sentence; falls back to the next playback tool if the current one
        dies before any of this sentence was heard. Returns False when nothing works."""
        pcm = (np.clip(item.pcm, -1.0, 1.0) * 32767.0).astype("<i2")
        tried = set()
        while True:
            if item.gen != self.gen:
                return True
            try:
                with self.player_lock:
                    if self.player is None:
                        self.player = self._open_player(item.id, exclude=tried)
                        self.player_id = item.id
                        self._clock, self._played = None, 0
                    p = self.player
                with self.lock:
                    if item.gen != self.gen:
                        return True
                    if not first_audio_sent:
                        self.emit("first_audio", id=item.id, latency_ms=int(round(1000 * (time.monotonic() - t_ref))))
                        first_audio_sent = True
                    self.emit("sentence", id=item.id, text=item.text)
                self._write_paced(p, pcm, item)
                return True
            except PlaybackError as e:
                if item.gen != self.gen:
                    return True                       # killed on purpose by stop()
                with self.player_lock:
                    p, self.player, self.player_id = self.player, None, None
                tool = getattr(p, "tool", None)
                if p is not None:
                    p.kill()
                if tool:
                    tried.add(tool)
                    self.failed_tools[tool] = time.monotonic()
                log.warning("playback failed: %s", e)
                if self.output_dir or not [t for t in self._tool_order() if t not in tried]:
                    self._error(f"Cannot play speech: {e}")
                    return False
                log.info("trying the next playback tool")

    def _write_paced(self, p, pcm, item):
        hop = self.rate // 30
        pace = self.realtime or not isinstance(p, WavPlayer)
        for i in range(0, len(pcm), hop):
            if item.gen != self.gen or self.stopping.is_set():
                return
            blk = pcm[i:i + hop]
            if pace:
                now = time.monotonic()
                if self._clock is None:
                    self._clock, self._played = now, 0
                due = self._clock + self._played / self.rate - LEAD_S
                d = due - now
                if d > 0:
                    time.sleep(d)
                elif d < -0.25:                       # synthesis fell behind: re-anchor, no catch-up burst
                    self._clock = now - self._played / self.rate + LEAD_S
            p.write(blk)
            self._played += len(blk)
            x = blk.astype(np.float32) / 32768.0
            rms = float(np.sqrt(np.mean(x * x))) if len(x) else 0.0
            with self.lock:
                if item.gen != self.gen:
                    return
                self.emit("level", source="tts", rms=level_from_rms(rms))
