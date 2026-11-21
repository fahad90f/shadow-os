"""shadow-voiced --serve end to end: the spec 2.2 protocol driven like the app drives it.

The microphone is replaced by session WAVs synthesised at test time with the engine's
own Kokoro voices (+ silence and light noise); the speaker by --output-dir. Capture
failures and silent microphones use --capture-command with small fake recorders.
Needs an installed engine data root (see harness.py).
"""
import os
import re
import signal
import subprocess
import tempfile
import time
import unittest

from harness import DATA, TMP, VOICED, Helper, fake_mic, need_data, processes_with, session, wav_seconds
from shadow_voice.wake import wer

SILENCE_THEN_NOISE = """
import os, sys, time, random
# 11 s of digital silence, then quiet room noise (-50 dBFS), paced in real time
chunk = 3200  # 100 ms of 16 kHz s16 mono
t0 = time.monotonic(); i = 0
while True:
    if i < 110:
        data = bytes(chunk)
    else:
        data = b"".join(int(random.gauss(0, 100)).to_bytes(2, "little", signed=True) for _ in range(chunk // 2))
    sys.stdout.buffer.write(data); sys.stdout.flush()
    i += 1
    d = t0 + i * 0.1 - time.monotonic()
    if d > 0:
        time.sleep(d)
"""
QUIET_MIC = """
import sys, time
chunk = bytes(3200)
t0 = time.monotonic(); i = 0
while True:
    sys.stdout.buffer.write(chunk); sys.stdout.flush(); i += 1
    d = t0 + i * 0.1 - time.monotonic()
    if d > 0:
        time.sleep(d)
"""
NOISE_THEN_DIE = """
import random, sys, time
for i in range(10):     # 1 s of noise in real time, then the "device" disappears
    sys.stdout.buffer.write(b"".join(int(random.gauss(0, 300)).to_bytes(2, "little", signed=True) for _ in range(1600)))
    sys.stdout.flush(); time.sleep(0.1)
sys.stderr.write("fake-mic: device unplugged\\n")
sys.exit(1)
"""


def text_ok(got, want, max_wer=0.0):
    return wer(want, got) <= max_wer


@need_data
class WakeWord(unittest.TestCase):
    def test_wake_flow(self):
        """one breath, bare wake + command, a mention, a plural: only the addressed ones are finals."""
        wav, tl = session("wake", [1.0, ("The library will be closed on Monday.", "am_michael"), 1.5,
                                   ("Shadow, open the terminal.", "af_heart"), 2.5, ("Shadow.", "bf_emma"), 1.0,
                                   ("What time is it?", "bf_emma"), 2.5,
                                   ("I saw my shadow on the wall this morning.", "am_michael"), 1.5,
                                   ("Shadows grow longer in winter.", "af_sarah"), 2.5])
        h = Helper("--input-wav", wav, "--output-dir", os.path.join(TMP, "out-wake"), log_name="wake")
        try:
            h.ready()
            eof = h.wait("eof", timeout=wav_seconds(wav) + 20)
            self.assertEqual(h.wait("mic", start=eof["_index"], status="stopped")["tool"], "input-wav")
            time.sleep(1.5)
            finals = h.named("final")
            self.assertEqual(len(finals), 2, f"finals: {[f['text'] for f in finals]}")
            self.assertTrue(text_ok(finals[0]["text"], "open the terminal"), finals[0])
            self.assertTrue(text_ok(finals[1]["text"], "what time is it"), finals[1])
            for f in finals:
                self.assertEqual(f["id"], "wake")
                self.assertEqual(f["mode"], "command")
                self.assertIsInstance(f["stt_ms"], int)
                self.assertTrue(f["heard"])
            self.assertGreaterEqual(len(h.named("wake", via="stt")), 2)
            states = [e["value"] for e in h.named("state")]
            self.assertIn("listening", states)
            self.assertIn("transcribing", states)
            levels = h.named("level", source="mic")
            self.assertGreater(len(levels), 100, "mic level events missing")
            self.assertTrue(all(0.0 <= e["rms"] <= 1.0 for e in levels))
            for e in h.events:
                self.assertIn("t", e)
        finally:
            self.assertEqual(h.close(), 0)

    def test_only_from_idle(self):
        """during a listen, "Shadow, ..." is the listen's utterance (no wake event)"""
        wav, _ = session("idle", [2.0, ("Shadow, lock the screen.", "af_heart"), 3.0])
        h = Helper("--input-wav", wav, "--output-dir", os.path.join(TMP, "out-idle"), log_name="idle")
        try:
            h.ready()
            h.send(cmd="listen", id="L1", mode="command", timeout=8)
            f = h.wait("final", id="L1", timeout=20)
            self.assertTrue(text_ok(f["text"], "lock the screen"), f)
            self.assertEqual(h.named("wake"), [])
        finally:
            h.close()


@need_data
class PushToTalk(unittest.TestCase):
    @staticmethod
    def wav_t0(h):
        """Wall time (monotonic) at which the input WAV started to play (first mic ok)."""
        return h.t0 + h.wait("mic", status="ok")["_wall"]
    def test_listen_finish_cancel_timeout(self):
        wav, tl = session("ptt", [3.0, ("Open the file manager.", "af_heart"), 2.0,
                                  ("Please open the downloads folder and sort all of the files by their size.", "am_michael"),
                                  3.0, ("Set the volume to fifty percent.", "af_sarah"), 7.0])
        h = Helper("--input-wav", wav, "--output-dir", os.path.join(TMP, "out-ptt"), "--no-wake", log_name="ptt")
        try:
            h.ready()
            # 1. plain push-to-talk: speech starts after 3 s, well inside the 8 s window
            h.send(cmd="listen", id="p1", mode="command", timeout=8)
            f = h.wait("final", id="p1", timeout=20)
            self.assertTrue(text_ok(f["text"], "open the file manager"), f)
            self.assertEqual(f["mode"], "command")
            # 2. finish while the user is still talking: transcribe what was said so far
            h.send(cmd="listen", id="p2")
            m = h.mark()
            h.wait("vad", start=m, speech=True, timeout=10)
            time.sleep(1.4)
            t = time.monotonic()
            h.send(cmd="finish")
            f = h.wait("final", id="p2", timeout=5)
            self.assertLess(time.monotonic() - t, 2.5, "finish must answer at once")
            self.assertTrue(f["text"])
            self.assertLess(len(f["text"].split()), 14, f"finish should cut the utterance: {f['text']!r}")
            # 3. cancel: no transcription at all (wait for the rest of phrase 2 to pass first)
            phrase3 = tl[2][0]
            h.wait("mic", status="ok")
            while time.monotonic() - self.wav_t0(h) < phrase3 - 1.0:
                time.sleep(0.1)
            h.send(cmd="listen", id="p3")
            m = h.mark()
            h.wait("vad", start=m, speech=True, timeout=15)
            h.send(cmd="cancel")
            h.wait("state", start=m, value="idle", timeout=3)
            time.sleep(3.0)
            self.assertEqual(h.named("final", m), [], "cancel must not produce a transcript")
            self.assertEqual(h.named("timeout", m), [])
            # 4. timeout: nobody speaks within 2 s (the session ends with 7 s of silence)
            while time.monotonic() - self.wav_t0(h) < tl[2][1] + 1.0:
                time.sleep(0.1)
            m = h.mark()
            h.send(cmd="listen", id="p4", timeout=2)
            t = time.monotonic()
            h.wait("timeout", start=m, id="p4", timeout=6)
            self.assertLess(time.monotonic() - t, 4.0)
            # 5. finish with no speech at all -> timeout at once
            m = h.mark()
            h.send(cmd="listen", id="p5", timeout=8)
            time.sleep(0.5)
            h.send(cmd="finish")
            h.wait("timeout", start=m, id="p5", timeout=3)
        finally:
            self.assertEqual(h.close(), 0)

    def test_yesno_and_followup(self):
        wav, _ = session("yesno", [2.0, ("Yes, go ahead.", "af_heart"), 3.0, ("Turn on dark mode.", "af_bella"), 4.0])
        h = Helper("--input-wav", wav, "--output-dir", os.path.join(TMP, "out-yesno"), log_name="yesno")
        try:
            h.ready()
            h.send(cmd="listen", id="c1", mode="yesno")
            f = h.wait("final", id="c1", timeout=20)
            self.assertEqual(f["mode"], "yesno")
            self.assertEqual(f["answer"], "yes", f)
            h.send(cmd="listen", id="f1", mode="followup", timeout=5)
            f = h.wait("final", id="f1", timeout=20)
            self.assertEqual(f["mode"], "followup")
            self.assertTrue(text_ok(f["text"], "turn on dark mode"), f)
        finally:
            h.close()

    def test_listen_survives_dead_capture(self):
        """capture that never works: listen still ends (timeout), never a stuck state"""
        h = Helper("--capture-command", "exit 3", log_name="dead")
        try:
            h.ready()
            h.send(cmd="listen", id="d1", timeout=2)
            h.wait("timeout", id="d1", timeout=8)
        finally:
            h.close()


@need_data
class Speech(unittest.TestCase):
    def test_streamed_chunks(self):
        out = os.path.join(TMP, "out-speak")
        h = Helper("--muted", "--output-dir", out, log_name="speak")
        try:
            h.ready()
            h.send(cmd="speak", id="s1", text="Here is the first part.", more=True)
            time.sleep(0.3)
            h.send(cmd="speak", id="s1", text="And here is the second part.", more=True)
            h.send(cmd="speak", id="s1", text="Done.", more=False)
            end = h.wait("speak_end", id="s1", timeout=30)
            self.assertFalse(end["interrupted"])
            self.assertEqual(len(h.named("speak_start", id="s1")), 1)
            self.assertEqual(len(h.named("speak_end", id="s1")), 1)
            sentences = [e["text"] for e in h.named("sentence", id="s1")]
            self.assertEqual(sentences, ["Here is the first part.", "And here is the second part.", "Done."])
            fa = h.wait("first_audio", id="s1")
            self.assertGreater(fa["latency_ms"], 0)
            self.assertTrue(h.named("level", source="tts"))
            wav = os.path.join(out, "speak-s1.wav")
            self.assertGreater(wav_seconds(wav), 2.5)
            # The engine reports speak_end first and only then that it stopped speaking
            # (state back to "muted"): wait for that event instead of racing it.
            h.wait("state", start=end["_index"], value="muted", timeout=10)
            states = [e["value"] for e in h.named("state")]
            self.assertEqual(states[:3], ["muted", "speaking", "muted"])

            # a reply that waits for more text: not "speaking" in the gap, one speak_end at the end
            m = h.mark()
            h.send(cmd="speak", id="s2", text="Let me check.", more=True)
            h.wait("sentence", start=m, id="s2", timeout=20)
            h.wait("state", start=m + 1, value="muted", timeout=10)
            time.sleep(1.0)
            self.assertEqual(h.named("speak_end", m, id="s2"), [])
            h.send(cmd="speak", id="s2", text="It is done.", more=False)
            h.wait("speak_end", start=m, id="s2", timeout=20)
            self.assertEqual(len(h.named("speak_start", m, id="s2")), 1)

            # nothing speakable: still exactly one start and one end, no error
            m = h.mark()
            h.send(cmd="speak", id="e1", text="🙂🙂 ✨", more=False)
            e = h.wait("speak_end", start=m, id="e1", timeout=10)
            self.assertFalse(e["interrupted"])
            self.assertEqual(len(h.named("speak_start", m, id="e1")), 1)
            self.assertEqual(h.named("first_audio", m, id="e1"), [])
            # odd unicode and markdown leftovers are spoken, not crashed on
            h.send(cmd="speak", id="u1", text="Héllo 👋 wörld™ — **½** ± `x`; Ｆｕｌｌ ﬁne.", more=False)
            h.wait("first_audio", start=m, id="u1", timeout=20)
            h.wait("speak_end", start=m, id="u1", timeout=20)
            self.assertEqual(h.named("error", m), [])
        finally:
            self.assertEqual(h.close(), 0)

    def test_stop_and_preview(self):
        out = os.path.join(TMP, "out-stop")
        h = Helper("--muted", "--output-dir", out, log_name="stop")
        try:
            h.ready()
            long_text = ("This is a long reply. It goes on for a while, sentence after sentence, so that there is "
                         "plenty of time to interrupt it. Nobody wants to hear all of it. It keeps going anyway.")
            h.send(cmd="speak", id="long", text=long_text, more=True)
            h.wait("first_audio", id="long", timeout=20)
            time.sleep(0.8)
            t = time.monotonic()
            h.send(cmd="stop")
            e = h.wait("speak_end", id="long", timeout=3)
            self.assertTrue(e["interrupted"])
            self.assertLess(time.monotonic() - t, 1.0, "stop must be immediate")
            after = e["_index"]
            time.sleep(0.5)
            self.assertEqual([x for x in h.events[after + 1:] if x.get("id") == "long"], [])
            self.assertEqual([x for x in h.events[after + 1:] if x["event"] == "level" and x["source"] == "tts"], [])
            # a late chunk of the interrupted reply is dropped, not spoken
            m = h.mark()
            h.send(cmd="speak", id="long", text="Stale words.", more=False)
            time.sleep(1.0)
            self.assertEqual(h.named("speak_start", m, id="long"), [])
            # preview interrupts what is playing and does not change the voice
            h.send(cmd="speak", id="r2", text=long_text, more=False)
            h.wait("first_audio", start=m, id="r2", timeout=20)
            h.send(cmd="preview", voice="bf_emma")
            self.assertTrue(h.wait("speak_end", start=m, id="r2", timeout=3)["interrupted"])
            e = h.wait("speak_end", start=m, id="preview", timeout=30)
            self.assertFalse(e["interrupted"])
            self.assertTrue(os.path.exists(os.path.join(out, "speak-preview.wav")))
            m = h.mark()
            h.send(cmd="preview", voice="xx_nobody")
            h.wait("error", start=m, timeout=5)
            self.assertTrue(h.wait("speak_end", start=m, id="preview", timeout=5)["interrupted"])
        finally:
            self.assertEqual(h.close(), 0)


@need_data
class HalfDuplex(unittest.TestCase):
    def test_own_voice_is_ignored(self):
        """the mic is ignored while speaking and 0.4 s after; afterwards wake works again"""
        wav, tl = session("duplex", [2.0, ("Shadow, open the terminal.", "af_heart"), 7.0,
                                     ("Shadow, close the window.", "am_michael"), 3.0])
        h = Helper("--input-wav", wav, "--output-dir", os.path.join(TMP, "out-duplex"), log_name="duplex")
        try:
            h.ready()
            h.wait("mic", status="ok")
            h.send(cmd="speak", id="talk", text=("I am talking for a few seconds now, so that the microphone has "
                                                 "to be ignored completely while I speak."))
            s = h.wait("speak_start", id="talk", timeout=10)
            e = h.wait("speak_end", id="talk", timeout=30)
            self.assertLess(tl[0][1], e["_wall"] - s["_wall"] + 2.0, "test setup: speech must cover phrase 1")
            during = [x for x in h.events[s["_index"]:e["_index"]] if x["event"] == "level" and x["source"] == "mic"]
            self.assertEqual(during, [], "no mic levels while speaking")
            f = h.wait("final", timeout=20)
            self.assertTrue(text_ok(f["text"], "close the window"), f"own-voice period leaked: {f}")
            self.assertEqual(len(h.named("final")), 1)
        finally:
            h.close()


@need_data
class Microphone(unittest.TestCase):
    def test_mute(self):
        wav, _ = session("mute", [2.0, ("Shadow, open the terminal.", "af_heart"), 6.0])
        h = Helper("--input-wav", wav, "--output-dir", os.path.join(TMP, "out-mute"), log_name="mute")
        try:
            h.ready()
            h.wait("mic", status="ok")
            m = h.mark()
            h.send(cmd="mute", value=True)
            h.wait("state", start=m, value="muted", timeout=3)
            stopped = h.wait("mic", start=m, status="stopped", timeout=3)
            h.send(cmd="listen", id="m1")
            self.assertEqual(h.wait("timeout", start=m, id="m1", timeout=3).get("reason"), "muted")
            time.sleep(3.0)                         # the wake phrase plays while muted
            self.assertEqual(h.named("final", m), [])
            self.assertEqual(h.named("level", stopped["_index"], source="mic"), [], "mic levels after mute")
            m = h.mark()
            h.send(cmd="config", muted=False)
            self.assertEqual(h.wait("config_ok", start=m)["applied"], {"muted": False})
            h.wait("mic", start=m, status="ok", timeout=5)
            h.wait("state", start=m, value="idle", timeout=3)
        finally:
            h.close()

    def test_capture_death_retries(self):
        h = Helper("--capture-command", fake_mic("diesoon", NOISE_THEN_DIE), log_name="die")
        try:
            h.ready()
            h.wait("mic", status="ok", timeout=10)
            e = h.wait("mic", status="error", timeout=10)
            self.assertIn("exited with code 1", e["message"])
            self.assertIn("device unplugged", e["message"])
            self.assertIn("Retrying in", e["message"])
            m = h.mark()
            h.wait("mic", start=m, status="ok", timeout=10)            # retried and working again
            e2 = h.wait("mic", start=m, status="error", timeout=10)
            self.assertIn("Retrying in", e2["message"])
        finally:
            self.assertEqual(h.close(), 0)

    def test_capture_never_starts_backoff(self):
        h = Helper("--capture-command", "echo 'no such device' >&2; exit 3", log_name="nostart")
        try:
            h.ready()
            e1 = h.wait("mic", status="error", timeout=10)
            self.assertIn("exited with code 3", e1["message"])
            self.assertIn("no such device", e1["message"])
            e2 = h.wait("mic", start=e1["_index"] + 1, status="error", timeout=10)
            e3 = h.wait("mic", start=e2["_index"] + 1, status="error", timeout=10)
            waits = [int(re.search(r"Retrying in (\d+) s", e["message"]).group(1)) for e in (e1, e2, e3)]
            self.assertEqual(waits, [1, 2, 4])
            self.assertGreater(e3["_wall"] - e2["_wall"], 1.5)
        finally:
            self.assertEqual(h.close(), 0)

    def test_silent_microphone(self):
        h = Helper("--capture-command", fake_mic("silent", SILENCE_THEN_NOISE), log_name="silent")
        try:
            h.ready()
            ok = h.wait("mic", status="ok", timeout=10)
            e = h.wait("mic", status="error", timeout=20)
            self.assertEqual(e["message"], "The microphone is silent (muted, or the wrong input is selected)")
            self.assertGreater(e["_wall"] - ok["_wall"], 9.0)
            self.assertEqual(len(h.named("mic", status="error")), 1, "reported once, not every frame")
            back = h.wait("mic", start=e["_index"] + 1, status="ok", timeout=10)
            self.assertIn("back", back["message"])
        finally:
            h.close()


@need_data
class Lifecycle(unittest.TestCase):
    def _start_with_fake_mic(self, marker):
        h = Helper("--capture-command", fake_mic(marker, QUIET_MIC), log_name=marker)
        h.ready()
        h.wait("mic", status="ok", timeout=10)
        self.assertTrue(processes_with(marker, exclude=(h.p.pid,)), "fake capture child should be running")
        return h

    def _assert_child_gone(self, marker):
        end = time.monotonic() + 3
        while processes_with(marker) and time.monotonic() < end:
            time.sleep(0.1)
        self.assertEqual(processes_with(marker), [], "capture child left behind")

    def test_stdin_eof(self):
        h = self._start_with_fake_mic("eofmic")
        t = time.monotonic()
        h.p.stdin.close()
        self.assertEqual(h.p.wait(10), 0)
        self.assertLess(time.monotonic() - t, 5)
        self._assert_child_gone("eofmic")
        h.close()

    def test_sigterm(self):
        h = self._start_with_fake_mic("termmic")
        h.kill(signal.SIGTERM)
        self.assertEqual(h.p.wait(10), 0)
        self._assert_child_gone("termmic")
        h.close()

    def test_sigkill_leaves_no_orphans(self):
        h = self._start_with_fake_mic("killmic")
        h.kill(signal.SIGKILL)
        h.p.wait(10)
        self._assert_child_gone("killmic")
        h.close()

    def test_quit_and_ping(self):
        h = Helper("--muted", log_name="quit")
        h.send(cmd="ping")
        h.wait("pong", timeout=10)                       # answered even while models load
        hello = h.wait("hello")
        self.assertEqual(hello["_index"], 0)
        self.assertIn("pid", hello)
        r = h.ready()
        for k in ("load_s", "stt", "tts", "voice", "voices", "kws", "capture", "playback", "data"):
            self.assertIn(k, r)
        self.assertIn("af_heart", r["voices"])
        h.send(cmd="quit")
        self.assertEqual(h.p.wait(10), 0)
        h.close()

    def test_commands_before_ready_keep_their_order(self):
        """sent while the models load: queued, run in order before the microphone opens"""
        h = Helper("--capture-command", fake_mic("early", QUIET_MIC), log_name="early")
        try:
            h.send(cmd="config", muted=True)
            h.send(cmd="listen", id="e1")
            h.send(cmd="config", voice="bf_emma")
            r = h.ready()
            c1 = h.wait("config_ok", start=r["_index"])
            self.assertEqual(c1["applied"], {"muted": True})
            t = h.wait("timeout", start=c1["_index"], id="e1")
            self.assertEqual(t.get("reason"), "muted")
            c2 = h.wait("config_ok", start=t["_index"])
            self.assertEqual(c2["applied"], {"voice": "bf_emma"})
            time.sleep(1.5)
            self.assertEqual(h.named("mic", status="ok"), [], "the microphone must stay closed when muted early")
            self.assertEqual(processes_with("early", exclude=(h.p.pid,)), [])
        finally:
            self.assertEqual(h.close(), 0)

    def test_bad_commands(self):
        h = Helper("--muted", log_name="bad")
        try:
            h.ready()
            h.p.stdin.write(b"this is not json\n{\"no\":\"cmd\"}\n[1,2]\n")
            h.p.stdin.flush()
            h.send(cmd="frobnicate")
            h.send(cmd="ping")
            h.wait("pong")
            errors = h.named("error")
            self.assertEqual(len(errors), 4, errors)
            self.assertTrue(all(e["fatal"] is False for e in errors))
            self.assertIn("frobnicate", errors[-1]["message"])
        finally:
            self.assertEqual(h.close(), 0)


@need_data
class Config(unittest.TestCase):
    def test_config_validation(self):
        h = Helper("--muted", log_name="config")
        try:
            h.ready()
            h.send(cmd="config", voice="af_bella", speed=2.0, wake_sensitivity="high", wake=True, bogus=1,
                   input_device="", follow_up_secs=4)
            c = h.wait("config_ok")
            self.assertEqual(c["applied"], {"voice": "af_bella", "speed": 1.4, "wake_sensitivity": "high",
                                            "wake": True, "input_device": "", "follow_up_secs": 4.0})
            self.assertEqual(c["ignored"], ["bogus"])
            m = h.mark()
            h.send(cmd="config", voice="zz_nope", speed="fast", stt="klingon")
            c = h.wait("config_ok", start=m)
            self.assertEqual(c["applied"], {})
            self.assertEqual(set(c["rejected"]), {"voice", "speed", "stt"})
            self.assertEqual(len(h.named("error", m)), 3)
        finally:
            h.close()

    def test_wake_off_closes_the_mic(self):
        h = Helper("--capture-command", fake_mic("wakeoff", QUIET_MIC), log_name="wakeoff")
        try:
            h.ready()
            h.wait("mic", status="ok", timeout=10)
            m = h.mark()
            h.send(cmd="config", wake=False)
            h.wait("mic", start=m, status="stopped", timeout=6)
            time.sleep(0.5)
            self.assertEqual(processes_with("wakeoff", exclude=(h.p.pid,)), [],
                             "the capture tool must not run without a reason")
            m = h.mark()
            h.send(cmd="listen", id="w1", timeout=2)       # push-to-talk opens the mic again
            h.wait("mic", start=m, status="ok", timeout=6)
            h.wait("timeout", start=m, id="w1", timeout=8)
        finally:
            h.close()

    def test_switch_stt_model(self):
        probe = subprocess.run([VOICED, "--probe", "--data", DATA], capture_output=True, text=True)
        if "whisper" not in probe.stdout:
            self.skipTest("whisper not installed")
        wav, _ = session("sttswitch", [6.0, ("Open the settings.", "af_heart"), 4.0])
        h = Helper("--input-wav", wav, "--output-dir", os.path.join(TMP, "out-stt"), "--no-wake", log_name="stt")
        try:
            h.ready()
            h.send(cmd="config", stt="whisper")
            self.assertEqual(h.wait("config_ok")["applied"], {"stt": "whisper"})
            h.send(cmd="listen", id="s1", timeout=12)
            f = h.wait("final", id="s1", timeout=25)
            self.assertEqual(f["stt"], "whisper")
            self.assertTrue(text_ok(f["text"], "open the settings"), f)
        finally:
            h.close()


@need_data
class Cli(unittest.TestCase):
    def run_json(self, *args, env=None):
        import json
        r = subprocess.run([VOICED, "--no-log-file"] + list(args), capture_output=True, text=True, timeout=180,
                           env=env)
        return r.returncode, json.loads(r.stdout.strip().splitlines()[-1]) if r.stdout.strip() else {}

    def test_probe_exit_codes(self):
        t = time.monotonic()
        code, out = self.run_json("--probe", "--data", DATA)
        self.assertLess(time.monotonic() - t, 3.0)
        self.assertEqual(code, 0, out)
        self.assertTrue(out["ok"])
        for k in ("data", "python", "missing", "stt", "tts", "kws", "capture", "playback", "reason"):
            self.assertIn(k, out)
        with tempfile.TemporaryDirectory() as empty:
            code, out = self.run_json("--probe", "--data", empty)
            self.assertEqual(code, 3, out)
            self.assertFalse(out["ok"])
            self.assertIn("shadow-voice-setup", out["reason"])
            # data present, but the Python runtime is not usable
            import json
            files = {"vad": ["models/vad/silero_vad.onnx"],
                     "stt-moonshine": ["models/stt/moonshine/encoder_model.ort",
                                       "models/stt/moonshine/decoder_model_merged.ort", "models/stt/moonshine/tokens.txt"],
                     "tts-kokoro": ["models/tts/kokoro/" + n for n in ("model.onnx", "voices.bin", "tokens.txt",
                                                                       "lexicon-us-en.txt", "lexicon-gb-en.txt")],
                     "runtime": []}
            for rels in files.values():
                for rel in rels:
                    os.makedirs(os.path.dirname(os.path.join(empty, rel)), exist_ok=True)
                    open(os.path.join(empty, rel), "w").close()
            os.makedirs(os.path.join(empty, "runtime"), exist_ok=True)
            with open(os.path.join(empty, "manifest.json"), "w") as f:
                json.dump({"format": 1, "installed": {c: {"files": {}} for c in files}}, f)
            code, out = self.run_json("--probe", "--data", empty)
            self.assertEqual(code, 4, out)
            self.assertTrue(out["missing"])
        with tempfile.TemporaryDirectory() as nobin:
            env = dict(os.environ, PATH=nobin)
            code, out = self.run_json("--probe", "--data", DATA, env=env)
            self.assertEqual(code, 5, out)
            self.assertIsNone(out["capture"])

    def test_transcribe_say_calibrate(self):
        wav, _ = session("cli", [0.5, ("Shadow, open the terminal.", "af_heart"), 0.8])
        code, out = self.run_json("--transcribe", wav, "--data", DATA)
        self.assertEqual(code, 0, out)
        self.assertTrue(out["wake"])
        self.assertTrue(text_ok(out["command"], "open the terminal"), out)
        target = os.path.join(TMP, "say.wav")
        code, out = self.run_json("--say", "Your disk is 82% full.", "--out", target, "--voice", "bf_emma", "--data", DATA)
        self.assertEqual(code, 0, out)
        self.assertGreater(wav_seconds(target), 1.0)
        self.assertEqual(out["voice"], "bf_emma")
        code, out = self.run_json("--calibrate", "5", "--expect", "Shadow, open the terminal", "--input-wav", wav,
                                  "--data", DATA)
        self.assertEqual(code, 0, out)
        self.assertIn(out["recommended"], ("moonshine", "whisper"))
        self.assertIn("moonshine", out["models"])
        for k in ("peak_dbfs", "rms_dbfs", "clipping", "silence", "noise_floor_dbfs"):
            self.assertIn(k, out["mic"])
        self.assertFalse(out["mic"]["silence"])
        self.assertEqual(out["models"][out["recommended"]]["wer"], 0.0, out)
        # the wake word went through the real listener (VAD + keyword spotter + policy)
        self.assertEqual(out["command"], "open the terminal")
        self.assertTrue(out["wake"]["ok"], out["wake"])
        self.assertEqual(out["wake"]["command"], "open the terminal", out["wake"])
        self.assertFalse(out["wake"]["needs_high"], out["wake"])
        # a silent recording: no recommendation, and advice that names the problem
        silent = os.path.join(TMP, "silent.wav")
        import wave
        with wave.open(silent, "wb") as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000); w.writeframes(bytes(16000 * 2 * 3))
        code, out = self.run_json("--calibrate", "3", "--expect", "hello", "--input-wav", silent, "--data", DATA)
        self.assertEqual(code, 1, out)
        self.assertIsNone(out["recommended"])
        self.assertTrue(out["mic"]["silence"])
        self.assertTrue(any("silence" in a for a in out["advice"]))

    def test_calibrate_wake_word_misheard(self):
        """A first word that is not "Shadow" is the wake check's failure, not a word error:
        the command words are still scored right, and wake.ok is false at every sensitivity."""
        wav, _ = session("cal-meadow", [0.6, ("Meadow, open the terminal.", "af_heart"), 1.0])
        code, out = self.run_json("--calibrate", "5", "--expect", "Shadow, open the terminal", "--input-wav", wav,
                                  "--data", DATA)
        self.assertEqual(code, 0, out)
        self.assertEqual(out["models"][out["recommended"]]["wer"], 0.0, out)
        self.assertFalse(out["wake"]["ok"], out["wake"])
        self.assertFalse(out["wake"]["needs_high"], out["wake"])
        self.assertIn("eadow", out["wake"]["heard"], out["wake"])


if __name__ == "__main__":
    unittest.main()
