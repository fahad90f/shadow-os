#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Headless tests of the model team (NVIDIA's hosted models working together).

Every test runs the REAL shadow-assistant binary without a window (--ask,
--dump, --set-api-key, --remove-api-key) against tests/mock_nvidia.py, a
stand-in for NVIDIA's API that records every request. Each test has its own
service, home directory and key, so nothing is shared and nothing reaches the
network; the keys are dummies (nvapi-test…).

    team_tests.py [--bin PATH] [--jobs N] [--keep] [--verbose] [--smoke] [TEST...]

--bin defaults to $SHADOW_ASSISTANT_BIN, then <build>/bin/shadow-assistant next
to the <build>/lib of $LD_LIBRARY_PATH, then shadow-assistant on PATH. --smoke
prints SHADOW-SMOKE-OK on success (applications/assistant/smoke-tests.list).
Exit status: 0 all passed, 1 a test failed, 2 setup error.
"""
import argparse
import concurrent.futures
import json
import os
import shutil
import socket
import stat
import struct
import subprocess
import sys
import tempfile
import time
import traceback
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from mock_nvidia import Behaviour, MockNvidia, short  # noqa: E402

MOCK_ANTHROPIC = os.path.join(HERE, "mock_api.py")

KIMI = "moonshotai/kimi-k3.1-pro"
ULTRA = "nvidia/nemotron-3-ultra-550b-a55b"
MISTRAL = "mistralai/mistral-large-4-1t-a52b"
MINIMAX = "minimaxai/minimax-m3.1-flash"
SUPER_VL = "nvidia/nemotron-3.5-super-vl-120b-a12b"
LAGUNA = "poolside/laguna-xs-2.1"
GEMMA = "google/gemma-4-31b-it"
BOLT = "nvidia/nemotron-3.5-bolt-e4b"
LIGHTNING = "nvidia/nemotron-3.5-lightning-30b-a3b"
STEP = "stepfun-ai/step-5-preview"
GLM = "z-ai/glm-5.3-flash"
MUSE = "meta/muse-glimmer-30b"

# The built-in groups (assistantconfig.cpp), lead first where it matters.
CODING = [KIMI, MINIMAX, SUPER_VL, LAGUNA, ULTRA, GEMMA]
AGENTS = [KIMI, BOLT, MINIMAX, SUPER_VL, LIGHTNING, LAGUNA, ULTRA, GEMMA]
CHAT = [MISTRAL, BOLT, SUPER_VL, STEP, GLM, MUSE, LAGUNA, ULTRA]
VISION_FIRST4 = [SUPER_VL, KIMI, STEP, GLM]

CHAT_Q = "What is the capital of France?"
CODE_Q = "Write a python function that reverses a string"
TOOL_Q = "Please run a command that prints team-ok"

SAVED_KEY = "nvapi-test-saved-000000000000000000001234"
CONF_KEY = "nvapi-test-conf-0000000000000000000005678"


class Failure(AssertionError):
    pass


def check(cond, msg):
    if not cond:
        raise Failure(msg)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def png(path, width, height, noise=False):
    """A PNG file written with nothing but zlib: plain colour bands, or noise
    (which no compressor can shrink, the worst case for the size limit)."""
    rows = []
    for y in range(height):
        if noise:
            row = os.urandom(width * 3)
        else:
            row = bytes(((y * 3) % 256, 80, 200)) * width
        rows.append(b"\x00" + row)
    raw = b"".join(rows)

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 1)) + chunk(b"IEND", b""))


class Suite:
    def __init__(self, binary, workdir, verbose):
        self.bin = binary
        self.work = workdir
        self.verbose = verbose


class Result:
    def __init__(self, proc, events):
        self.rc = proc.returncode
        self.stdout = proc.stdout
        self.stderr = proc.stderr
        self.events = events
        self.turns = [e for e in events if e.get("event") in ("turn", "done")]
        self.done = next((e for e in reversed(events) if e.get("event") == "done"), None)

    def state(self, model, event=None):
        team = (event or self.done)["team"]
        return next((m["state"] for m in team if m["id"] == model), None)


class Run:
    """One isolated run: its own home directory and its own NVIDIA stand-in."""

    def __init__(self, suite, name, behaviour=None, service=True):
        self.suite = suite
        self.dir = tempfile.mkdtemp(prefix=name + "-", dir=suite.work)
        self.home = os.path.join(self.dir, "home")
        self.confdir = os.path.join(self.home, ".config", "shadow")
        os.makedirs(self.confdir)
        self.rt = os.path.join(self.dir, "run")
        os.makedirs(self.rt, mode=0o700)
        self.conf_path = os.path.join(self.confdir, "assistant.conf")
        self.key_path = os.path.join(self.confdir, "assistant.key")
        self.cache_path = os.path.join(self.home, ".cache", "shadow", "assistant-models.json")
        self.mock = MockNvidia(behaviour or Behaviour()).start() if service else None
        self.n = 0

    def close(self):
        if self.mock:
            self.mock.stop()

    def conf(self, text):
        with open(self.conf_path, "w") as f:
            f.write(text)

    def env(self, key="nvapi-test", base=None, extra=None):
        e = {"PATH": "/usr/local/bin:/usr/bin:/bin", "HOME": self.home, "XDG_RUNTIME_DIR": self.rt,
             "LANG": "C.UTF-8", "SHADOW_VOICED": "/nonexistent/shadow-voiced"}
        if key:
            e["SHADOW_ASSISTANT_API_KEY"] = key
        url = base if base is not None else (self.mock.url if self.mock else "")
        if url:
            e["SHADOW_ASSISTANT_BASE_URL"] = url
        build = os.path.dirname(os.path.dirname(self.suite.bin))
        if os.path.isdir(os.path.join(build, "lib")):
            e["LD_LIBRARY_PATH"] = os.path.join(build, "lib")
        if os.path.isdir(os.path.join(build, "qml")):
            e["QML_IMPORT_PATH"] = os.path.join(build, "qml")
        e.update(extra or {})
        return e

    def run(self, args, env=None, stdin=None, timeout=90):
        self.n += 1
        p = subprocess.run([self.suite.bin] + list(args), env=env or self.env(), input=stdin,
                           capture_output=True, text=True, timeout=timeout)
        for name, text in (("stdout", p.stdout), ("stderr", p.stderr)):
            with open(os.path.join(self.dir, f"{name}-{self.n}.log"), "w") as f:
                f.write(text)
        return p

    def ask(self, *texts, args=(), timeout=60, **envargs):
        argv = []
        for t in texts:
            argv += ["--ask", t]
        argv += ["--ask-timeout", str(timeout)] + list(args)
        p = self.run(argv, self.env(**envargs), timeout=timeout + 30)
        events = []
        for line in p.stdout.splitlines():
            line = line.strip()
            if line.startswith("{"):
                events.append(json.loads(line))
        r = Result(p, events)
        check(r.done is not None, f"no 'done' line (exit {p.returncode}):\n{p.stdout[-1500:]}\n{p.stderr[-1500:]}")
        return r

    def dump(self, topic, **envargs):
        p = self.run(["--dump", topic], self.env(**envargs))
        check(p.returncode == 0, f"--dump {topic} failed: {p.stderr[-800:]}")
        return json.loads(p.stdout)

    # What the stand-in saw.
    def chats(self):
        return self.mock.chat_requests()

    def advisors(self):
        return [r for r in self.chats() if r["advisor"]]

    def leads(self):
        return [r for r in self.chats() if not r["advisor"] and not r["images"]]


TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn


def names(models):
    return sorted(short(m) for m in models)


# ----------------------------------------------------------------------------- the team at work
@test
def chat_team_works_in_parallel(suite):
    """A question goes to every chat model at once; the lead answers with all their notes."""
    b = Behaviour()
    b.default_delay = 0.6
    r = Run(suite, "chat_team", b)
    try:
        res = r.ask(CHAT_Q)
        d = res.done
        check(res.rc == 0 and d["error"] == "", f"failed: {d}")
        check(d["provider"] == "nvidia" and d["kind"] == "chat" and d["model"] == MISTRAL, f"{d['provider']} {d['kind']} {d['model']}")
        adv, leads = r.advisors(), r.leads()
        check(names(a["model"] for a in adv) == names(CHAT[1:]), f"advisors asked: {names(a['model'] for a in adv)}")
        check(len(leads) == 1 and leads[0]["model"] == MISTRAL, f"lead requests: {[l['model'] for l in leads]}")
        # At the same time: every advisor was asked before the first one had answered.
        check(max(a["start"] for a in adv) < min(a["end"] for a in adv),
              f"advisors were not asked at the same time: {[(round(a['start'] % 100, 2), round(a['end'] % 100, 2)) for a in adv]}")
        # The lead waited for them, and got every note.
        check(leads[0]["start"] >= max(a["end"] for a in adv) - 0.05, "the lead started before the advisors were done")
        check(sorted(leads[0]["notes"]) == names(CHAT[1:]), f"notes in the lead's prompt: {leads[0]['notes']}")
        # Seven models at 0.6 s each, one after the other, would be 4.2 s before the lead even starts.
        check(d["t"] < 3.0, f"took {d['t']} s: not parallel")
        # Only the lead can act.
        check(all(a["tools"] == [] for a in adv), "an advisor was given tools")
        check("run_shell_command" in leads[0]["tools"], f"the lead has no tools: {leads[0]['tools']}")
        check(d["reply"].startswith("LEAD mistral-large-4-1t-a52b | notes: "), f"reply: {d['reply']!r}")
        check(all(m["state"] == "done" for m in d["team"]) and len(d["team"]) == 8, f"team: {d['team']}")
        check(all(c["authorised"] for c in r.chats()), "a request went out without the key")
        # The progress lines show the team gathering, then the lead working.
        team_events = [e for e in res.events if e["event"] == "team"]
        check(any(e["gathering"] for e in team_events), "no progress line while the team worked")
        check(any(not e["gathering"] and e["members"][0]["state"] == "working" for e in team_events),
              "no progress line for the lead answering")
    finally:
        r.close()


@test
def coding_team(suite):
    """A programming request goes to the coding models; kimi leads."""
    r = Run(suite, "coding")
    try:
        d = r.ask(CODE_Q).done
        check(d["kind"] == "coding" and d["model"] == KIMI, f"{d['kind']} {d['model']}")
        check(names(a["model"] for a in r.advisors()) == names(CODING[1:]), f"{names(a['model'] for a in r.advisors())}")
        check(sorted(r.leads()[0]["notes"]) == names(CODING[1:]), f"{r.leads()[0]['notes']}")
        # The mock's note says which brief the advisor got.
        check(d["error"] == "" and "notes: " in d["reply"], f"{d}")
        # Code gets more room than talk.
        check(all(a["max_tokens"] >= 2000 for a in r.advisors()), f"{[a['max_tokens'] for a in r.advisors()]}")
    finally:
        r.close()


@test
def action_runs_once(suite):
    """Eight models plan, one command runs: only the lead acts, and only once."""
    b = Behaviour()
    marker = None
    r = Run(suite, "action", b)
    try:
        marker = os.path.join(r.dir, "ran.txt")
        b.tool_call = ("run_shell_command", {"command": "sh", "args": ["-c", f"echo ran >> {marker}; echo team-ok"]})
        res = r.ask(TOOL_Q)
        d = res.done
        check(d["kind"] == "agents" and d["model"] == ULTRA, f"{d['kind']} {d['model']}")
        check([t["name"] for t in d["tools"]] == ["run_shell_command"] and d["tools"][0]["status"] == "done", f"{d['tools']}")
        with open(marker) as f:
            runs = f.read().count("ran")
        check(runs == 1, f"the command ran {runs} times")
        check("team-ok" in d["reply"] and d["reply"].startswith("LEAD nemotron-3-ultra-550b-a55b"), f"{d['reply']!r}")
        adv, leads = r.advisors(), r.leads()
        # The advisors work once per message, not once per round.
        check(len(adv) == len(AGENTS) - 1, f"{len(adv)} advisor requests")
        check(len(leads) == 2 and all(l["model"] == ULTRA for l in leads), f"{[l['model'] for l in leads]}")
        check(sorted(leads[0]["notes"]) == sorted(leads[1]["notes"]) == names(m for m in AGENTS if m != ULTRA),
              f"notes: {leads[0]['notes']} / {leads[1]['notes']}")
        check(leads[1]["roles"][-2:] == ["assistant", "tool"], f"second round: {leads[1]['roles']}")
    finally:
        r.close()


@test
def asks_before_acting(suite):
    """With "ask every time" the team's plan waits for a yes; a no runs nothing."""
    for answer, expect in (("", "pending"), ("yes", "done"), ("no", "denied")):
        b = Behaviour()
        r = Run(suite, "asks_" + (answer or "wait"), b)
        try:
            marker = os.path.join(r.dir, "ran.txt")
            b.tool_call = ("run_shell_command", {"command": "sh", "args": ["-c", f"echo ran >> {marker}"]})
            r.conf("[behaviour]\nautonomy=ask_every\n")
            res = r.ask(TOOL_Q, args=["--ask-approve", answer] if answer else [])
            d = res.done
            check(d["tools"] and d["tools"][0]["status"] == expect, f"answer {answer!r}: {d['tools']}")
            check(os.path.exists(marker) == (answer == "yes"), f"answer {answer!r}: ran = {os.path.exists(marker)}")
            check(d["awaiting"] == (answer == ""), f"answer {answer!r}: awaiting = {d['awaiting']}")
            check(any(e["event"] == "confirm" for e in res.events), "no question was asked")
        finally:
            r.close()


@test
def think_hard_brings_everyone(suite):
    """Think hard: every model of every group works on the request."""
    r = Run(suite, "think_hard")
    try:
        d = r.ask(CHAT_Q, args=["--ask-think-hard"]).done
        everyone = set(CHAT) | set(CODING) | set(AGENTS)
        check(names(a["model"] for a in r.advisors()) == names(everyone - {MISTRAL}),
              f"{names(a['model'] for a in r.advisors())}")
        check(d["model"] == MISTRAL and len(r.leads()[0]["notes"]) == len(everyone) - 1, f"{d['model']} {r.leads()[0]['notes']}")
    finally:
        r.close()


@test
def solo_mode(suite):
    """[team] mode=solo: one model, one request."""
    r = Run(suite, "solo")
    try:
        r.conf("[team]\nmode=solo\n")
        d = r.ask(CHAT_Q).done
        chats = r.chats()
        check(len(chats) == 1 and chats[0]["model"] == MISTRAL and not chats[0]["advisor"], f"{[(c['model'], c['advisor']) for c in chats]}")
        check(chats[0]["notes"] == [] and d["reply"].startswith("LEAD mistral-large-4-1t-a52b | notes:  |"), f"{d['reply']!r}")
        check(len(d["team"]) == 1 and d["team"][0]["role"] == "lead", f"{d['team']}")
        check(r.dump("config")["team"]["mode"] == "solo", "config does not say solo")
    finally:
        r.close()


@test
def max_models_limits_the_team(suite):
    """[team] max_models=3: the lead and two others."""
    r = Run(suite, "max_models")
    try:
        r.conf("[team]\nmax_models=3\n")
        d = r.ask(CHAT_Q).done
        check([a["model"] for a in sorted(r.advisors(), key=lambda a: CHAT.index(a["model"]))] == CHAT[1:3],
              f"{[a['model'] for a in r.advisors()]}")
        check(len(d["team"]) == 3, f"{d['team']}")
    finally:
        r.close()


@test
def custom_groups(suite):
    """The groups can be set in the conf file; unknown models are simply not asked."""
    r = Run(suite, "custom")
    try:
        r.conf(f"[team]\nchat={GEMMA}, example/not-a-model, {LAGUNA}, {KIMI}\nlead_chat={LAGUNA}\n")
        d = r.ask(CHAT_Q).done
        check(d["model"] == LAGUNA, f"lead: {d['model']}")
        check(names(a["model"] for a in r.advisors()) == names([GEMMA, KIMI]), f"{names(a['model'] for a in r.advisors())}")
        check(not any(c["model"] == "example/not-a-model" for c in r.chats()), "a model the service does not list was asked")
    finally:
        r.close()


@test
def multi_turn_conversation(suite):
    """The second message: advisors get the conversation as alternating turns; the lead gets it all."""
    r = Run(suite, "multi_turn")
    try:
        res = r.ask(CHAT_Q, "And of Italy?")
        check(len(res.turns) == 2 and all(t["error"] == "" for t in res.turns), f"{res.turns}")
        check("Italy" in res.turns[1]["reply"] and "France" in res.turns[0]["reply"], f"{[t['reply'] for t in res.turns]}")
        second = [c for c in r.chats() if c["last_user"] == "And of Italy?"]
        adv = [c for c in second if c["advisor"]]
        lead = [c for c in second if not c["advisor"]]
        check(len(adv) == 7 and len(lead) == 1, f"{len(adv)} advisors, {len(lead)} leads in turn 2")
        check(all(a["roles"] == ["system", "user", "assistant", "user"] for a in adv), f"{adv[0]['roles']}")
        check(lead[0]["roles"] == ["system", "user", "assistant", "user"], f"{lead[0]['roles']}")
        # Turn 2's notes are turn 2's: seven fresh ones.
        check(len(lead[0]["notes"]) == 7, f"{lead[0]['notes']}")
    finally:
        r.close()


@test
def classification(suite):
    """Which team a request goes to."""
    cases = [
        ("What is the capital of France?", "chat"), ("How do I make pancakes?", "chat"),
        ("What type of dog is calm?", "chat"), ("Tell me about Java, the island", "chat"),
        ("Is rust on a car dangerous?", "chat"), ("Tell me a joke", "chat"),
        ("Write a python function that reverses a string", "coding"), ("Explain this in Rust", "coding"),
        ("write me a program that sorts numbers", "coding"), ("How do I center a div in CSS?", "coding"),
        ("What does main.cpp do?", "coding"), ("I need a C++ example", "coding"),
        ("Open Firefox", "agents"), ("Shadow, open the terminal", "agents"), ("Can you install vlc?", "agents"),
        ("What's using my disk space?", "agents"), ("List files in my home folder", "agents"),
        ("Summarise my system info", "agents"), ("What is on my screen?", "agents"),
        ("How much RAM do I have?", "agents"), ("make a folder called work", "agents"),
    ]
    b = Behaviour()
    b.tool_trigger = r"\bnever matches\b"
    r = Run(suite, "classify", b)
    try:
        r.conf("[team]\nmode=solo\n[tools]\ndisabled=run_shell_command\n")
        res = r.ask(*[c[0] for c in cases], timeout=120)
        got = [t["kind"] for t in res.turns]
        wrong = [(text, want, have) for (text, want), have in zip(cases, got) if want != have]
        check(len(got) == len(cases) and not wrong, f"wrong team for: {wrong} ({len(got)} turns)")
        lead_of = {"chat": MISTRAL, "coding": KIMI, "agents": ULTRA}
        check([t["model"] for t in res.turns] == [lead_of[k] for _, k in cases], "the lead does not follow the kind")
    finally:
        r.close()


# ----------------------------------------------------------------------------- when things go wrong
@test
def retired_models_are_left_out(suite):
    """Models NVIDIA no longer lists are neither asked nor chosen as lead."""
    b = Behaviour()
    b.models = [m for m in b.models if m not in (KIMI, MINIMAX, GEMMA)]
    r = Run(suite, "retired", b)
    try:
        d = r.ask(CODE_Q).done
        check(d["error"] == "" and d["model"] == ULTRA, f"lead: {d['model']} {d['error']}")
        asked = {c["model"] for c in r.chats()}
        check(not asked & {KIMI, MINIMAX, GEMMA}, f"asked a retired model: {asked & {KIMI, MINIMAX, GEMMA}}")
        check(names(a["model"] for a in r.advisors()) == names([SUPER_VL, LAGUNA]), f"{names(a['model'] for a in r.advisors())}")
        team = r.dump("team")
        coding = next(g for g in team["groups"] if g["kind"] == "coding")
        avail = {m["id"]: m["available"] for m in coding["members"]}
        check(avail[KIMI] is False and avail[ULTRA] is True and coding["lead"] == short(ULTRA), f"{coding}")
    finally:
        r.close()


@test
def failing_advisors_do_not_matter(suite):
    """Advisors that fail (server error, not offered, no credit) are left out; the answer still comes."""
    b = Behaviour()
    b.errors = {BOLT: (500, {"status": 500, "title": "Internal Server Error"}),
                STEP: (404, {"status": 404, "title": "Not Found"}),
                GLM: (402, {"status": 402, "title": "Payment Required"})}
    r = Run(suite, "adv_fail", b)
    try:
        res = r.ask(CHAT_Q)
        d = res.done
        check(res.rc == 0 and d["error"] == "", f"{d}")
        check(sorted(r.leads()[0]["notes"]) == names([SUPER_VL, MUSE, LAGUNA, ULTRA]), f"{r.leads()[0]['notes']}")
        check([res.state(m) for m in (BOLT, STEP, GLM)] == ["failed"] * 3 and res.state(MUSE) == "done", f"{d['team']}")
        # One try each: an advisor is not worth retrying.
        check(all(sum(1 for c in r.chats() if c["model"] == m) == 1 for m in (BOLT, STEP, GLM)), "a failed advisor was asked again")
    finally:
        r.close()


@test
def slow_advisor_is_left_out(suite):
    """The lead does not wait for ever: after the wait a slow model is dropped."""
    b = Behaviour()
    b.delays = {MUSE: 12}
    r = Run(suite, "adv_slow", b)
    try:
        r.conf("[team]\nadvisor_seconds=3\n")
        res = r.ask(CHAT_Q)
        d = res.done
        check(d["error"] == "" and 2.8 <= d["t"] < 7, f"took {d['t']} s: {d['error']}")
        check(res.state(MUSE) == "late", f"{d['team']}")
        check(short(MUSE) not in r.leads()[0]["notes"] and len(r.leads()[0]["notes"]) == 6, f"{r.leads()[0]['notes']}")
    finally:
        r.close()


@test
def all_advisors_fail(suite):
    """Even with nobody else answering, the lead answers alone."""
    b = Behaviour()
    b.errors = {m: (500, {"status": 500, "title": "Internal Server Error"}) for m in CHAT[1:]}
    r = Run(suite, "adv_all_fail", b)
    try:
        d = r.ask(CHAT_Q).done
        check(d["error"] == "" and d["reply"].startswith("LEAD mistral-large-4-1t-a52b | notes:  |"), f"{d}")
    finally:
        r.close()


@test
def lead_that_cannot_use_tools(suite):
    """A lead that rejects tools hands over to the next one, and is remembered."""
    b = Behaviour()
    b.no_tools = {ULTRA}
    r = Run(suite, "lead_no_tools", b)
    try:
        res = r.ask(TOOL_Q)
        d = res.done
        check(d["error"] == "" and d["model"] == KIMI and "team-ok" in d["reply"], f"{d['model']} {d['error']} {d['reply']!r}")
        leads = [l["model"] for l in r.leads()]
        check(leads == [ULTRA, KIMI, KIMI], f"lead requests: {leads}")
        # The notes went to the second lead as well, and the advisors were not asked again.
        check(len(r.leads()[1]["notes"]) >= 6 and len(r.advisors()) == len(AGENTS) - 1, f"{r.leads()[1]['notes']}")
        with open(r.cache_path) as f:
            cache = json.load(f)
        check(ULTRA in json.dumps(cache.get("noTools", cache)), f"not remembered: {cache.keys()}")
        # Next time it is not tried as lead at all.
        r.mock.clear()
        d2 = r.ask(TOOL_Q).done
        check(d2["model"] == KIMI and [l["model"] for l in r.leads()] == [KIMI, KIMI], f"{[l['model'] for l in r.leads()]}")
        check(ULTRA in {a["model"] for a in r.advisors()}, "a model without tools can still advise")
    finally:
        r.close()


@test
def not_found_is_not_no_tools(suite):
    """NVIDIA's 404 for a model the account lacks mentions "function": that says nothing about tools."""
    b = Behaviour()
    b.errors = {ULTRA: (404, {"status": 404, "title": "Not Found",
                              "detail": "Function 'nvidia/nemotron-3-ultra-550b-a55b': Not found for account 'x'"})}
    r = Run(suite, "notfound_function", b)
    try:
        d = r.ask(TOOL_Q).done
        check(d["error"] == "" and d["model"] == KIMI and "team-ok" in d["reply"], f"{d['model']} {d['error']}")
        cache = {}
        if os.path.exists(r.cache_path):
            with open(r.cache_path) as f:
                cache = json.load(f)
        check(not cache.get("noTools"), f"learnt from a 404: {cache.get('noTools')}")
    finally:
        r.close()


@test
def complaint_about_the_conversation_is_not_learnt(suite):
    """An error on a request that carries earlier tool results is not held against the model's tools."""
    b = Behaviour()
    b.tool_history_errors = {ULTRA: (400, {"error": {"message": "Unexpected role 'tool' after tool_calls in messages."}})}
    r = Run(suite, "tool_history", b)
    try:
        d = r.ask(TOOL_Q).done
        # The second round fails on the first lead; the next lead finishes the turn.
        check(d["error"] == "" and d["model"] == KIMI and "team-ok" in d["reply"], f"{d['model']} {d['error']} {d['reply']!r}")
        check(len(d["tools"]) == 1 and d["tools"][0]["status"] == "done", f"the command ran {len(d['tools'])} times")
        with open(r.cache_path) as f:
            cache = json.load(f)
        check(not cache.get("noTools"), f"learnt: {cache.get('noTools')}")
    finally:
        r.close()


@test
def learnt_limits_do_not_disable_a_team(suite):
    """Every lead marked "no tools" (a wrong conclusion): the request is still answered; old marks expire; Read again forgets."""
    r = Run(suite, "poisoned")
    try:
        def seed(when):
            os.makedirs(os.path.dirname(r.cache_path), exist_ok=True)
            with open(r.cache_path, "w") as f:
                json.dump({"baseUrl": r.mock.url, "ids": r.mock.behaviour.models,
                           "noTools": {m: when for m in CHAT}, "noImages": {}}, f)

        seed(time.time())
        d = r.ask(CHAT_Q).done
        check(d["error"] == "" and d["reply"].startswith("LEAD "), f"all leads marked: {d}")
        # A mark older than a week is forgotten: the preferred lead is back.
        seed(time.time() - 8 * 24 * 3600)
        d = r.ask(CHAT_Q).done
        check(d["model"] == MISTRAL, f"after the marks expired: {d['model']}")
        # Marks made at another endpoint do not apply here.
        os.makedirs(os.path.dirname(r.cache_path), exist_ok=True)
        with open(r.cache_path, "w") as f:
            json.dump({"baseUrl": "http://127.0.0.1:1/v1", "ids": [], "noTools": {MISTRAL: time.time()}}, f)
        d = r.ask(CHAT_Q).done
        check(d["model"] == MISTRAL, f"marks of another endpoint: {d['model']}")
    finally:
        r.close()


@test
def several_actions_in_one_reply(suite):
    """Two tool calls in one reply stay two: streamed with numbers, streamed without, and in a one-piece reply."""
    for shape in ("numbered", "unnumbered", "one piece"):
        b = Behaviour()
        if shape == "unnumbered":
            b.no_index = {ULTRA}
        if shape == "one piece":
            b.one_piece = {ULTRA}
        r = Run(suite, "two_calls", b)
        try:
            one, two = os.path.join(r.dir, "one.txt"), os.path.join(r.dir, "two.txt")
            b.tool_calls = [("run_shell_command", {"command": "sh", "args": ["-c", f"echo 1 >> {one}"]}),
                            ("run_shell_command", {"command": "sh", "args": ["-c", f"echo 2 >> {two}"]})]
            d = r.ask(TOOL_Q).done
            check([t["status"] for t in d["tools"]] == ["done", "done"], f"{shape}: {d['tools']} {d['error']}")
            for path in (one, two):
                with open(path) as f:
                    check(len(f.read().split()) == 1, f"{shape}: {path} was not written exactly once")
            check(r.leads()[-1]["roles"][-3:] == ["assistant", "tool", "tool"], f"{shape}: {r.leads()[-1]['roles']}")
        finally:
            r.close()


@test
def earlier_turn_cannot_start_a_request_in_this_one(suite):
    """Stop during the pause before a retry, then a new message: the old retry does not fire inside the new turn."""
    b = Behaviour()
    b.default_delay = 1.5
    b.fail_first = {MISTRAL: 1}
    r = Run(suite, "stale_retry", b)
    try:
        # Turn 1: the team is ready at 1.5 s, the lead fails at once (500), its
        # retry is due at 3.0 s. Stop at 2.0 s. Turn 2 starts and gathers
        # until about 3.6 s: the old retry would fire in the middle of it.
        res = r.ask(CHAT_Q, "And of Italy?", args=["--ask-stop-after", "2.0"], timeout=40)
        check(res.turns[0]["reply"] == "" and res.turns[1]["error"] == "", f"{[(t['reply'][:20], t['error']) for t in res.turns]}")
        second = [q for q in r.chats() if "Italy" in str(q["last_user"])]
        adv = [q for q in second if q["advisor"]]
        lead = [q for q in second if not q["advisor"]]
        check(len(lead) == 1, f"{len(lead)} lead requests in the second turn")
        check(lead[0]["start"] >= max(a["end"] for a in adv) - 0.05 and len(lead[0]["notes"]) == 7,
              f"the lead started early or without notes: {lead[0]['notes']}")
        check(res.turns[1]["reply"].startswith("LEAD "), f"{res.turns[1]['reply']!r}")
    finally:
        r.close()


@test
def stop_then_send_is_a_clean_conversation(suite):
    """Stop before any reply, then a new message: the lead gets one user turn, not two in a row."""
    b = Behaviour()
    b.default_delay = 1.5
    r = Run(suite, "tidy", b)
    try:
        res = r.ask(CHAT_Q, "And of Italy?", args=["--ask-stop-after", "0.5"], timeout=40)
        check(res.turns[1]["error"] == "", f"{res.turns[1]}")
        lead = [q for q in r.leads() if "Italy" in str(q["last_user"])]
        check(len(lead) == 1 and lead[0]["roles"] == ["system", "user"], f"roles: {[q['roles'] for q in lead]}")
        check("France" in lead[0]["last_user"] and "Italy" in lead[0]["last_user"], f"{lead[0]['last_user']!r}")
    finally:
        r.close()


@test
def service_unreachable(suite):
    """No connection: one message after the retries, not a tour of every lead."""
    r = Run(suite, "offline", service=False)
    try:
        port = free_port()
        res = r.ask(CHAT_Q, base=f"http://127.0.0.1:{port}/v1", timeout=40,
                    extra={"SHADOW_ASSISTANT_NO_MODEL_REFRESH": "1"})
        d = res.done
        check(res.rc == 1 and d["reply"] == "" and d["error"] != "", f"{d}")
        check(d["t"] < 16, f"took {d['t']} s to give up")
    finally:
        r.close()


@test
def lead_not_offered(suite):
    """A lead the key cannot use (404, 403) hands over to the next one."""
    for status in (404, 403):
        b = Behaviour()
        b.errors = {MISTRAL: (status, {"status": status, "title": "Not Found" if status == 404 else "Forbidden"})}
        r = Run(suite, f"lead_{status}", b)
        try:
            d = r.ask(CHAT_Q).done
            check(d["error"] == "" and d["model"] == ULTRA, f"HTTP {status}: {d['model']} {d['error']}")
            check(d["reply"].startswith("LEAD nemotron-3-ultra-550b-a55b"), f"{d['reply']!r}")
        finally:
            r.close()


@test
def every_lead_fails(suite):
    """When no lead works the user gets one clear error, after at most four tries."""
    b = Behaviour()
    b.errors = {m: (404, {"status": 404, "title": "Not Found"}) for m in CHAT}
    r = Run(suite, "lead_all_fail", b)
    try:
        res = r.ask(CHAT_Q)
        d = res.done
        check(res.rc == 1 and d["reply"] == "" and "404" in d["error"], f"{d}")
        check(len(r.leads()) == 4, f"{len(r.leads())} lead requests")
    finally:
        r.close()


@test
def wrong_key(suite):
    """A key NVIDIA rejects: one clear message, no parade of fallbacks."""
    r = Run(suite, "wrong_key")
    try:
        res = r.ask(CHAT_Q, key="nvapi-test-wrong")
        d = res.done
        check(res.rc == 1 and d["reply"] == "", f"{d}")
        check("403" in d["error"] and "did not accept the API key" in d["error"] and "Settings" in d["error"], f"error: {d['error']!r}")
        check(len(r.leads()) == 1, f"{len(r.leads())} lead requests after a rejected key")
        check("nvapi-test-wrong" not in res.stdout + res.stderr, "the key was printed")
    finally:
        r.close()


@test
def lead_takes_shorter_replies_only(suite):
    """A lead that rejects the reply length asked for is asked again for less, and that is remembered."""
    b = Behaviour()
    b.max_tokens = {MISTRAL: 2048}
    r = Run(suite, "reply_limit", b)
    try:
        res = r.ask(CHAT_Q, "And of Italy?")
        check(all(t["error"] == "" and t["model"] == MISTRAL for t in res.turns), f"{[(t['model'], t['error']) for t in res.turns]}")
        asked = [(l["max_tokens"], l["status"]) for l in r.leads()]
        check(asked == [(8192, 400), (4096, 400), (2048, 200), (2048, 200)], f"lead requests: {asked}")
    finally:
        r.close()


@test
def lead_forbidden_for_this_key(suite):
    """403 without "authorization" wording is about the model, not the key: the next lead takes over."""
    b = Behaviour()
    b.errors = {MISTRAL: (403, {"status": 403, "title": "Forbidden", "detail": "Model access requires an entitlement"})}
    r = Run(suite, "lead_entitlement", b)
    try:
        d = r.ask(CHAT_Q).done
        check(d["error"] == "" and d["model"] == ULTRA, f"{d['model']} {d['error']}")
    finally:
        r.close()


@test
def no_key(suite):
    """No key anywhere: nothing is sent."""
    r = Run(suite, "no_key")
    try:
        res = r.ask(CHAT_Q, key="")
        check(res.rc == 1 and "no API key" in res.done["error"], f"{res.done}")
        check(r.chats() == [], "a request was sent without a key")
        c = r.dump("config", key="")
        check(c["apiKeyConfigured"] is False and c["provider"] == "nvidia", f"{c['apiKeyConfigured']} {c['provider']}")
    finally:
        r.close()


@test
def model_list_unreachable(suite):
    """Without the model list (first start, service trouble) every configured model is tried."""
    b = Behaviour()
    b.list_status = 500
    r = Run(suite, "no_list", b)
    try:
        d = r.ask(CHAT_Q).done
        check(d["error"] == "" and d["model"] == MISTRAL and len(r.advisors()) == 7, f"{d['model']} {d['error']} {len(r.advisors())}")
    finally:
        r.close()


@test
def rate_limit_shrinks_the_team(suite):
    """After "too many requests" the next message goes to a small team."""
    b = Behaviour()
    b.rate_limited = {BOLT, STEP}
    r = Run(suite, "rate_limit", b)
    try:
        res = r.ask(CHAT_Q, "And of Italy?")
        check(all(t["error"] == "" for t in res.turns), f"{[t['error'] for t in res.turns]}")
        first = [c for c in r.advisors() if c["last_user"] == CHAT_Q]
        second = [c for c in r.advisors() if c["last_user"] == "And of Italy?"]
        check(len(first) == 7 and len(second) == 2, f"{len(first)} then {len(second)} advisors")
    finally:
        r.close()


@test
def stream_shapes(suite):
    """Reasoning text is not shown as the answer; a reply in one piece is read too."""
    b = Behaviour()
    b.reasoning = {MISTRAL, BOLT}
    r = Run(suite, "reasoning", b)
    try:
        d = r.ask(CHAT_Q).done
        check("Let me think" not in d["reply"] and d["reply"].startswith("LEAD "), f"{d['reply']!r}")
        check(short(BOLT) in r.leads()[0]["notes"], "an advisor that thinks first lost its note")
    finally:
        r.close()
    b = Behaviour()
    b.one_piece = {MISTRAL, GLM}
    r = Run(suite, "one_piece", b)
    try:
        d = r.ask(CHAT_Q).done
        check(d["error"] == "" and d["reply"].startswith("LEAD mistral-large-4-1t-a52b"), f"{d}")
        check(short(GLM) in r.leads()[0]["notes"], f"{r.leads()[0]['notes']}")
    finally:
        r.close()
    b = Behaviour()
    b.one_piece = {ULTRA}
    r = Run(suite, "one_piece_tool", b)
    try:
        d = r.ask(TOOL_Q).done
        check(d["tools"] and d["tools"][0]["status"] == "done" and "team-ok" in d["reply"], f"{d}")
    finally:
        r.close()


@test
def stop_ends_the_whole_team(suite):
    """Stop while the others write, or while the lead answers: everything ends, nothing is said, and the next message works."""
    for delay, stop_at, expect_lead in ((3.0, 1.0, False), (1.0, 1.5, True)):
        b = Behaviour()
        b.default_delay = delay
        r = Run(suite, "stop", b)
        try:
            res = r.ask(CHAT_Q, args=["--ask-stop-after", str(stop_at)])
            d = res.done
            check(res.rc == 0 and d["reply"] == "" and d["error"] == "", f"after Stop at {stop_at} s: {d}")
            check(d["t"] < stop_at + 1.0, f"Stop at {stop_at} s took until {d['t']} s")
            time.sleep(delay + 0.6)  # anything still running would show up at the service by now
            leads = r.leads()
            check((len(leads) == 1) == expect_lead and len(leads) <= 1, f"lead requests after Stop at {stop_at} s: {len(leads)}")
            check(not any(e["event"] == "team" and e["t"] > stop_at + 0.3 and any(m["state"] == "working" for m in e["members"])
                          for e in res.events), "a model was still shown as working after Stop")
            # The conversation is usable afterwards.
            b.default_delay = 0
            d2 = r.ask(CHAT_Q).done
            check(d2["error"] == "" and d2["reply"].startswith("LEAD "), f"after a Stop, the next message: {d2}")
        finally:
            r.close()


@test
def closed_while_the_team_works(suite):
    """Closing the app in mid-request (advisors, then the lead, still in flight) does not crash it."""
    for delay, wait in ((6, 1.5), (1.2, 2.0)):  # advisors in flight; then the lead in flight
        b = Behaviour()
        b.default_delay = delay
        r = Run(suite, "closed", b)
        try:
            res = r.ask(CHAT_Q, timeout=wait)
            check(res.rc == 1 and "timed out" in res.done["error"], f"exit {res.rc}: {res.done}")
            check("Segmentation" not in res.stderr, res.stderr[-400:])
        finally:
            r.close()


# ----------------------------------------------------------------------------- seeing
@test
def looks_at_a_picture(suite):
    """look_at_image: four vision models get the picture at once, small enough to send."""
    b = Behaviour()
    b.tool_trigger = r"\blook at\b"
    b.default_delay = 0.3
    r = Run(suite, "vision", b)
    try:
        picture = os.path.join(r.dir, "big.png")
        png(picture, 2400, 1500, noise=True)  # ~11 MB of noise
        b.tool_call = ("look_at_image", {"path": picture, "question": "What is in it?"})
        res = r.ask("Please look at the picture big.png")
        d = res.done
        check(d["error"] == "" and d["tools"] and d["tools"][0]["name"] == "look_at_image" and d["tools"][0]["status"] == "done",
              f"{d['tools']} {d['error']}")
        looks = [c for c in r.chats() if c["images"]]
        check(names(c["model"] for c in looks) == names(VISION_FIRST4), f"{names(c['model'] for c in looks)}")
        check(max(c["start"] for c in looks) < min(c["end"] for c in looks), "the vision models were not asked at the same time")
        for c in looks:
            check(c["image_mime"] == "image/jpeg (jpeg data)" and 2000 < c["image_bytes"] <= 180000,
                  f"{c['model']}: {c['image_mime']} {c['image_bytes']} bytes")
            check(c["last_user"] == "What is in it?" and c["tools"] == [], f"{c['last_user']!r} {c['tools']}")
        seen = r.leads()[-1]["tool_result"]
        check("4 vision models" in seen and all(f"[{short(m)}]\n{short(m)} sees an image" in seen for m in VISION_FIRST4),
              f"what the lead was told: {seen!r}")
        # The picture is not in the conversation the lead gets back, only what was seen.
        check(all(l["images"] == 0 for l in r.leads()), "the picture went to the lead as well")
    finally:
        r.close()


@test
def picture_problems(suite):
    """A missing file, a model that takes no pictures, and "no" to sending the picture."""
    b = Behaviour()
    b.tool_trigger = r"\blook at\b"
    r = Run(suite, "vision_missing", b)
    try:
        b.tool_call = ("look_at_image", {"path": os.path.join(r.dir, "nothing.png")})
        d = r.ask("Please look at the picture").done
        check(d["tools"][0]["status"] in ("failed", "error") and not [c for c in r.chats() if c["images"]], f"{d['tools']}")
        check("no file" in d["reply"].lower(), f"{d['reply']!r}")
    finally:
        r.close()

    b = Behaviour()
    b.tool_trigger = r"\blook at\b"
    b.no_images = {SUPER_VL, KIMI}
    r = Run(suite, "vision_no_images", b)
    try:
        picture = os.path.join(r.dir, "small.png")
        png(picture, 320, 200)
        b.tool_call = ("look_at_image", {"path": picture})
        d = r.ask("Please look at the picture").done
        check(d["tools"][0]["status"] == "done" and "2 vision models" in d["reply"], f"{d['tools']} {d['reply']!r}")
        # Remembered: next time they are not sent the picture, and others take their place.
        r.mock.clear()
        r.ask("Please look at the picture again")
        looks = {c["model"] for c in r.chats() if c["images"]}
        check(not looks & {SUPER_VL, KIMI} and len(looks) == 4, f"{looks}")
    finally:
        r.close()

    b = Behaviour()
    b.tool_trigger = r"\blook at\b"
    r = Run(suite, "vision_denied", b)
    try:
        picture = os.path.join(r.dir, "small.png")
        png(picture, 320, 200)
        b.tool_call = ("look_at_image", {"path": picture})
        r.conf("[behaviour]\nautonomy=ask_risky\n")
        res = r.ask("Please look at the picture", args=["--ask-approve", "no"])
        check(any(e["event"] == "confirm" for e in res.events), "sending a picture was not asked about")
        check(not [c for c in r.chats() if c["images"]], "the picture was sent after a no")
    finally:
        r.close()


@test
def two_pictures_in_one_reply(suite):
    """The lead asks for two pictures at once: both are looked at, one after the other, and the turn ends."""
    b = Behaviour()
    b.tool_trigger = r"\bcompare\b"
    b.default_delay = 0.2
    r = Run(suite, "vision_two", b)
    try:
        a, c = os.path.join(r.dir, "a.png"), os.path.join(r.dir, "b.png")
        png(a, 320, 200)
        png(c, 200, 320)
        b.tool_calls = [("look_at_image", {"path": a, "question": "first"}), ("look_at_image", {"path": c, "question": "second"})]
        res = r.ask("Please compare the two pictures", timeout=40)
        d = res.done
        check(res.rc == 0 and d["error"] == "", f"{d}")
        check([t["status"] for t in d["tools"]] == ["done", "done"], f"{d['tools']}")
        looks = [q for q in r.chats() if q["images"]]
        first = [q for q in looks if q["last_user"] == "first"]
        second = [q for q in looks if q["last_user"] == "second"]
        check(len(first) == 4 and len(second) == 4, f"{len(first)} + {len(second)} picture requests")
        check(min(q["start"] for q in second) >= max(q["end"] for q in first) - 0.05, "the second picture did not wait for the first")
        check(d["reply"].startswith("LEAD ") and "DONE after tool" in d["reply"], f"{d['reply']!r}")
    finally:
        r.close()


@test
def stop_while_looking(suite):
    """Stop while the vision models look: the turn ends at once and the next message works."""
    b = Behaviour()
    b.tool_trigger = r"\blook at\b"
    r = Run(suite, "vision_stop", b)
    try:
        picture = os.path.join(r.dir, "small.png")
        png(picture, 320, 200)
        b.tool_call = ("look_at_image", {"path": picture})
        b.delays = {m: 5 for m in VISION_FIRST4}  # the lookers are slow; the teams below do not include them
        r.conf("[team]\nagents=" + ", ".join([ULTRA, BOLT, LAGUNA]) + "\nchat=" + ", ".join([MISTRAL, BOLT, LAGUNA]) + "\n")
        res = r.ask("Please look at the picture", "And now? " + CHAT_Q, args=["--ask-stop-after", "1.5"], timeout=40)
        first, second = res.turns[0], res.turns[1]
        check(first["tools"] and first["tools"][0]["status"] == "error" and "Stopped" in json.dumps(res.events), f"{first}")
        check(first["t"] < 3.0, f"Stop took until {first['t']} s")
        check(second["error"] == "" and second["reply"].startswith("LEAD "), f"the next message: {second}")
    finally:
        r.close()


@test
def no_vision_models_no_tool(suite):
    """Without a model that can see, the tool is not offered at all."""
    b = Behaviour()
    vision = {SUPER_VL, KIMI, STEP, GLM, MISTRAL, MUSE, "nvidia/nemotron-3-nano-omni-30b-a3b-reasoning",
              "nvidia/cosmos-reason2-8b", "nvidia/ising-calibration-1.5-31b"}
    b.models = [m for m in b.models if m not in vision]
    r = Run(suite, "no_vision", b)
    try:
        r.ask(TOOL_Q)
        lead_tools = r.leads()[0]["tools"]
        check("run_shell_command" in lead_tools and "look_at_image" not in lead_tools, f"{lead_tools}")
    finally:
        r.close()
    r = Run(suite, "with_vision")
    try:
        r.ask(TOOL_Q)
        check("look_at_image" in r.leads()[0]["tools"], f"{r.leads()[0]['tools']}")
    finally:
        r.close()


# ----------------------------------------------------------------------------- the key
@test
def key_saved_from_settings(suite):
    """--set-api-key (what Settings does): an owner-only file, used at once, never printed."""
    b = Behaviour()
    b.key = SAVED_KEY
    r = Run(suite, "key_store", b)
    try:
        p = r.run(["--set-api-key"], r.env(key=""), stdin=SAVED_KEY + "\n")
        check(p.returncode == 0, f"--set-api-key failed: {p.stderr}")
        out = json.loads(p.stdout)
        check(out["saved"] is True and out["inEffect"] is True and out["provider"] == "nvidia", f"{out}")
        check(SAVED_KEY not in p.stdout + p.stderr, "the key was printed")
        st = os.stat(r.key_path)
        check(stat.S_IMODE(st.st_mode) == 0o600, f"mode {oct(stat.S_IMODE(st.st_mode))}")
        with open(r.key_path) as f:
            check(f.read().strip() == SAVED_KEY, "the file does not hold the key")
        c = r.run(["--dump", "config"], r.env(key=""))
        cfg = json.loads(c.stdout)
        check(cfg["apiKeyConfigured"] and cfg["apiKeySource"] == "Settings" and cfg["apiKeySaved"] and cfg["apiKeyHint"] == "1234",
              f"{cfg['apiKeyConfigured']} {cfg['apiKeySource']} {cfg['apiKeyHint']}")
        check(SAVED_KEY not in c.stdout + c.stderr, "the key is in the config dump")
        # It is the key that goes out.
        res = r.ask(CHAT_Q, key="")
        check(res.done["error"] == "" and all(x["authorised"] for x in r.chats()), f"{res.done['error']}")
        check(SAVED_KEY not in res.stdout + res.stderr, "the key was printed by --ask")
        # A file someone made readable for others is corrected.
        os.chmod(r.key_path, 0o644)
        r.dump("config", key="")
        check(stat.S_IMODE(os.stat(r.key_path).st_mode) == 0o600, "permissions were not corrected")
        # Remove.
        p = r.run(["--remove-api-key"], r.env(key=""))
        check(p.returncode == 0 and not os.path.exists(r.key_path), "the key file is still there")
        check(json.loads(p.stdout)["apiKeyConfigured"] is False, p.stdout)
        # Nothing else in the home directory holds the key.
        for root, _, files in os.walk(r.home):
            for name in files:
                with open(os.path.join(root, name), "rb") as f:
                    check(SAVED_KEY.encode() not in f.read(), f"the key is in {os.path.join(root, name)}")
    finally:
        r.close()


@test
def key_must_look_like_a_key(suite):
    """Empty, too short, with a space, or the template's example: refused, nothing written."""
    r = Run(suite, "key_bad", service=False)
    try:
        for bad in ("", "short", "nvapi-with a space-00000000000000000", "nvapi-XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"):
            p = r.run(["--set-api-key"], r.env(key=""), stdin=bad + "\n")
            check(p.returncode == 1 and p.stderr.strip() != "" and not os.path.exists(r.key_path), f"{bad!r}: exit {p.returncode}")
    finally:
        r.close()


@test
def key_precedence(suite):
    """Environment, then the key saved from Settings, then the conf file."""
    r = Run(suite, "key_order", service=False)
    try:
        def source(**env):
            c = json.loads(r.run(["--dump", "config"], r.env(key="", extra=env)).stdout)
            return c["apiKeySource"], c["apiKeyHint"], c["provider"]

        r.conf(f"[api]\nkey={CONF_KEY}\n")
        check(source() == (r.conf_path, "5678", "nvidia"), f"conf only: {source()}")
        with open(r.key_path, "w") as f:
            f.write(SAVED_KEY + "\n")
        os.chmod(r.key_path, 0o600)
        check(source() == ("Settings", "1234", "nvidia"), f"saved + conf: {source()}")
        got = source(NVIDIA_API_KEY="nvapi-test-env-nvidia-00000000000000aaaa")
        check(got[0].startswith("NVIDIA_API_KEY") and got[1] == "aaaa", f"NVIDIA_API_KEY: {got}")
        got = source(NVIDIA_API_KEY="nvapi-test-env-nvidia-00000000000000aaaa",
                     SHADOW_ASSISTANT_API_KEY="nvapi-test-env-shadow-00000000000000bbbb")
        check(got[0].startswith("SHADOW_ASSISTANT_API_KEY") and got[1] == "bbbb", f"both env: {got}")
        # ANTHROPIC_API_KEY is the last resort, and switches the service.
        os.remove(r.key_path)
        r.conf("")
        got = source(ANTHROPIC_API_KEY="sk-ant-test-0000000000000000cccc")
        check(got[0].startswith("ANTHROPIC_API_KEY") and got[2] == "anthropic", f"anthropic env: {got}")
        # The template's example key is not a key.
        template = r.dump("config", key="")["configTemplate"]
        r.conf(template)
        c = r.dump("config", key="")
        check(c["apiKeyConfigured"] is False, "the template's example key counts as a key")
        check("nvapi-XXXX" in template and "[team]" in template, "the template is not NVIDIA's")
    finally:
        r.close()


@test
def which_service(suite):
    """nvapi- keys go to NVIDIA, sk-ant- keys to Claude; either can be forced."""
    r = Run(suite, "provider", service=False)
    try:
        def cfg(key, **env):
            return json.loads(r.run(["--dump", "config"], r.env(key=key, base="", extra=env)).stdout)

        c = cfg("nvapi-test")
        check(c["provider"] == "nvidia" and c["baseUrl"] == "https://integrate.api.nvidia.com/v1", f"{c['provider']} {c['baseUrl']}")
        c = cfg("sk-ant-test")
        check(c["provider"] == "anthropic" and "anthropic.com" in c["baseUrl"], f"{c['provider']} {c['baseUrl']}")
        c = cfg("some-other-key-0000")
        check(c["provider"] == "nvidia", f"unknown key shape: {c['provider']}")
        c = cfg("some-other-key-0000", SHADOW_ASSISTANT_PROVIDER="anthropic")
        check(c["provider"] == "anthropic" and c["apiKeyConfigured"], f"forced by the environment: {c['provider']}")
        r.conf("[api]\nprovider=anthropic\n")
        check(cfg("some-other-key-0000")["provider"] == "anthropic", "forced by the conf file")

        # A key is only ever used with its own service. With the service named,
        # a key that is plainly the other one's is passed over.
        c = cfg("nvapi-test", SHADOW_ASSISTANT_PROVIDER="anthropic")
        check(c["provider"] == "anthropic" and c["apiKeyConfigured"] is False, f"nvapi key with Claude forced: {c['apiKeyConfigured']}")
        c = cfg("", SHADOW_ASSISTANT_PROVIDER="anthropic", NVIDIA_API_KEY="nvapi-test-env-000000000000000000")
        check(c["provider"] == "anthropic" and c["apiKeyConfigured"] is False, f"NVIDIA_API_KEY with Claude forced: {c['apiKeyConfigured']}")
        c = cfg("", SHADOW_ASSISTANT_PROVIDER="nvidia", ANTHROPIC_API_KEY="sk-ant-test-000000000000000000")
        check(c["provider"] == "nvidia" and c["apiKeyConfigured"] is False, f"ANTHROPIC_API_KEY with NVIDIA forced: {c['apiKeyConfigured']}")
        c = cfg("sk-ant-test", SHADOW_ASSISTANT_PROVIDER="nvidia", NVIDIA_API_KEY="nvapi-test-env-000000000000000000")
        check(c["apiKeyConfigured"] and c["apiKeySource"].startswith("NVIDIA_API_KEY"), f"the next fitting key is used: {c['apiKeySource']}")
        # The variable's name says whose key it is, whatever it looks like.
        r.conf("")
        c = cfg("", ANTHROPIC_API_KEY="gateway-token-000000000000")
        check(c["provider"] == "anthropic" and "anthropic.com" in c["baseUrl"], f"ANTHROPIC_API_KEY of another shape: {c['provider']} {c['baseUrl']}")

        # Endpoints are per service: [api] base_url is Anthropic's (it predates
        # NVIDIA), nvidia_base_url NVIDIA's. An NVIDIA key never goes to an
        # endpoint written down for the other service.
        r.conf("[api]\nbase_url=https://api.anthropic.com/v1\n")
        c = cfg("nvapi-test")
        check(c["provider"] == "nvidia" and c["baseUrl"] == "https://integrate.api.nvidia.com/v1", f"{c['provider']} {c['baseUrl']}")
        r.conf("[api]\nbase_url=https://gateway.example.net/v1\n")
        c = cfg("nvapi-test")
        check(c["provider"] == "nvidia" and c["baseUrl"] == "https://integrate.api.nvidia.com/v1", f"nvapi key with a custom base_url: {c['baseUrl']}")
        # ...while a key of unknown shape with that conf is the Anthropic-protocol setup it always was.
        c = cfg("gateway-token-000000000000")
        check(c["provider"] == "anthropic" and c["baseUrl"] == "https://gateway.example.net/v1", f"{c['provider']} {c['baseUrl']}")
        r.conf("[api]\nnvidia_base_url=https://nim.example.net/v1\nbase_url=https://gateway.example.net/v1\n")
        c = cfg("nvapi-test")
        check(c["baseUrl"] == "https://nim.example.net/v1", f"nvidia_base_url: {c['baseUrl']}")
        c = cfg("sk-ant-test")
        check(c["baseUrl"] == "https://gateway.example.net/v1", f"base_url with a Claude key: {c['baseUrl']}")
        r.conf("")
        team = json.loads(r.run(["--dump", "team"], r.env(key="nvapi-test", base="",
                                                          extra={"SHADOW_ASSISTANT_NO_MODEL_REFRESH": "1"})).stdout)
        check([g["kind"] for g in team["groups"]] == ["coding", "agents", "chat", "vision"], f"{[g['kind'] for g in team['groups']]}")
    finally:
        r.close()


@test
def key_never_goes_to_the_other_service(suite):
    """Claude forced while only an NVIDIA key exists: nothing is sent anywhere."""
    r = Run(suite, "key_cross")
    try:
        res = r.ask(CHAT_Q, key="", extra={"SHADOW_ASSISTANT_PROVIDER": "anthropic",
                                           "NVIDIA_API_KEY": "nvapi-test-env-000000000000000000"})
        check(res.rc == 1 and "no API key" in res.done["error"], f"{res.done}")
        check([q for q in r.mock.requests if q.get("auth")] == [], "a request carried the key")
    finally:
        r.close()


@test
def key_file_is_private_from_the_first_byte(suite):
    """Whatever the umask, the saved key is never readable by others, and no temporary file stays behind."""
    r = Run(suite, "key_umask", service=False)
    try:
        env = r.env(key="")
        old = os.umask(0)
        try:
            p = r.run(["--set-api-key"], env, stdin=SAVED_KEY + "\n")
        finally:
            os.umask(old)
        check(p.returncode == 0, p.stderr)
        check(stat.S_IMODE(os.stat(r.key_path).st_mode) == 0o600, oct(stat.S_IMODE(os.stat(r.key_path).st_mode)))
        check(sorted(os.listdir(r.confdir)) == ["assistant.key"], f"left behind: {os.listdir(r.confdir)}")
        # Replacing it keeps it private too.
        p = r.run(["--set-api-key"], env, stdin=CONF_KEY + "\n")
        with open(r.key_path) as f:
            check(p.returncode == 0 and f.read().strip() == CONF_KEY, "the key was not replaced")
        check(stat.S_IMODE(os.stat(r.key_path).st_mode) == 0o600 and sorted(os.listdir(r.confdir)) == ["assistant.key"], "after replacing")
    finally:
        r.close()


@test
def claude_key_still_works(suite):
    """With an sk-ant- key the assistant talks to Claude as before: one model, no team."""
    r = Run(suite, "anthropic", service=False)
    port = free_port()
    mock = subprocess.Popen([sys.executable, "-I", MOCK_ANTHROPIC, "--port", str(port), "--mode", "stream"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
                break
            except OSError:
                time.sleep(0.05)
        res = r.ask("Hello there", key="sk-ant-test", base=f"http://127.0.0.1:{port}/v1")
        d = res.done
        check(res.rc == 0 and d["provider"] == "anthropic" and d["reply"].strip() != "" and d["team"] == [], f"{d}")
        check(d["model"].startswith("claude-"), f"model: {d['model']}")
    finally:
        mock.terminate()
        mock.wait(timeout=5)
        r.close()


# ----------------------------------------------------------------------------- runner
def find_binary(explicit):
    cands = [explicit, os.environ.get("SHADOW_ASSISTANT_BIN")]
    for var in ("LD_LIBRARY_PATH", "QML_IMPORT_PATH"):
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
    ap.add_argument("--verbose", action="store_true", help="print the output of failed tests")
    ap.add_argument("--smoke", action="store_true", help="print SHADOW-SMOKE-OK when everything passed")
    ap.add_argument("tests", nargs="*")
    args = ap.parse_args()

    binary = find_binary(args.bin)
    if not binary:
        print("team_tests: shadow-assistant not found (use --bin)", file=sys.stderr)
        return 2
    chosen = [t for t in TESTS if not args.tests or t.__name__ in args.tests]
    if not chosen:
        print("team_tests: no such test; known: " + " ".join(t.__name__ for t in TESTS), file=sys.stderr)
        return 2
    work = tempfile.mkdtemp(prefix="shadow-team-tests-")
    suite = Suite(binary, work, args.verbose)
    print(f"team_tests: {binary}  ({len(chosen)} tests, work dir {work})", flush=True)
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

    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        for name, err, secs in pool.map(one, chosen):
            results[name] = err
            status = "PASS" if err is None else "FAIL"
            print(f"{status}  {name}  ({secs:.1f}s)" + ("" if err is None else f"\n      {err}"), flush=True)
    failed = [n for n, e in results.items() if e is not None]
    print(f"team_tests: {len(results) - len(failed)} passed, {len(failed)} failed in {time.time() - t0:.1f}s"
          + (f" ({' '.join(failed)})" if failed else ""), flush=True)
    if not args.keep and not failed:
        shutil.rmtree(work, ignore_errors=True)
    elif failed:
        print(f"team_tests: logs kept in {work}", flush=True)
    if args.smoke and not failed:
        print("SHADOW-SMOKE-OK team-tests", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
