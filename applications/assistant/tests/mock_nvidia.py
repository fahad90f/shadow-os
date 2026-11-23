#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A stand-in for NVIDIA's hosted model API (https://integrate.api.nvidia.com/v1),
for tests/team_tests.py and for trying the assistant without a key:

    python3 tests/mock_nvidia.py --port 8910
    SHADOW_ASSISTANT_BASE_URL=http://127.0.0.1:8910/v1 SHADOW_ASSISTANT_API_KEY=nvapi-test \\
        shadow-assistant --ask "What is the capital of France?"

It speaks the two calls the assistant makes, in the shapes the real service
uses (read from it on 2026-11-21):
  GET  /v1/models            {"object": "list", "data": [{"id": "publisher/name", ...}]}
  POST /v1/chat/completions  OpenAI-compatible; "stream": true answers as
                             Server-Sent Events ("data: {...}" chunks, then
                             "data: [DONE]"); no key is 401 "Unauthorized access",
                             a wrong key 403 {"detail": "Authorization failed"},
                             other errors a JSON body.

What a model "says" is fixed, so a test can tell who was asked what:
  * an advisor (its system prompt says it is one of several models) answers
    "NOTE-FROM-<name>: ..." — the lead's prompt must then quote that note;
  * the lead answers "LEAD <name> | notes: a,b,c | ...", naming the notes it was
    given; asked to "run a command" (and given tools) it first calls
    run_shell_command, then reports the tool's output (Behaviour.tool_trigger
    and .tool_call change what it calls and when);
  * a request with an image answers "<name> sees an image ...".
Behaviour (which models exist, who is slow, who fails, who rejects tools) is
set per test through MockNvidia.behaviour.

Every request is recorded (MockNvidia.requests, or --log FILE as JSON lines):
when it started and ended, the model, whether the key was sent, whether it
was an advisor, tools and images, and the notes found in the prompt.
"""
import argparse
import base64
import json
import re
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# The chat models NVIDIA listed on 2026-11-21 that Shadow's team uses.
DEFAULT_MODELS = [
    "google/gemma-4-31b-it",
    "meta/muse-glimmer-30b",
    "minimaxai/minimax-m3.1-flash",
    "mistralai/mistral-large-4-1t-a52b",
    "moonshotai/kimi-k3.1-pro",
    "nvidia/cosmos-reason2-8b",
    "nvidia/ising-calibration-1.5-31b",
    "nvidia/nemotron-3-embed-1b",
    "nvidia/nemotron-3-nano-omni-30b-a3b-reasoning",
    "nvidia/nemotron-3-ultra-550b-a55b",
    "nvidia/nemotron-3.5-bolt-e4b",
    "nvidia/nemotron-3.5-embed-1b",
    "nvidia/nemotron-3.5-lightning-30b-a3b",
    "nvidia/nemotron-3.5-super-vl-120b-a12b",
    "poolside/laguna-xs-2.1",
    "stepfun-ai/step-5-preview",
    "z-ai/glm-5.3-flash",
]

ADVISOR_MARK = "one of several AI models"
NOTES_MARK = "--- Team notes ---"
SKILL_MARK = "--- Skill instructions ---"
SKILL_HEADING = re.compile(r"^# (.+)$", re.M)


def short(model):
    return model.rsplit("/", 1)[-1]


class Behaviour:
    """What the service does; change the fields before (or between) requests."""

    def __init__(self):
        self.models = list(DEFAULT_MODELS)
        self.key = "nvapi-test"      # the only key accepted; None accepts any
        self.delays = {}             # model -> seconds before it answers
        self.default_delay = 0.0
        self.errors = {}             # model -> (status, body) for every request to it
        self.no_tools = set()        # models that reject a request with tools (HTTP 400)
        self.no_images = set()       # models that reject a request with an image
        self.reasoning = set()       # models that stream "reasoning_content" first
        self.one_piece = set()       # models that ignore "stream" and answer with one JSON document
        self.list_status = 200       # GET /models status
        self.list_body = None        # GET /models body, verbatim (bytes); None = built from 'models'
        # What the lead calls when the user's message matches tool_trigger and
        # the request carries tools: (tool name, arguments).
        self.tool_trigger = r"\brun (a|the|this) command\b"
        self.tool_call = ("run_shell_command", {"command": "echo", "args": ["team-ok"]})
        self.rate_limited = set()    # models that answer HTTP 429
        self.max_tokens = {}         # model -> the largest max_tokens it accepts (HTTP 400 above it)
        self.tool_calls = None       # several calls in one reply: [(tool name, arguments), ...] (instead of tool_call)
        self.no_index = set()        # models whose streamed tool calls carry no "index"
        self.tool_history_errors = {}  # model -> (status, body) for a request that carries tool results
        self.fail_first = {}         # model -> how many of its first requests answer HTTP 500


class MockNvidia:
    def __init__(self, behaviour=None, port=0, log_path=None):
        self.behaviour = behaviour or Behaviour()
        self.requests = []
        self._lock = threading.Lock()
        self._log_path = log_path
        mock = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *a):
                pass

            def do_GET(self):
                mock._get(self)

            def do_POST(self):
                mock._post(self)

        class Server(ThreadingHTTPServer):
            # A team opens a connection per model in the same instant. With the
            # default queue of 5 the kernel drops the rest, and those clients
            # only try again a second later.
            request_queue_size = 256
            daemon_threads = True

        self.server = Server(("127.0.0.1", port), Handler)
        self.port = self.server.server_address[1]
        self.thread = None

    @property
    def url(self):
        return f"http://127.0.0.1:{self.port}/v1"

    def start(self):
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        return self

    def stop(self):
        self.server.shutdown()
        self.server.server_close()

    def clear(self):
        with self._lock:
            self.requests.clear()

    def chat_requests(self):
        with self._lock:
            return [r for r in self.requests if r["path"].endswith("/chat/completions")]

    # ---- plumbing ---------------------------------------------------------------
    def _record(self, entry):
        with self._lock:
            self.requests.append(entry)
            if self._log_path:
                with open(self._log_path, "a") as f:
                    f.write(json.dumps(entry) + "\n")

    @staticmethod
    def _send(handler, status, body, ctype="application/json"):
        data = body if isinstance(body, bytes) else (
            body.encode() if isinstance(body, str) else json.dumps(body).encode())
        try:
            handler.send_response(status)
            handler.send_header("Content-Type", ctype)
            handler.send_header("Content-Length", str(len(data)))
            handler.end_headers()
            handler.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass  # the assistant gave up on this request (Stop, too slow)

    def _authorised(self, handler):
        auth = handler.headers.get("Authorization", "")
        if self.behaviour.key is None:
            return auth.startswith("Bearer ") and len(auth) > 7
        return auth == "Bearer " + self.behaviour.key

    def _get(self, handler):
        entry = {"path": handler.path, "start": time.time(), "auth": bool(handler.headers.get("Authorization"))}
        if handler.path.rstrip("/") == "/v1/models":
            b = self.behaviour
            if b.list_status != 200:
                self._send(handler, b.list_status, {"status": b.list_status, "title": "Error"})
            elif b.list_body is not None:
                self._send(handler, 200, b.list_body)
            else:
                # The real list is public: no key needed.
                self._send(handler, 200, {"object": "list", "data": [
                    {"id": m, "object": "model", "created": 735790403, "owned_by": m.split("/")[0]} for m in b.models]})
        else:
            self._send(handler, 404, {"status": 404, "title": "Not Found"})
        entry["end"] = time.time()
        self._record(entry)

    def _post(self, handler):
        length = int(handler.headers.get("Content-Length", "0"))
        raw = handler.rfile.read(length)
        entry = {"path": handler.path, "start": time.time(), "auth": bool(handler.headers.get("Authorization")),
                 "authorised": self._authorised(handler)}
        try:
            req = json.loads(raw or b"{}")
        except ValueError:
            req = {}
        b = self.behaviour
        model = req.get("model", "")
        messages = req.get("messages", [])
        system = next((m.get("content", "") for m in messages if m.get("role") == "system"), "")
        if not isinstance(system, str):
            system = ""
        users = [m for m in messages if m.get("role") == "user"]
        last_user = users[-1].get("content", "") if users else ""
        images = 0
        image_bytes = 0
        image_mime = ""
        question = last_user
        if isinstance(last_user, list):
            for part in last_user:
                if part.get("type") != "image_url":
                    continue
                images += 1
                url = part.get("image_url", {}).get("url", "")
                m = re.match(r"data:([^;]+);base64,(.*)$", url, re.S)
                if m:
                    image_mime = m.group(1)
                    try:
                        data = base64.b64decode(m.group(2), validate=True)
                        image_bytes = len(data)
                        if data[:3] == b"\xff\xd8\xff":
                            image_mime += " (jpeg data)"
                    except ValueError:
                        image_mime += " (bad base64)"
            question = " ".join(p.get("text", "") for p in last_user if p.get("type") == "text")
        tools = req.get("tools") or []
        advisor = ADVISOR_MARK in system
        skill_block = ""
        if SKILL_MARK in system:
            after = system.split(SKILL_MARK, 1)[1]
            skill_block = after.split("--- End of skill instructions ---", 1)[0]
        skills = SKILL_HEADING.findall(skill_block)
        notes = re.findall(r"^\[([^\]\n]+)\]\nNOTE-FROM", system, re.M) if NOTES_MARK in system else []
        tool_results = [m for m in messages if m.get("role") == "tool"]
        roles = [m.get("role") for m in messages]
        entry.update({"model": model, "advisor": advisor, "tools": [t.get("function", {}).get("name") for t in tools],
                      "images": images, "image_bytes": image_bytes, "image_mime": image_mime,
                      "last_user": question, "notes": notes, "stream": bool(req.get("stream")),
                      "temperature": req.get("temperature"),
                      "tool_result": str(tool_results[-1].get("content", ""))[:6000] if tool_results else "",
                      "roles": roles, "max_tokens": req.get("max_tokens"), "system_chars": len(system),
                      "skills": skills, "has_skill_block": SKILL_MARK in system})

        def finish(status):
            entry["status"] = status
            entry["end"] = time.time()
            self._record(entry)

        if not handler.path.rstrip("/").endswith("/chat/completions"):
            self._send(handler, 404, {"status": 404, "title": "Not Found"})
            return finish(404)
        if not self._authorised(handler):
            # As the real service answered on 2026-11-21: no key at all is 401
            # with a bare sentence, a key it does not know is 403 with JSON.
            if handler.headers.get("Authorization"):
                self._send(handler, 403, {"status": 403, "title": "Forbidden", "detail": "Authorization failed"})
                return finish(403)
            self._send(handler, 401, "Unauthorized access", "text/plain")
            return finish(401)
        if model not in b.models:
            self._send(handler, 404, {"status": 404, "title": "Not Found",
                                      "detail": f"Function for model '{model}' not found for account"})
            return finish(404)
        delay = b.delays.get(model, b.default_delay)
        if delay:
            time.sleep(delay)
        if model in b.rate_limited:
            self._send(handler, 429, {"status": 429, "title": "Too Many Requests"})
            return finish(429)
        if model in b.max_tokens and (req.get("max_tokens") or 0) > b.max_tokens[model]:
            self._send(handler, 400, {"error": {"message": f"max_tokens must be at most {b.max_tokens[model]}, got {req.get('max_tokens')}.",
                                                "type": "BadRequestError", "code": 400}})
            return finish(400)
        if tool_results and model in b.tool_history_errors:
            status, body = b.tool_history_errors[model]
            self._send(handler, status, body)
            return finish(status)
        with self._lock:
            failing = b.fail_first.get(model, 0)
            if failing > 0:
                b.fail_first[model] = failing - 1
        if failing > 0:
            self._send(handler, 500, {"status": 500, "title": "Internal Server Error"})
            return finish(500)
        if model in b.errors:
            status, body = b.errors[model]
            self._send(handler, status, body)
            return finish(status)
        if tools and model in b.no_tools:
            self._send(handler, 400, {"error": {"message": "This model does not support tool calling (tools, tool_choice).",
                                                "type": "BadRequestError", "code": 400}})
            return finish(400)
        if images and model in b.no_images:
            self._send(handler, 400, {"error": {"message": "This model does not accept image input.",
                                                "type": "BadRequestError", "code": 400}})
            return finish(400)

        # What the model says.
        name = short(model)
        call = None
        calls = []
        if images:
            text = f"{name} sees an image and was asked: {question.strip()[:120]}"
        elif advisor:
            kind = ("code" if "programming request" in system else
                    "steps" if "needs things done" in system else "answer")
            text = f"NOTE-FROM-{name}: my {kind} for \"{str(question).strip()[:60]}\"."
        elif tool_results:
            text = f"LEAD {name} | notes: {','.join(notes)} | DONE after tool: {str(tool_results[-1].get('content', ''))[:200]}"
        elif tools and re.search(b.tool_trigger, str(question), re.I) and all(
                c[0] in entry["tools"] for c in (b.tool_calls or [b.tool_call])):
            calls = [(name, json.dumps(arguments)) for name, arguments in (b.tool_calls or [b.tool_call])]
            call = calls[0]
            text = ""
        else:
            text = f"LEAD {name} | notes: {','.join(notes)} | reply to \"{str(question).strip()[:60]}\""

        try:
            if model in b.one_piece or not req.get("stream"):
                message = {"role": "assistant", "content": text}
                if call:
                    message["tool_calls"] = [{"id": f"call_{n + 1}", "type": "function",
                                              "function": {"name": c[0], "arguments": c[1]}} for n, c in enumerate(calls)]
                self._send(handler, 200, {"id": "cmpl-mock", "object": "chat.completion", "model": model,
                                          "choices": [{"index": 0, "message": message,
                                                       "finish_reason": "tool_calls" if call else "stop"}]})
                return finish(200)
            handler.send_response(200)
            handler.send_header("Content-Type", "text/event-stream")
            handler.send_header("Cache-Control", "no-cache")
            handler.send_header("Connection", "close")
            handler.end_headers()

            def chunk(delta, finish_reason=None):
                payload = {"id": "cmpl-mock", "object": "chat.completion.chunk", "model": model,
                           "choices": [{"index": 0, "delta": delta, "finish_reason": finish_reason}]}
                handler.wfile.write(b"data: " + json.dumps(payload).encode() + b"\n\n")
                handler.wfile.flush()

            chunk({"role": "assistant", "content": ""})
            if model in b.reasoning:
                for piece in ("Let me think. ", "The user wants an answer. "):
                    chunk({"reasoning_content": piece})
            if call:
                numbered = model not in b.no_index
                for n, c in enumerate(calls):
                    first = {"id": f"call_{n + 1}", "type": "function", "function": {"name": c[0], "arguments": ""}}
                    if numbered:
                        first["index"] = n
                    chunk({"tool_calls": [first]})
                    half = len(c[1]) // 2
                    for part in (c[1][:half], c[1][half:]):
                        rest = {"function": {"arguments": part}}
                        if numbered:
                            rest["index"] = n
                        chunk({"tool_calls": [rest]})
                chunk({}, "tool_calls")
            else:
                for piece in re.findall(r"\S+\s*", text):
                    chunk({"content": piece})
                chunk({}, "stop")
            handler.wfile.write(b"data: [DONE]\n\n")
            handler.wfile.flush()
            handler.close_connection = True
            finish(200)
        except (BrokenPipeError, ConnectionResetError):
            # The assistant gave up on this model (too slow, or Stop).
            entry["aborted"] = True
            finish(0)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int, default=8910)
    ap.add_argument("--log", help="write one JSON line per request to this file")
    ap.add_argument("--key", default="nvapi-test", help="the key to accept ('any' accepts every key)")
    ap.add_argument("--delay", type=float, default=0.0, help="seconds every model takes to answer")
    ap.add_argument("--models-file", help="a saved answer of the real GET /v1/models to serve as it is")
    args = ap.parse_args()
    behaviour = Behaviour()
    if args.models_file:
        with open(args.models_file, "rb") as f:
            behaviour.list_body = f.read()
        behaviour.models = [m["id"] for m in json.loads(behaviour.list_body)["data"]]
    behaviour.key = None if args.key == "any" else args.key
    behaviour.default_delay = args.delay
    mock = MockNvidia(behaviour, args.port, args.log)
    print(f"mock_nvidia listening on {mock.url} (key: {args.key})", flush=True)
    try:
        mock.server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    sys.exit(main())
