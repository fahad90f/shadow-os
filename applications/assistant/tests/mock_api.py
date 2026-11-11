#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Tiny mock of an OpenAI-compatible chat/completions endpoint for testing
# SHADOW Assistant offline. It speaks Server-Sent Events like NVIDIA's
# integrate.api.nvidia.com and can be told, per request, to exercise each
# branch of the client: plain streaming, a tool-call round-trip, and the
# 401 / 404 / timeout error paths.
#
#   python3 mock_api.py [--port N] [--mode MODE]
#
# The app is pointed at it with:
#   SHADOW_ASSISTANT_BASE_URL=http://127.0.0.1:N/v1
#   SHADOW_ASSISTANT_API_KEY=test-key        (any non-empty value)
#
# Behaviour is chosen by, in order: the X-Mock-Mode request header, the
# --mode command-line default, or inferred from the conversation (a user
# message containing "run" / "read file" triggers a tool call; the presence of
# a prior tool result makes the model answer normally).
#
# Modes: stream (default), tool, auth401, notfound404, timeout, ratelimit.

import argparse
import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

DEFAULT_MODE = "stream"

REPLY_TEXT = (
    "Hello! I am **Shadow Assistant** running against the local mock server.\n\n"
    "- Streaming works\n- Markdown renders\n- Tool calls are confirmed before they run\n\n"
    "Ask me to *run a command* or *read a file* to see the confirmation flow."
)

FOLLOWUP_TEXT = (
    "Done — I ran the tool and here is what I found. The command completed and "
    "its output is shown in the card above."
)


def sse(obj):
    return ("data: " + json.dumps(obj) + "\n\n").encode()


class Handler(BaseHTTPRequestHandler):
    server_mode = DEFAULT_MODE

    def log_message(self, *a):
        pass

    def _reject(self, code, message, mtype="invalid_request_error"):
        body = json.dumps({"error": {"message": message, "type": mtype}}).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        if not self.path.rstrip("/").endswith("/chat/completions"):
            self._reject(404, "no such endpoint")
            return

        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length) if length else b"{}"
        try:
            req = json.loads(raw or b"{}")
        except json.JSONDecodeError:
            req = {}
        messages = req.get("messages", [])
        has_tools = bool(req.get("tools"))
        had_tool_result = any(m.get("role") == "tool" for m in messages)
        last_user = ""
        for m in reversed(messages):
            if m.get("role") == "user":
                last_user = (m.get("content") or "").lower()
                break

        mode = self.headers.get("X-Mock-Mode") or self.server_mode

        if mode == "auth401":
            self._reject(401, "invalid api key provided", "authentication_error")
            return
        if mode == "notfound404":
            self._reject(404, "the model 'nvidia/does-not-exist' does not exist", "not_found_error")
            return
        if mode == "ratelimit":
            self._reject(429, "rate limit exceeded", "rate_limit_error")
            return
        if mode == "timeout":
            # Accept, send headers, then stall so the client's transfer timeout fires.
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            try:
                time.sleep(600)
            except Exception:
                pass
            return

        # Decide whether to emit a tool call.
        wants_tool = (
            has_tools
            and not had_tool_result
            and (mode == "tool" or "run" in last_user or "command" in last_user
                 or "read file" in last_user or "list files" in last_user)
        )

        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()

        try:
            if wants_tool:
                # Stream a tool_call for run_shell_command: `ls -la ~`.
                args = json.dumps({"command": "ls", "args": ["-la", "/tmp"]})
                base = {"id": "chatcmpl-mock", "object": "chat.completion.chunk",
                        "model": req.get("model", "mock")}
                # name first, then arguments in two fragments (as real streams do).
                self.wfile.write(sse({**base, "choices": [{"index": 0, "delta": {
                    "tool_calls": [{"index": 0, "id": "call_mock_1", "type": "function",
                                    "function": {"name": "run_shell_command", "arguments": ""}}]},
                    "finish_reason": None}]}))
                self.wfile.flush()
                for frag in (args[: len(args) // 2], args[len(args) // 2:]):
                    self.wfile.write(sse({**base, "choices": [{"index": 0, "delta": {
                        "tool_calls": [{"index": 0, "function": {"arguments": frag}}]},
                        "finish_reason": None}]}))
                    self.wfile.flush()
                    time.sleep(0.05)
                self.wfile.write(sse({**base, "choices": [{"index": 0, "delta": {},
                                                           "finish_reason": "tool_calls"}]}))
                self.wfile.write(b"data: [DONE]\n\n")
                self.wfile.flush()
                return

            text = FOLLOWUP_TEXT if had_tool_result else REPLY_TEXT
            base = {"id": "chatcmpl-mock", "object": "chat.completion.chunk",
                    "model": req.get("model", "mock")}
            # role chunk
            self.wfile.write(sse({**base, "choices": [{"index": 0,
                                 "delta": {"role": "assistant"}, "finish_reason": None}]}))
            self.wfile.flush()
            for tok in _tokens(text):
                self.wfile.write(sse({**base, "choices": [{"index": 0,
                                     "delta": {"content": tok}, "finish_reason": None}]}))
                self.wfile.flush()
                time.sleep(0.02)
            self.wfile.write(sse({**base, "choices": [{"index": 0, "delta": {},
                                                       "finish_reason": "stop"}]}))
            self.wfile.write(b"data: [DONE]\n\n")
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass


def _tokens(text):
    # Split into small word-ish chunks to mimic token streaming.
    import re
    return [t for t in re.findall(r"\S+\s*|\n", text)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8900)
    ap.add_argument("--mode", default=DEFAULT_MODE)
    args = ap.parse_args()
    Handler.server_mode = args.mode
    srv = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"mock_api listening on http://127.0.0.1:{args.port}/v1 (mode={args.mode})", flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
