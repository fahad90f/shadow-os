#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Headless tests of the assistant's voice (VoiceController v2, voice spec §2.3–§4).

Every test runs the REAL shadow-assistant binary in one of its headless modes —
--voice-session (the full app-side voice loop: voice controller + a real
Assistant against tests/mock_api.py), --dump, --voice-selftest,
--voice-speech-text — with tests/fake_voiced.py standing in for the voice
engine helper (deterministic, no models, no audio devices) and a stub eSpeak NG
that only records what it was asked to say. Nothing reaches the network or a
sound card; no real API key is used (sk-ant-test).

    voice_tests.py [--bin PATH] [--jobs N] [--keep] [--verbose] [--smoke] [TEST...]

--bin defaults to $SHADOW_ASSISTANT_BIN, then <build>/bin/shadow-assistant next
to the <build>/lib of $LD_LIBRARY_PATH (how scripts/dev.sh run sets things up),
then shadow-assistant on PATH. --smoke prints SHADOW-SMOKE-OK on success, so
the suite can run from applications/assistant/smoke-tests.list via
tests/desktop/smoke.sh. Exit status: 0 all passed, 1 a test failed, 2 setup error.
"""
import argparse
import concurrent.futures
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import traceback
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
FAKE = os.path.join(HERE, "fake_voiced.py")
MOCK = os.path.join(HERE, "mock_api.py")
NO_HELPER = "/nonexistent/shadow-voiced"

# The configuration template that shadow-assistant 343e765 showed on its setup
# page (and that users pasted): comments AFTER values, which QSettings reads as
# part of the value. Only the key is a dummy.
OLD_TEMPLATE = """\
# ~/.config/shadow/assistant.conf  —  create with mode 0600 (chmod 600)
# SHADOW Assistant reads your Claude (Anthropic) API key from here (or
# from the SHADOW_ASSISTANT_API_KEY or ANTHROPIC_API_KEY environment
# variable). The key is never stored in the application or the repository.
# Get a key at https://console.anthropic.com/ (keys start with sk-ant-).

[api]
key=sk-ant-test
# base_url=https://api.anthropic.com/v1   # usually leave unset
# anthropic_version=2023-06-01            # Messages API version header
# Offline: run a model on this computer instead of the cloud (no key
# needed). Easiest setup:  shadow-assistant-local setup
# local=true
# local_base_url=http://127.0.0.1:11434/v1

[models]
# One Claude model serves every role by default (claude-sonnet-4-6,
# run at high effort with adaptive thinking). Change any role to a
# different Claude model ID if you prefer (e.g. claude-opus-5-5 for the
# brain, claude-haiku-5-5 for fast low-cost replies).
brain=claude-sonnet-4-6
planner=claude-sonnet-4-6
fast=claude-sonnet-4-6
omni=claude-sonnet-4-6
# local=llama3.1:8b   # model used when [api] local=true

[behaviour]
# autonomy = autopilot (run everything without asking; the default) |
#            ask_risky (ask before changing the system) | ask_every (ask every time)
autonomy=autopilot

[tools]
# disabled = comma-separated tool names to turn off, e.g. run_shell_command
disabled=

[voice]
# Hands-free voice assistant "Shadow". Always-listening is OPT-IN.
wake_word_enabled=false       # listen continuously for the wake word
wake_phrase=shadow            # keyphrase that wakes the assistant
wake_threshold=1e-20          # pocketsphinx kws threshold (smaller = stricter)
muted=false                   # privacy mic mute (nothing is captured when true)
speak_replies=true            # speak answers aloud
tts_engine=auto               # auto | piper | speechd | espeak
tts_voice=                    # speechd/espeak voice name (e.g. en-us)
piper_bin=piper               # piper executable (neural TTS, if installed)
piper_model=                  # path to a piper .onnx voice model (required for piper)
stt_mode=auto                 # voice is transcribed offline with pocketsphinx (Claude has no speech-to-text)
capture_tool=auto             # auto | parecord | pw-record | arecord
input_device=                 # optional capture device (PulseAudio/PipeWire source)
max_utterance_secs=12         # stop capturing a command after this long
pocketsphinx_model=           # optional path to an acoustic model directory
"""

# The same from the old README (commas inside the comments: QSettings even
# turned those values into lists).
OLD_README = """\
[api]
key=sk-ant-test
# base_url=https://api.anthropic.com/v1   # usually leave unset
# anthropic_version=2023-06-01            # Messages API version header

[models]
brain=claude-sonnet-4-6    # chat, tools, vision — the default
planner=claude-sonnet-4-6  # "Think hard" deep reasoning
fast=claude-sonnet-4-6     # quick background helpers
omni=claude-sonnet-4-6     # voice & image

[behaviour]
autonomy=autopilot        # autopilot (default) | ask_risky | ask_every

[tools]
disabled=                 # comma-separated tool names to turn off

[voice]
wake_word_enabled=true    # listen for the wake phrase (on; the toggle turns it off)
wake_phrase=shadow        # word that wakes the assistant
wake_threshold=1e-20      # keyphrase-spotting threshold, used only with pocketsphinx 5
muted=false               # privacy mic mute — nothing is captured when true
speak_replies=true        # speak answers aloud
tts_engine=auto           # auto | piper | speechd | espeak
tts_voice=                # speechd/espeak voice name (e.g. en-us)
piper_bin=piper           # piper executable (neural TTS, if installed)
piper_model=              # path to a piper .onnx voice model (required for piper)
stt_mode=auto             # voice is transcribed offline (Claude has no speech-to-text)
capture_tool=auto         # auto | parecord | pw-record | arecord
input_device=             # optional capture device (PulseAudio/PipeWire source)
max_utterance_secs=12     # give up waiting for a command after this long
pocketsphinx_model=       # optional path to an acoustic model directory
"""

MARKDOWN_SAMPLE = """\
## Steps

Hello! I am **Shadow Assistant**, e.g. your helper. See [the docs](https://example.com/docs) or https://example.org/x.

- First thing
- Second *thing*
1. Run `ls -la` now

```bash
rm -rf /tmp/never-spoken
```

| Name | Size |
|------|------|
| secret-cell | 42 |

> Quoted words here.
Done
"""


class Failure(AssertionError):
    pass


def check(cond, msg):
    if not cond:
        raise Failure(msg)


# ----------------------------------------------------------------------------- infrastructure
def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Mock:
    """tests/mock_api.py in one mode, on a free port."""

    def __init__(self, mode, workdir):
        self.port = free_port()
        self.log = open(os.path.join(workdir, f"mock-{mode}.log"), "w")
        self.proc = subprocess.Popen([sys.executable, "-I", MOCK, "--port", str(self.port), "--mode", mode],
                                     stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.time() + 10
        while time.time() < deadline:
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=0.2).close()
                return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError(f"mock_api ({mode}) did not start")

    @property
    def url(self):
        return f"http://127.0.0.1:{self.port}/v1"

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
        self.log.close()


class ScriptedMock:
    """An Anthropic Messages API mock in this process, for replies mock_api.py
    has no mode for: each request gets the next reply of a script, then
    `default`. A reply is {"text": str, "delay": s/token} (streamed text),
    {"tools": [{"name", "input"}]} (tool_use blocks), or {"text": ...,
    "sse_error": message} (the stream breaks off with an SSE error event and
    then ends normally, like an overloaded_error mid-answer)."""

    def __init__(self, replies, default=None):
        self.replies = list(replies)
        self.default = default or {"text": "All done."}
        self.requests = []
        self.bodies = []   # each request's "messages" array, as sent
        self.lock = threading.Lock()
        mock = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *a):
                pass

            def ev(self, etype, obj):
                data = ("event: " + etype + "\n" + "data: " + json.dumps({"type": etype, **obj}) + "\n\n").encode()
                self.wfile.write(("%x\r\n" % len(data)).encode() + data + b"\r\n")
                self.wfile.flush()

            def do_POST(self):
                n = int(self.headers.get("Content-Length", 0))
                try:
                    req = json.loads(self.rfile.read(n) or b"{}")
                except ValueError:
                    req = {}
                results = sum(1 for m in req.get("messages", []) if isinstance(m.get("content"), list)
                              for b in m["content"] if isinstance(b, dict) and b.get("type") == "tool_result")
                with mock.lock:
                    reply = mock.replies.pop(0) if mock.replies else dict(mock.default)
                    mock.requests.append({"messages": len(req.get("messages", [])), "tool_results": results})
                    mock.bodies.append(req.get("messages", []))
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Transfer-Encoding", "chunked")
                self.end_headers()
                try:
                    self.ev("message_start", {"message": {"id": "msg_scripted", "type": "message", "role": "assistant",
                                                          "model": req.get("model", "mock"), "content": []}})
                    idx = 0
                    text = reply.get("text", "")
                    if text:
                        self.ev("content_block_start", {"index": idx, "content_block": {"type": "text", "text": ""}})
                        for tok in [t for t in text.replace("\n", " \n ").split(" ") if t]:
                            self.ev("content_block_delta", {"index": idx, "delta": {"type": "text_delta",
                                                                                    "text": tok + " "}})
                            time.sleep(float(reply.get("delay", 0.02)))
                        self.ev("content_block_stop", {"index": idx})
                        idx += 1
                    if reply.get("sse_error"):
                        self.ev("error", {"error": {"type": "overloaded_error", "message": reply["sse_error"]}})
                    for k, call in enumerate(reply.get("tools", [])):
                        self.ev("content_block_start", {"index": idx, "content_block": {
                            "type": "tool_use", "id": "toolu_scripted_%d_%d" % (len(mock.requests), k),
                            "name": call["name"], "input": {}}})
                        self.ev("content_block_delta", {"index": idx, "delta": {
                            "type": "input_json_delta", "partial_json": json.dumps(call.get("input", {}))}})
                        self.ev("content_block_stop", {"index": idx})
                        idx += 1
                    self.ev("message_delta", {"delta": {"stop_reason": "tool_use" if reply.get("tools") else "end_turn"}})
                    self.ev("message_stop", {})
                    self.wfile.write(b"0\r\n\r\n")
                    self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError):
                    pass

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.server.daemon_threads = True
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    @property
    def url(self):
        return f"http://127.0.0.1:{self.server.server_address[1]}/v1"

    def stop(self):
        self.server.shutdown()
        self.server.server_close()


class Suite:
    def __init__(self, binary, workdir, verbose):
        self.bin = binary
        self.work = workdir
        self.verbose = verbose
        self.mocks = {}
        # Run the fake through python3 explicitly: works even without the
        # executable bit (a copied tree), and needs nothing but python3.
        self.fake = os.path.join(workdir, "fake-voiced")
        with open(self.fake, "w") as f:
            f.write(f'#!/bin/sh\nexec {sys.executable} -I "{FAKE}" "$@"\n')
        os.chmod(self.fake, 0o755)

    def mock(self, mode):
        if mode not in self.mocks:
            self.mocks[mode] = Mock(mode, self.work)
        return self.mocks[mode]

    def stop(self):
        for m in self.mocks.values():
            m.stop()


class Run:
    """One isolated app run: own HOME, runtime dir, stub PATH and fake helper state."""

    def __init__(self, suite, name):
        self.suite = suite
        self.dir = tempfile.mkdtemp(prefix=name + "-", dir=suite.work)
        self.home = os.path.join(self.dir, "home")
        os.makedirs(os.path.join(self.home, ".config", "shadow"))
        self.rt = os.path.join(self.dir, "run")
        os.makedirs(self.rt, mode=0o700)
        self.bindir = os.path.join(self.dir, "bin")
        os.makedirs(self.bindir)
        self.espeak_log = os.path.join(self.dir, "espeak.log")
        self.cmd_log = os.path.join(self.dir, "helper-commands.log")
        self.state = os.path.join(self.dir, "fake-voiced.count")
        self.out = os.path.join(self.dir, "out")
        # eSpeak NG stub: records its arguments, never makes a sound.
        stub = os.path.join(self.bindir, "espeak-ng")
        with open(stub, "w") as f:
            f.write(f"#!{sys.executable} -I\nimport json, sys, time\n"
                    f"open({self.espeak_log!r}, 'a').write(json.dumps(sys.argv[1:]) + '\\n')\n"
                    "time.sleep(0.05)\n")
        os.chmod(stub, 0o755)
        self.conf_path = os.path.join(self.home, ".config", "shadow", "assistant.conf")

    def conf(self, text):
        with open(self.conf_path, "w") as f:
            f.write(text)

    def env(self, helper=None, espeak=True, api="stream", extra=None):
        path = (self.bindir + ":" if espeak else "") + "/usr/local/bin:/usr/bin:/bin"
        if not espeak:
            # No eSpeak NG at all: only what the app itself needs (nothing).
            empty = os.path.join(self.dir, "empty-bin")
            os.makedirs(empty, exist_ok=True)
            path = empty
        e = {
            "PATH": path, "HOME": self.home, "XDG_RUNTIME_DIR": self.rt, "LANG": "C.UTF-8",
            "SHADOW_VOICED": helper if helper is not None else self.suite.fake,
            "SHADOW_ASSISTANT_API_KEY": "sk-ant-test",
            "FAKE_VOICED_LOG": self.cmd_log, "FAKE_VOICED_STATE": self.state,
        }
        if api:
            e["SHADOW_ASSISTANT_BASE_URL"] = self.suite.mock(api).url
        build = os.path.dirname(os.path.dirname(self.suite.bin))
        if os.path.isdir(os.path.join(build, "lib")):
            e["LD_LIBRARY_PATH"] = os.path.join(build, "lib")
        if os.path.isdir(os.path.join(build, "qml")):
            e["QML_IMPORT_PATH"] = os.path.join(build, "qml")
        e.update(extra or {})
        return e

    def run(self, args, env, timeout=60):
        p = subprocess.run([self.suite.bin] + args, env=env, capture_output=True, text=True, timeout=timeout)
        with open(os.path.join(self.dir, "stdout.log"), "w") as f:
            f.write(p.stdout)
        with open(os.path.join(self.dir, "stderr.log"), "w") as f:
            f.write(p.stderr)
        return p

    def session(self, scenario=None, actions="", timeout=30, **envargs):
        scn = os.path.join(self.dir, "scenario.json")
        with open(scn, "w") as f:
            json.dump(scenario or {}, f)
        args = ["--voice-session", scn, "--voice-out", self.out, "--voice-timeout", str(timeout)]
        if actions:
            args += ["--voice-actions", actions]
        p = self.run(args, self.env(**envargs), timeout=timeout + 30)
        events = []
        for line in p.stdout.splitlines():
            line = line.strip()
            if line.startswith("{"):
                events.append(json.loads(line))
        return Session(p.returncode, events, p.stderr, self)

    def espeak_calls(self):
        if not os.path.exists(self.espeak_log):
            return []
        with open(self.espeak_log) as f:
            return [json.loads(line) for line in f if line.strip()]

    def helper_commands(self):
        if not os.path.exists(self.cmd_log):
            return []
        with open(self.cmd_log) as f:
            return [json.loads(line) for line in f if line.strip()]


class Session:
    def __init__(self, rc, events, stderr, run):
        self.rc, self.events, self.stderr, self.run = rc, events, stderr, run

    def all(self, event, /, **match):
        return [e for e in self.events if e.get("event") == event
                and all(e.get(k) == v for k, v in match.items())]

    def first(self, event, /, **match):
        found = self.all(event, **match)
        check(found, f"no '{event}' event matching {match}")
        return found[0]

    def index(self, event):
        return self.events.index(event)

    @property
    def done(self):
        return self.first("done")

    def tail(self, n=40):
        return "\n".join(json.dumps(e, ensure_ascii=False) for e in self.events[-n:])


TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn


# ----------------------------------------------------------------------------- text for speech
@test
def speech_text(suite):
    """Markdown is spoken as plain sentences; streamed in chunks the result is the same."""
    r = Run(suite, "speech_text")
    md = os.path.join(r.dir, "reply.md")
    with open(md, "w") as f:
        f.write(MARKDOWN_SAMPLE)
    p = r.run(["--voice-speech-text", md], r.env(api=None))
    check(p.returncode == 0, f"exit {p.returncode}: {p.stderr}")
    out = json.loads(p.stdout)
    s = out["sentences"]
    speech = out["speech"]
    check(s[0] == "Steps.", f"heading is a sentence of its own: {s[:2]}")
    check("Hello!" in s, f"first sentence alone: {s}")
    check(any("Shadow Assistant, e.g. your helper." in x for x in s), f"no split after e.g., bold removed: {s}")
    check("the docs" in speech and "example.com" not in speech, "link text kept, URL dropped")
    check("a link" in speech, "bare URL spoken as 'a link'")
    check("First thing." in s and "Second thing." in s, f"list items are sentences: {s}")
    check("Run ls -la now." in s, f"inline code kept as words: {s}")
    check(speech.count("(code omitted)") == 1 and "never-spoken" not in speech, "code block omitted")
    check("secret-cell" not in speech and "|" not in speech, "table skipped")
    check("Quoted words here." in speech, "quote text kept")
    for ch in "*#`_[]":
        check(ch not in speech, f"markup {ch!r} left in {speech!r}")
    check(out["streamed"] == s, f"streamed differs from whole:\n{out['streamed']}\nvs\n{s}")
    check(out["steps"] and out["steps"][0]["sentences"][0] == "Steps.", "first sentence ready early")


# ----------------------------------------------------------------------------- configuration
def dump_config(r):
    p = r.run(["--dump", "config"], r.env(api=None, extra={"SHADOW_ASSISTANT_API_KEY": ""}))
    check(p.returncode == 0, f"--dump config exit {p.returncode}: {p.stderr}")
    check("sk-ant-test" not in p.stdout, "the key itself must never be printed")
    return json.loads(p.stdout)


@test
def config_old_template(suite):
    """The old setup-page template (comments after values) reads correctly: muted is false."""
    r = Run(suite, "config_template")
    r.conf(OLD_TEMPLATE)
    c = dump_config(r)
    v = c["voice"]
    check(v["micMuted"] is False, f"muted=false  # … must read false: {v}")
    check(v["wakeEnabled"] is True, f"wake word on by default: {v}")
    check(v["speakReplies"] is True and v["ttsEngine"] == "auto", f"{v}")
    check(v["inputDevice"] == "" and v["voice"] == "af_heart" and v["sttModel"] == "auto", f"{v}")
    check(c["apiKeyConfigured"] is True and c["apiKeySource"].endswith("assistant.conf"), f"{c}")
    check(c["models"]["brain"] == "claude-sonnet-4-6", f"{c['models']}")
    check(c["autonomy"] == "autopilot" and c["disabledTools"] == [], f"{c}")


@test
def config_old_readme(suite):
    """The old README example (commas in comments → QSettings lists) reads correctly."""
    r = Run(suite, "config_readme")
    r.conf(OLD_README)
    c = dump_config(r)
    for role in ("brain", "planner", "fast", "omni"):
        check(c["models"][role] == "claude-sonnet-4-6", f"{role}: {c['models']}")
    check(c["autonomy"] == "autopilot", f"{c['autonomy']}")
    check(c["disabledTools"] == [], f"{c['disabledTools']}")
    v = c["voice"]
    check(v["micMuted"] is False and v["wakeEnabled"] is True and v["ttsEngine"] == "auto", f"{v}")


@test
def config_voice_keys(suite):
    """New [voice] keys: inline comments stripped, values validated, old engine names mapped."""
    r = Run(suite, "config_keys")
    r.conf("[voice]\nwake_word_chosen=true\nwake_word_enabled=false   ; off\nmuted=true  # privacy\n"
           "voice=bf_emma   # British\nspeech_rate=1.9 ; too fast\nstt_model=Whisper  # case\n"
           "wake_sensitivity=high\nfollow_up=no\ninput_device=alsa_input.usb-mic  # USB\n"
           "tts_engine=piper   # old value\n[tools]\ndisabled=run_shell_command, write_file   # two\n")
    c = dump_config(r)
    v = c["voice"]
    check(v["wakeEnabled"] is False and v["micMuted"] is True, f"{v}")
    check(v["voice"] == "bf_emma" and v["speechRate"] == 1.4 and v["sttModel"] == "whisper", f"{v}")
    check(v["wakeSensitivity"] == "high" and v["followUp"] is False, f"{v}")
    check(v["inputDevice"] == "alsa_input.usb-mic" and v["ttsEngine"] == "auto", f"{v}")
    check(c["disabledTools"] == ["run_shell_command", "write_file"], f"{c['disabledTools']}")


@test
def config_new_template(suite):
    """The template the setup page offers: no comment on a value line, reads as the defaults."""
    r = Run(suite, "config_new")
    template = dump_config(r)["configTemplate"]
    for line in template.splitlines():
        check(not line.startswith("#"), f"'#' is no comment for QSettings: {line!r}")
        if "=" in line and not line.startswith(";"):
            check("#" not in line and ";" not in line, f"comment on a value line: {line!r}")
    r.conf(template.replace("sk-ant-XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX", "sk-ant-test"))
    c = dump_config(r)
    v = c["voice"]
    check(c["apiKeyConfigured"] and c["autonomy"] == "autopilot" and c["disabledTools"] == [], f"{c}")
    check(v == {"wakeEnabled": True, "wakePhrase": "shadow", "micMuted": False, "speakReplies": True,
                "voice": "af_heart", "speechRate": 1, "sttModel": "auto", "wakeSensitivity": "normal",
                "followUp": True, "inputDevice": "", "ttsEngine": "auto"}, f"{v}")


@test
def config_heals_on_save(suite):
    """Changing a voice setting rewrites the old template's values clean (no comment in a value)."""
    r = Run(suite, "config_heal")
    r.conf(OLD_TEMPLATE)
    s = r.session(actions="0.2:mute;0.5:unmute")
    check(s.rc == 0, f"rc {s.rc}")
    with open(r.conf_path) as f:
        text = f.read()
    voice = text[text.index("[voice]"):]
    for line in ("muted=false", "speak_replies=true", "tts_engine=auto", "voice=af_heart", "follow_up=true"):
        check(f"\n{line}\n" in voice, f"{line!r} not written clean:\n{voice}")
    check("%23" not in text, f"'#' lines of the old template were kept as keys:\n{text}")
    for dead in ("piper_bin", "wake_threshold", "pocketsphinx_model", "stt_mode"):
        check(f"\n{dead}=" not in voice, f"dead key {dead} kept")
    check("key=sk-ant-test" in text, "the user's key line is kept as it was")
    c = dump_config(r)
    check(c["voice"]["micMuted"] is False and c["voice"]["wakeEnabled"] is True, f"{c['voice']}")


# ----------------------------------------------------------------------------- diagnostics
@test
def dump_voice_probe_only(suite):
    """--dump voice asks the helper's --probe only: the engine (and microphone) never start."""
    r = Run(suite, "dump_voice")
    p = r.run(["--dump", "voice"], r.env(api=None))
    check(p.returncode == 0, f"exit {p.returncode}: {p.stderr}")
    d = json.loads(p.stdout)
    check(d["probeOnly"] is True and d["engine"] == "neural" and d["probeExit"] == 0, f"{d}")
    check(d["probe"].get("capture") == "pw-record", f"{d['probe']}")
    check(not os.path.exists(r.state), "--serve was started by --dump voice")
    check(r.helper_commands() == [], "commands were sent to a helper")


@test
def selftest(suite):
    """--voice-selftest runs the helper's --selftest and passes its JSON and exit status on."""
    r = Run(suite, "selftest")
    p = r.run(["--voice-selftest"], r.env(api=None))
    check(p.returncode == 0 and json.loads(p.stdout.strip().splitlines()[-1])["ok"] is True,
          f"ok: exit {p.returncode} {p.stdout}")
    p = r.run(["--voice-selftest"], r.env(api=None, extra={"FAKE_VOICED_SELFTEST": "fail"}))
    check(p.returncode == 1, f"failing selftest exit {p.returncode}")
    p = r.run(["--voice-selftest"], r.env(api=None, helper=NO_HELPER))
    check(p.returncode == 3 and json.loads(p.stdout)["ok"] is False, f"missing helper: {p.returncode} {p.stdout}")


# ----------------------------------------------------------------------------- the voice loop
@test
def wake_reply_followup(suite):
    """wake → final → sent → reply streamed to speech sentence by sentence → speak_end → follow-up."""
    r = Run(suite, "wake_reply")
    s = r.session({"utterances": [{"at": 0.5, "text": "what can you do", "heard": "Shadow, what can you do"}]})
    check(s.rc == 0 and s.done["reason"] == "eof", f"rc {s.rc}")
    final = s.first("final", text="what can you do", id="wake")
    sent = s.first("sent", text="what can you do")
    check(s.index(final) < s.index(sent), "final before sent")
    speaks = s.all("speak")
    check(len(speaks) >= 3, f"reply spoken in chunks: {speaks}")
    rid = speaks[0]["id"]
    check(all(x["id"] == rid for x in speaks), f"one speech id for the reply: {speaks}")
    check(all(x["more"] for x in speaks[:-1]) and speaks[-1]["more"] is False, "more:true chunks, final more:false")
    reply = s.first("reply")
    check(s.index(speaks[0]) < s.index(reply), "speech started while the reply was still streaming")
    spoken = " ".join(x["text"] for x in speaks)
    check("Shadow Assistant" in spoken and "**" not in spoken and "- " not in spoken and "*" not in spoken,
          f"Markdown stripped: {spoken}")
    end = s.first("speak_end", id=rid)
    check(end["interrupted"] is False, "not interrupted")
    fu = s.first("follow_up")
    check(fu["mode"] == "followup" and fu["timeout"] == 5 and s.index(fu) > s.index(end), f"follow-up after speak_end: {fu}")
    s.first("timeout", id=fu["id"])
    d = s.done
    check(d["finals"] == 1 and d["sent"] == 1 and d["replies"] == 1 and d["voiceTurn"] is False, f"{d}")
    # 33 ms ticks: up to 31 land in one wall-clock second, +1 for a text change made while idle.
    check(0 < d["levels_max_hz"] <= 32, f"level notifications ≤ ~30 Hz: {d['levels_max_hz']}")
    check(d["micLevel"] == 0 and d["speechLevel"] == 0, "levels decayed to 0")
    check(os.path.exists(os.path.join(r.out, f"speak-{rid}.wav")), "speech written to --voice-out")
    check(d["statusText"] == "Natural voice ready · Heart · listening for “Shadow”", d["statusText"])


@test
def ends_after_input(suite):
    """After the end of the input a listen is refused at once (no state change): the session
    still notices it is over (regression: it used to run into its timeout)."""
    r = Run(suite, "input_end")
    s = r.session({"utterances": [{"at": 0.3, "text": "hello there"}], "eof_at": 0.6}, timeout=20)
    check(s.rc == 0 and s.done["reason"] == "eof", f"rc {s.rc} {s.done.get('reason')}")
    fu = s.first("follow_up")
    s.first("timeout", id=fu["id"], reason="no-input")
    check(s.done["t"] - fu["t"] < 5, f"ended {s.done['t'] - fu['t']:.1f} s after the refused listen")


@test
def followup_continues(suite):
    """A follow-up heard without the wake word is sent as the next message."""
    r = Run(suite, "followup")
    s = r.session({"utterances": [{"at": 0.4, "text": "hello there", "heard": "Shadow, hello there"}],
                   "answers": {"followup": [{"text": "and what else"}]}})
    check(s.rc == 0, f"rc {s.rc}")
    sents = [e["text"] for e in s.all("sent")]
    check(sents == ["hello there", "and what else"], f"{sents}")
    check(len(s.all("reply")) == 2 and len(s.all("follow_up")) == 2, "second reply, second follow-up")


@test
def push_to_talk_finish(suite):
    """Push-to-talk: the second click transcribes what was said (it does not discard it)."""
    r = Run(suite, "ptt")
    s = r.session({"answers": {"command": [{"text": "tell me a joke", "hold": True}]}},
                  actions="0.3:start;1.3:finish")
    check(s.rc == 0, f"rc {s.rc}")
    lis = s.first("listen", mode="command")
    fin = s.first("command", name="finish")
    final = s.first("final", text="tell me a joke")
    check(s.index(lis) < s.index(fin) < s.index(final), "listen → finish → final")
    s.first("sent", text="tell me a joke")
    s.first("reply")
    check(not s.all("unavailable"), f"{s.all('unavailable')}")


@test
def toggle_listening(suite):
    """toggleListening(): first call listens, second finishes."""
    r = Run(suite, "toggle")
    s = r.session({"answers": {"command": [{"text": "what is the weather", "hold": True}]}},
                  actions="0.3:toggle;1.0:toggle")
    s.first("command", name="finish")
    s.first("sent", text="what is the weather")


@test
def cancel_discards(suite):
    """cancel(): no transcription, nothing sent, the voice turn ends."""
    r = Run(suite, "cancel")
    s = r.session({"answers": {"command": [{"text": "never sent", "hold": True}]}},
                  actions="0.3:start;0.9:cancel")
    check(s.rc == 0, f"rc {s.rc}")
    s.first("command", name="cancel")
    check(not s.all("final") and not s.all("sent"), "nothing transcribed or sent")
    turns = [e["value"] for e in s.all("voice_turn")]
    check(turns == [True, False], f"voice turn on then off: {turns}")
    check(s.done["state"] == "idle", s.done["state"])


@test
def yes_no_confirms_tool(suite):
    """A tool needing approval is asked aloud; "yes" runs it via confirmTool, the result is spoken."""
    r = Run(suite, "yesno_yes")
    r.conf("[behaviour]\nautonomy=ask_every\n")
    s = r.session({"utterances": [{"at": 0.4, "text": "run a command that lists the files in tmp"}],
                   "answers": {"yesno": [{"text": "yes please"}]}})
    check(s.rc == 0, f"rc {s.rc}")
    conf = s.first("confirm")
    ask = s.first("speak", text="I'd like to run a command. Shall I go ahead?")
    check(ask["more"] is False, "the question closes its speech")
    yn = s.first("listen", mode="yesno")
    end = s.first("speak_end", id=ask["id"])
    check(s.index(end) < s.index(yn), "listen for the answer after the question was spoken")
    res = s.first("yesno")
    check(res["yes"] is True and res["tag"] == conf["tag"], f"{res} vs {conf}")
    tools = [e["status"] for e in s.all("tool")]
    check(tools[:2] == ["running", "done"], f"tool ran: {tools}")
    check(s.all("reply") and s.index(s.first("reply")) > s.index(res), "reply after the answer")


@test
def yes_no_denies_tool(suite):
    """"No" denies the tool."""
    r = Run(suite, "yesno_no")
    r.conf("[behaviour]\nautonomy=ask_every\n")
    s = r.session({"utterances": [{"at": 0.4, "text": "run a command that lists the files in tmp"}],
                   "answers": {"yesno": [{"text": "no, don't"}]}})
    check(s.first("yesno")["yes"] is False, "answered no")
    check([e["status"] for e in s.all("tool")] == ["denied"], f"{s.all('tool')}")


@test
def yes_no_helper_classifies(suite):
    """The helper's own yes/no classification ("answer") is used when it sends one."""
    r = Run(suite, "yesno_answer")
    r.conf("[behaviour]\nautonomy=ask_every\n")
    s = r.session({"utterances": [{"at": 0.4, "text": "run a command that lists the files in tmp"}],
                   "answers": {"yesno": [{"text": "uh huh", "answer": "yes"}]}})
    check(s.first("yesno")["yes"] is True, "the helper's answer decided")
    check([e["status"] for e in s.all("tool")][:2] == ["running", "done"], f"{s.all('tool')}")


@test
def yes_no_timeout_leaves_card(suite):
    """No answer: the card decides — nothing is confirmed or denied, nothing more is said."""
    r = Run(suite, "yesno_timeout")
    r.conf("[behaviour]\nautonomy=ask_every\n")
    s = r.session({"utterances": [{"at": 0.4, "text": "run a command that lists the files in tmp"}]}, timeout=6)
    check(s.rc == 1 and s.done["reason"] == "timeout", "the card still waits (session runs into its timeout)")
    yn = s.first("listen", mode="yesno")
    s.first("timeout", id=yn["id"])
    check(not s.all("yesno") and not s.all("tool"), "no decision was taken")
    check(s.done["voiceTurn"] is True, "the voice turn waits for the card")
    after = [e for e in s.events[s.index(yn):] if e["event"] == "speak"]
    check(not after, f"nothing said after the timeout: {after}")


@test
def spoken_yes_while_card_waits(suite):
    """A card waits after a missed answer; "Shadow, yes" then confirms it (the first waiting call)."""
    r = Run(suite, "yes_later")
    r.conf("[behaviour]\nautonomy=ask_every\n")
    s = r.session({"utterances": [{"at": 0.4, "text": "run a command that lists the files in tmp"},
                                  {"at": 4.0, "text": "yes", "heard": "Shadow, yes"}]})
    check(s.rc == 0, f"rc {s.rc}")
    check([e["status"] for e in s.all("tool")][:2] == ["running", "done"], f"{s.all('tool')}")
    check(not s.all("unavailable"), f"{s.all('unavailable')}")


@test
def spoken_yes_answers_one_call(suite):
    """Two calls wait at once: a spoken "yes" answers the card's question — the first call only.
    The second one is asked aloud on its own (here: "no"), never run unseen."""
    r = Run(suite, "yes_one")
    r.conf("[behaviour]\nautonomy=ask_every\n")
    marker = os.path.join(r.dir, "SECOND-CALL-RAN")
    mock = ScriptedMock([{"tools": [{"name": "get_system_info", "input": {}},
                                    {"name": "run_shell_command", "input": {"command": "touch " + marker}}]},
                         {"text": "Here is what I found."}])
    try:
        s = r.session({"utterances": [{"at": 0.4, "text": "check my system", "heard": "Shadow, check my system"},
                                      {"at": 4.0, "text": "yes", "heard": "Shadow, yes"}],
                       "answers": {"yesno": [{"timeout": True}, {"text": "no"}]}},
                      extra={"SHADOW_ASSISTANT_BASE_URL": mock.url})
    finally:
        mock.stop()
    check(s.rc == 0, f"rc {s.rc}\n{s.tail()}")
    tools = [(e["name"], e["status"]) for e in s.all("tool")]
    check(("get_system_info", "done") in tools, f"the first call ran: {tools}")
    check(("run_shell_command", "running") not in tools and ("run_shell_command", "denied") in tools,
          f"the second call was asked and denied, not run: {tools}")
    check(not os.path.exists(marker), "the second command never ran")
    yes = s.first("final", text="yes")
    after = s.events[s.index(yes):]
    asks = [e for e in after if e["event"] == "speak" and e.get("text") == "I'd like to run a command. Shall I go ahead?"]
    check(asks, "the second call is asked aloud: " + json.dumps([e for e in after if e["event"] == "speak"]))
    check([e for e in after if e["event"] == "listen" and e.get("mode") == "yesno"], "and its answer listened for")


@test
def sse_error_frees_the_agent(suite):
    """An API error inside the stream (an SSE "error" event after part of the answer): the request
    is closed at once, so the phase change that reports the error already says busy=false."""
    r = Run(suite, "sse_error")
    mock = ScriptedMock([{"text": "Partial answer", "sse_error": "Overloaded"},
                         {"text": "The second answer."}])
    try:
        s = r.session(actions="0.3:send:first question;2.0:send:second question",
                      extra={"SHADOW_ASSISTANT_BASE_URL": mock.url})
    finally:
        mock.stop()
    check(s.rc == 0, f"rc {s.rc}\n{s.tail()}")
    err = s.first("error", source="agent")
    check("Overloaded" in err["message"], err["message"])
    later = [e for e in s.events[s.index(err):] if e["event"] == "phase"]
    check(later and all(e["busy"] is False for e in later if e["value"] in ("error", "idle")),
          f"not busy once the error is reported: {later}")
    check(len(mock.requests) == 2, f"the next message was sent: {mock.requests}")


class SavedConversations:
    """Watches the conversation files a run saves (~/.local/share/<org>/<app>/*.json) while its
    session runs (a session deletes the conversation it created when it ends) and keeps, per file,
    the longest version that parsed."""

    def __init__(self, r):
        self.dir = os.path.join(r.home, ".local", "share")
        self.seen = {}
        self.done = threading.Event()
        self.thread = threading.Thread(target=self.loop, daemon=True)
        self.thread.start()

    def loop(self):
        while not self.done.is_set():
            for root, _dirs, files in os.walk(self.dir):
                for name in files:
                    if not name.endswith(".json"):
                        continue
                    try:
                        with open(os.path.join(root, name)) as f:
                            conv = json.load(f)
                    except (OSError, ValueError):
                        continue   # gone, or caught mid-write
                    if len(conv.get("messages", [])) >= len(self.seen.get(name, {}).get("messages", [])):
                        self.seen[name] = conv
            time.sleep(0.02)

    def stop(self):
        self.done.set()
        self.thread.join()
        return list(self.seen.values())


@test
def user_and_error_rows(suite):
    """A typed message is a user row of exactly its text, sent as a plain user turn; the API error
    it got is an error row of exactly the error's text that stays local (never sent back). Neither
    carries any other field (tool ids, replayed thinking): the rows MessageModel::appendUser /
    appendError build field by field (a positional list of every field once fell behind the
    struct, -Wmissing-field-initializers)."""
    r = Run(suite, "rows")
    mock = ScriptedMock([{"text": "Partial answer", "sse_error": "Overloaded right now"},
                         {"text": "The second answer."}])
    watch = SavedConversations(r)
    try:
        s = r.session(actions="0.3:send:first question;2.0:send:second question",
                      extra={"SHADOW_ASSISTANT_BASE_URL": mock.url})
    finally:
        mock.stop()
        saved = watch.stop()
    check(s.rc == 0, f"rc {s.rc}\n{s.tail()}")
    err = s.first("error", source="agent")
    check("Overloaded right now" in err["message"], err["message"])
    check(len(saved) == 1, f"one conversation saved while the session ran: {saved}")
    rows = saved[0].get("messages", [])
    check([m for m in rows if m.get("kind") == "user"]
          == [{"kind": "user", "content": "first question"}, {"kind": "user", "content": "second question"}],
          f"user rows: {rows}")
    check([m for m in rows if m.get("kind") == "error"] == [{"kind": "error", "content": err["message"]}],
          f"error row: {rows}")
    check([m.get("kind") for m in rows] == ["user", "assistant", "error", "user", "assistant"], f"rows: {rows}")
    check(len(mock.bodies) == 2, f"two requests: {mock.bodies}")
    first, second = mock.bodies
    check(first == [{"role": "user", "content": "first question"}], f"first request: {first}")
    check([m["content"] for m in second if m.get("role") == "user"] == ["first question", "second question"],
          f"user turns of the second request: {second}")
    check([m.get("role") for m in second] == ["user", "assistant", "user"], f"second request: {second}")
    check("Overloaded" not in json.dumps(second), f"the error row stays local: {second}")
    check(all(isinstance(m.get("content"), str) for m in second if m.get("role") == "user"),
          f"user turns carry text only: {second}")


@test
def crash_mid_reply_one_voice(suite):
    """The helper dies in the middle of a spoken reply: eSpeak takes over that reply and keeps it
    to its end, even once the helper is back — two voices never read one reply at once."""
    r = Run(suite, "crash_reply")
    mock = ScriptedMock([{"text": "First sentence of a long answer. " + " ".join(
        f"Sentence number {i} goes on for a little while." for i in range(2, 9)), "delay": 0.08}])
    try:
        s = r.session(actions="0.3:send:tell me a long story", timeout=40,
                      extra={"SHADOW_ASSISTANT_BASE_URL": mock.url, "FAKE_VOICED_CRASH": "speak",
                             "FAKE_VOICED_CRASH_TIMES": "1"})
    finally:
        mock.stop()
    check(s.rc == 0, f"rc {s.rc}\n{s.tail()}")
    restarts = s.all("engine_restart")
    check(restarts, "the helper crashed and was restarted")
    readies = s.all("ready")
    check(len(readies) >= 2, f"the helper came back: {len(readies)} ready events")
    back = s.index(readies[-1])
    reply_ids = {e["id"] for e in s.events[:back] if e["event"] == "speak" and str(e.get("id", "")).startswith("r")}
    check(reply_ids, "the reply had started to speak before the crash")
    later = [e for e in s.events[back:] if e["event"] == "speak" and e.get("id") in reply_ids]
    check(not later, f"after the restart nothing of that reply goes to the helper: {later}")
    check(r.espeak_calls(), "eSpeak read the rest of the reply")


@test
def busy_transcript_heard(suite):
    """A command while the agent still works: heard(text) + "I'm still working…", nothing sent."""
    r = Run(suite, "busy")
    s = r.session({"utterances": [{"at": 0.4, "text": "first question"},
                                  {"at": 1.2, "text": "second question"}]},
                  actions="2.5:stop-agent", api="timeout")
    check(s.rc == 0, f"rc {s.rc}")
    check([e["text"] for e in s.all("sent")] == ["first question"], f"{s.all('sent')}")
    s.first("heard", text="second question")
    s.first("unavailable", reason="I'm still working on the last request.")


@test
def error_ends_voice_turn(suite):
    """An API error during a voice turn: a short spoken apology, then the turn ends."""
    r = Run(suite, "error")
    s = r.session({"utterances": [{"at": 0.4, "text": "hello there"}]}, api="auth401")
    check(s.rc == 0, f"rc {s.rc}")
    err = s.first("error", source="agent")
    sorry = s.first("speak", text="Sorry, that didn't work.")
    check(s.index(err) < s.index(sorry), "apology after the error")
    check([e["value"] for e in s.all("voice_turn")] == [True, False], f"{s.all('voice_turn')}")
    check(not s.all("follow_up"), "no follow-up after a failure")


@test
def stop_stops_speech(suite):
    """The Stop button (agent.stop) also stops the voice."""
    r = Run(suite, "stop")
    s = r.session(actions="0.3:send:Hello there;1.2:stop-agent", extra={"FAKE_VOICED_CPS": "30"})
    check(s.rc == 0, f"rc {s.rc}")
    act = s.first("action", name="stop-agent")
    stop = s.first("command", name="stop")
    check(s.index(stop) > s.index(act), "stop sent to the helper after Stop")
    later = [e for e in s.events[s.index(act):] if e["event"] == "speak"]
    check(not later, f"nothing spoken after Stop: {later}")
    check(s.all("speak_end", interrupted=True), "speech interrupted")
    check(s.done["voiceTurn"] is False, "turn ended")


@test
def background_needs_an_engine(suite):
    """--background (login autostart) leaves at once when the wake word cannot be heard,
    and stays (loads the hidden UI) when it can."""
    r = Run(suite, "background")
    headless = {"QT_QPA_PLATFORM": "offscreen"}  # login autostart has a display; this test has none
    t0 = time.time()
    p = r.run(["--background"], r.env(helper=NO_HELPER, api=None, extra=headless))
    check(p.returncode == 0 and "cannot be heard" in p.stderr and time.time() - t0 < 10, f"{p.returncode} {p.stderr}")
    p = r.run(["--background"], r.env(api=None, extra=dict(headless, FAKE_VOICED_PROBE="nodata")))
    check(p.returncode == 0 and "not installed" in p.stderr, f"{p.returncode} {p.stderr}")
    p = r.run(["--background"], r.env(api=None, extra={"QT_QPA_PLATFORM": "offscreen", "SHADOW_SMOKE_TEST": "1",
                                                       "SHADOW_SCREENSHOT_DELAY": "1500"}))
    check(p.returncode == 0 and "SHADOW-SMOKE-OK" in p.stdout, f"stays with a usable engine: {p.returncode} "
                                                               f"{p.stdout} {p.stderr[-800:]}")


# ----------------------------------------------------------------------------- engine health
@test
def crash_backoff_then_error(suite):
    """A helper that keeps crashing: restarts after 1 s, 2 s, then engine "error" with its stderr."""
    r = Run(suite, "crash")
    s = r.session(extra={"FAKE_VOICED_CRASH": "start"})
    check(s.rc == 0 and s.done["reason"] == "engine-settled", f"rc {s.rc} {s.done.get('reason')}")
    restarts = s.all("engine_restart")
    check([e["delay_ms"] for e in restarts] == [1000, 2000], f"backoff: {restarts}")
    gap = restarts[1]["t"] - restarts[0]["t"]
    check(0.9 <= gap <= 2.5, f"second failure about 1 s after the first: {gap:.2f}")
    d = s.done
    check(d["engine"] == "error" and "fake crash" in d["engineMessage"], f"{d['engine']}: {d['engineMessage']}")
    check(any("fake crash" in line for line in d["helper_stderr"]), "stderr kept")


@test
def crash_once_recovers(suite):
    """One crash after "ready": restarted after 1 s, and the voice works again."""
    r = Run(suite, "crash_once")
    s = r.session({"utterances": [{"at": 1.0, "text": "hello again"}]},
                  extra={"FAKE_VOICED_CRASH": "ready", "FAKE_VOICED_CRASH_TIMES": "1"})
    check(s.rc == 0, f"rc {s.rc}")
    check([e["delay_ms"] for e in s.all("engine_restart")] == [1000], f"{s.all('engine_restart')}")
    s.first("sent", text="hello again")
    s.first("reply")
    check(s.done["engine"] == "neural", s.done["engine"])


@test
def helper_missing_basic_voice(suite):
    """No helper, eSpeak NG present: engine "basic" — replies are spoken, listening is refused."""
    r = Run(suite, "basic")
    s = r.session(actions="0.2:start;0.4:speak:**Hello** there, [friend](https://x.y)!;0.8:send:hello",
                  helper=NO_HELPER)
    check(s.rc == 0, f"rc {s.rc}")
    d = s.done
    check(d["engine"] == "basic" and d["resolvedTtsEngine"] == "espeak", f"{d['engine']}")
    check("shadow-voice-setup" in d["engineMessage"] and "SHADOW_VOICED" in d["engineMessage"], d["engineMessage"])
    refused = s.first("unavailable")
    check("Speech recognition is not installed" in refused["reason"], refused["reason"])
    before_send = s.events[:s.index(s.first("action", name="send"))]
    check(not [e for e in before_send if e["event"] == "voice_turn" and e["value"]], "refused start left no voice turn")
    calls = r.espeak_calls()
    check(calls and calls[0] == ["-v", "en-us+f3", "-s", "175", "--", "Hello there, friend!"], f"{calls[:1]}")
    check(any("Shadow Assistant" in c[-1] for c in calls[1:]), f"reply spoken by eSpeak: {calls}")


@test
def nothing_installed(suite):
    """No helper and no eSpeak NG: engine "missing", honest message naming both and the fix."""
    r = Run(suite, "missing")
    s = r.session(actions="0.2:start", helper=NO_HELPER, espeak=False)
    d = s.done
    check(d["engine"] == "missing" and d["state"] == "unavailable", f"{d['engine']} {d['state']}")
    check("espeak-ng" in d["engineMessage"] and "shadow-voice-setup" in d["engineMessage"], d["engineMessage"])
    check(d["capabilities"]["tts"] is False and d["capabilities"]["stt"] is False, f"{d['capabilities']}")
    check(s.all("unavailable"), "push-to-talk refused")


@test
def models_missing(suite):
    """Probe says the data is missing (exit 3): basic voice, message names what and the fix."""
    r = Run(suite, "nodata")
    s = r.session(extra={"FAKE_VOICED_PROBE": "nodata"})
    d = s.done
    check(d["engine"] == "basic", d["engine"])
    check("Voice models are not installed" in d["engineMessage"] and "shadow-voice-setup" in d["engineMessage"],
          d["engineMessage"])
    check(not os.path.exists(r.state), "no --serve when the probe says the data is missing")


@test
def muted_refuses_listening(suite):
    """Muted: push-to-talk is refused (unavailable) and no voice turn is left on; the helper
    learns about the mute before anything else and is never asked to listen."""
    r = Run(suite, "muted")
    r.conf("[voice]\nmuted=true\n")
    s = r.session(actions="0.3:start")
    s.first("unavailable", reason="The microphone is muted.")
    check(not s.all("voice_turn"), f"{s.all('voice_turn')}")
    check(s.done["state"] == "muted", s.done["state"])
    cmds = r.helper_commands()
    check(cmds and cmds[0] == {"cmd": "mute", "value": True}, f"mute first: {cmds[:2]}")
    check(not [c for c in cmds if c["cmd"] == "listen"], "never asked to listen")


@test
def mute_cancels_listening(suite):
    """Muting while listening cancels the listen at once."""
    r = Run(suite, "mute_listen")
    s = r.session({"answers": {"command": [{"text": "not heard", "hold": True}]}},
                  actions="0.3:start;0.8:mute")
    cancel = s.first("command", name="cancel")
    check(s.index(cancel) > s.index(s.first("action", name="mute")), "cancel after mute")
    s.first("command", name="mute", value=True)
    check(not s.all("final") and s.done["voiceTurn"] is False, "nothing heard, turn ended")


@test
def microphone_failure(suite):
    """A capture failure reported by the helper is said, and listening is refused with it."""
    r = Run(suite, "mic_error")
    s = r.session({"mic": "error"}, actions="0.4:start")
    reasons = [e["reason"] for e in s.all("unavailable")]
    check(len(reasons) == 2 and all(x.startswith("Microphone not available:") for x in reasons), f"{reasons}")
    check(s.index(s.first("action", name="start")) < s.index(s.all("unavailable")[1]), "push-to-talk refused")
    check(not s.all("voice_turn") and not s.all("listen"), "no voice turn, no listen")


@test
def test_hooks(suite):
    """SHADOW_ASSISTANT_TEST_PARTIAL / _TEST_LEVEL give screenshots live-looking values."""
    r = Run(suite, "hooks")
    s = r.session(extra={"SHADOW_ASSISTANT_TEST_PARTIAL": "open the term", "SHADOW_ASSISTANT_TEST_LEVEL": "0.6"})
    st = s.first("session")
    check(st["partial"] == "open the term" and st["micLevel"] == 0.6 and st["speechLevel"] == 0.6, f"{st}")
    d = s.done
    check(d["partial"] == "open the term" and d["micLevel"] == 0.6, f"{d}")


@test
def install_failure(suite):
    """A failing installer: its error line becomes installStatus (and a toast); the engine stays as it was."""
    r = Run(suite, "install_fail")
    setup = os.path.join(r.dir, "shadow-voice-setup")
    with open(setup, "w") as f:
        f.write(f"#!{sys.executable} -I\nimport json, sys\n"
                "print(json.dumps({'step': 'download', 'progress': 0.1, 'message': 'Downloading the voice'}), flush=True)\n"
                "print(json.dumps({'step': 'error', 'code': 6, 'message': 'Download failed: no network'}), flush=True)\n"
                "sys.exit(6)\n")
    os.chmod(setup, 0o755)
    s = r.session(actions="0.3:install", extra={"SHADOW_VOICE_SETUP": setup, "FAKE_VOICED_PROBE": "nodata"})
    last = s.all("install")[-1]
    check(last["installing"] is False and last["status"] == "Download failed: no network", f"{last}")
    s.first("unavailable", reason="Download failed: no network")
    check(s.done["engine"] == "basic", s.done["engine"])


# ----------------------------------------------------------------------------- spokenSentence / listenMode
# Spoken through speak(): numbers, a path, a version and CamelCase come back from
# the helper spelled out ("thirty-eight point two gigabytes"); "42." has no word in
# common with its spoken form at all.
SPOKEN_TEXT = ("Your home folder is using 38.2 GB. Most of it sits in ~/Videos, about 21 GB. "
               "Version 3.13 of Python is installed, and PipeWire runs fine. 42. All done!")
SPOKEN_SENTENCES = ["Your home folder is using 38.2 GB.", "Most of it sits in ~/Videos, about 21 GB.",
                    "Version 3.13 of Python is installed, and PipeWire runs fine.", "42.", "All done!"]


def shown_during(s):
    """For each helper "sentence" event: (its text, the spokenSentence shown while it plays)."""
    out, current, pending = [], "", None
    for e in s.events:
        if e["event"] == "sentence":
            if pending is not None:
                out.append((pending, current))
            pending = e["text"]
        elif e["event"] == "spoken":
            current = e["text"]
        elif e["event"] == "speak_end" and pending is not None:
            out.append((pending, current))
            pending = None
    if pending is not None:
        out.append((pending, current))
    return out


@test
def spoken_sentence(suite):
    """spokenSentence: the sentence being spoken as the app sent it — digits, not the helper's
    words — in order; a helper sentence that cannot be matched shows nothing (never a wrong
    sentence); cleared when the speech ends."""
    r = Run(suite, "spoken")
    s = r.session(actions="0.3:speak:" + SPOKEN_TEXT, extra={"FAKE_VOICED_CPS": "80"})
    check(s.rc == 0, f"rc {s.rc}")
    sent = s.first("speak")
    check(sent["text"] == " ".join(SPOKEN_SENTENCES), f"sent as before (one chunk): {sent}")
    during = shown_during(s)
    check(any("thirty" in h for h, _ in during), f"the helper says numbers as words: {during}")
    for heard, shown in during:
        check(shown == "" or shown in SPOKEN_SENTENCES, f"{heard!r} showed {shown!r}")
    shown = [x for _, x in during if x]
    order = [SPOKEN_SENTENCES.index(x) for x in shown]
    check(order == sorted(order), f"in order: {shown}")
    for want in ("Your home folder is using 38.2 GB.", "Version 3.13 of Python is installed, and PipeWire runs fine.",
                 "All done!"):
        check(want in shown, f"{want!r} not shown: {during}")
    forty = [x for h, x in during if "forty" in h.lower()]
    check(forty == [""], f"'42.' has no word in common with 'forty-two': nothing, not a guess: {during}")
    spoken = s.all("spoken")
    check(spoken[-1]["text"] == "" and s.index(spoken[-1]) > s.index(s.first("speak_end", id=sent["id"])),
          "cleared at speak_end")


@test
def spoken_sentence_reply(suite):
    """A streamed voice reply: each sentence shown was sent to be spoken, in the order sent;
    Stop clears it at once and nothing is shown afterwards."""
    r = Run(suite, "spoken_reply")
    s = r.session(actions="0.3:send:Hello there;2.2:stop-agent", extra={"FAKE_VOICED_CPS": "25"})
    check(s.rc == 0, f"rc {s.rc}")
    sent = [e["text"] for e in s.all("speak") if e["text"]]
    shown = [x for _, x in shown_during(s) if x]
    check(shown, f"a sentence was shown while it was spoken: {s.tail()}")
    check(all(x in sent for x in shown), f"only sent sentences: {shown} vs {sent}")
    check([sent.index(x) for x in shown] == sorted(sent.index(x) for x in shown), f"in order: {shown}")
    stop = s.first("action", name="stop-agent")
    after = [e["text"] for e in s.events[s.index(stop):] if e["event"] == "spoken"]
    check(after and all(x == "" for x in after), f"cleared by Stop: {after}")


@test
def spoken_sentence_basic_voice(suite):
    """With the basic voice (eSpeak NG) a streamed sentence is shown while eSpeak says it."""
    r = Run(suite, "spoken_basic")
    s = r.session(actions="0.3:send:Hello there", helper=NO_HELPER)
    check(s.rc == 0, f"rc {s.rc}")
    said = [c[-1] for c in r.espeak_calls()]
    shown = [e["text"] for e in s.all("spoken") if e["text"]]
    check(shown and all(x in said for x in shown), f"{shown} vs {said}")
    check(s.all("spoken")[-1]["text"] == "", "cleared after the last sentence")


@test
def listen_mode(suite):
    """listenMode: "wake" while the helper takes a command after "Shadow", "followup" after a
    spoken reply, "command" for push-to-talk, "yesno" for a spoken confirmation, "" otherwise."""
    r = Run(suite, "listen_mode")
    s = r.session({"utterances": [{"at": 0.4, "text": "hello there", "heard": "Shadow, hello there"}]})
    check(s.rc == 0, f"rc {s.rc}")
    modes = [e["value"] for e in s.all("listen_mode")]
    check(modes[:2] == ["wake", ""], f"wake utterance: {modes}")
    fu = s.first("listen_mode", value="followup")
    check(s.index(fu) > s.index(s.all("speak_end")[-1]), "follow-up after the reply was spoken")
    check(modes[-1] == "", f"ends empty: {modes}")

    r = Run(suite, "listen_mode_ptt")
    r.conf("[behaviour]\nautonomy=ask_every\n")
    s = r.session({"answers": {"command": [{"text": "run a command that lists the files in tmp", "hold": True}],
                               "yesno": [{"text": "yes"}]}}, actions="0.3:start;1.0:finish")
    check(s.rc == 0, f"rc {s.rc}")
    modes = [e["value"] for e in s.all("listen_mode")]
    check(modes[:2] == ["command", ""], f"push-to-talk: {modes}")
    yn = s.first("listen_mode", value="yesno")
    check(s.index(yn) > s.index(s.first("confirm")), "yes/no after the question")
    check("wake" not in modes and modes[-1] == "", f"{modes}")


@test
def bare_wake_listens(suite):
    """A bare "Shadow": wakeDetected, then the helper listens for the command itself (listenMode
    "wake" lasts until its final) — what the UI greets with "Yes?". "Shadow, <command>" in one
    breath: the command follows the wake at once, the listen mode is over with it."""
    r = Run(suite, "bare_wake")
    s = r.session({"utterances": [{"at": 0.4, "bare": True, "then": "what time is it", "after": 1.0}]})
    check(s.rc == 0, f"rc {s.rc}\n{s.tail()}")
    wake = s.first("wake", via="stt")
    lm = [e for e in s.events[s.index(wake):] if e["event"] == "listen_mode"]
    check(lm and lm[0]["value"] == "wake", f"listening after a bare wake: {lm}")
    fin = s.first("final", id="wake")
    held = fin["t"] - lm[0]["t"]
    check(held > 0.5, f"the wake listen lasts until the command is said ({held:.2f} s)")
    s.first("sent", text="what time is it")
    after = [e["value"] for e in s.events[s.index(fin):] if e["event"] == "listen_mode"]
    check(after[:1] == [""], f"over with the final: {after}")


@test
def install_engine(suite):
    """installEngine(): progress from shadow-voice-setup --user --progress-json, then a re-probe."""
    r = Run(suite, "install")
    marker = os.path.join(r.dir, "installed")
    setup = os.path.join(r.dir, "shadow-voice-setup")
    with open(setup, "w") as f:
        f.write(f"#!{sys.executable} -I\nimport json, sys, time\n"
                "assert sys.argv[1:] == ['--user', '--progress-json'], sys.argv\n"
                "for i, msg in enumerate(['Downloading the voice', 'Downloading speech recognition', 'Verifying']):\n"
                "    print(json.dumps({'step': 'download', 'progress': (i + 1) / 4, 'message': msg}), flush=True)\n"
                "    time.sleep(0.15)\n"
                f"open({marker!r}, 'w').close()\n"
                "print(json.dumps({'step': 'done', 'ok': True, 'progress': 1.0, 'message': 'Installed'}), flush=True)\n")
    os.chmod(setup, 0o755)
    s = r.session(actions="0.3:install", extra={"SHADOW_VOICE_SETUP": setup, "FAKE_VOICED_INSTALLED": marker})
    check(s.rc == 0, f"rc {s.rc}")
    engines = [e["engine"] for e in s.all("engine")]
    check("basic" in engines and "installing" in engines and engines[-1] == "neural", f"{engines}")
    progress = [e["progress"] for e in s.all("install") if e["installing"]]
    check(progress == sorted(progress) and progress[-1] >= 0.75, f"{progress}")
    last = s.all("install")[-1]
    check(last["installing"] is False and last["progress"] == 1 and last["status"] == "The natural voice is installed.",
          f"{last}")


# ----------------------------------------------------------------------------- runner
def find_binary(explicit):
    cands = [explicit, os.environ.get("SHADOW_ASSISTANT_BIN")]
    for var in ("LD_LIBRARY_PATH", "QML_IMPORT_PATH"):  # scripts/dev.sh run: <build>/lib, <build>/qml
        for d in os.environ.get(var, "").split(":"):
            if d:
                cands.append(os.path.join(os.path.dirname(d.rstrip("/")), "bin", "shadow-assistant"))
    cands.append(shutil.which("shadow-assistant"))
    for c in cands:
        if c and os.path.isfile(c) and os.access(c, os.X_OK):
            return os.path.abspath(c)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--keep", action="store_true", help="keep the work directory")
    ap.add_argument("--verbose", action="store_true", help="print the event log of failed tests")
    ap.add_argument("--smoke", action="store_true", help="print SHADOW-SMOKE-OK when everything passed")
    ap.add_argument("tests", nargs="*")
    args = ap.parse_args()

    binary = find_binary(args.bin)
    if not binary:
        print("voice_tests: shadow-assistant not found (use --bin)", file=sys.stderr)
        return 2
    chosen = [t for t in TESTS if not args.tests or t.__name__ in args.tests]
    if not chosen:
        print("voice_tests: no such test; known: " + " ".join(t.__name__ for t in TESTS), file=sys.stderr)
        return 2
    work = tempfile.mkdtemp(prefix="shadow-voice-tests-")
    suite = Suite(binary, work, args.verbose)
    print(f"voice_tests: {binary}  ({len(chosen)} tests, work dir {work})", flush=True)
    results = {}
    t0 = time.time()

    def one(fn):
        start = time.time()
        try:
            fn(suite)
            return fn.__name__, None, time.time() - start
        except Failure as e:
            return fn.__name__, str(e), time.time() - start
        except Exception:  # a bug in the test or a crashed app
            return fn.__name__, traceback.format_exc(), time.time() - start

    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
            for name, err, secs in pool.map(one, chosen):
                results[name] = err
                status = "PASS" if err is None else "FAIL"
                print(f"{status}  {name}  ({secs:.1f}s)" + ("" if err is None else f"\n      {err}"), flush=True)
                if err is not None and args.verbose:
                    for d in sorted(os.listdir(work)):
                        if d.startswith(name.split("_")[0]):
                            log = os.path.join(work, d, "stdout.log")
                            if os.path.exists(log):
                                print(open(log).read()[-6000:])
    finally:
        suite.stop()
    failed = [n for n, e in results.items() if e is not None]
    print(f"voice_tests: {len(results) - len(failed)} passed, {len(failed)} failed in {time.time() - t0:.1f}s"
          + (f" ({' '.join(failed)})" if failed else ""), flush=True)
    if not args.keep and not failed:
        shutil.rmtree(work, ignore_errors=True)
    elif failed:
        print(f"voice_tests: logs kept in {work}", flush=True)
    if args.smoke and not failed:
        print("SHADOW-SMOKE-OK voice-tests", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
