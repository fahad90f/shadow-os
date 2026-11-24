#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Stand-ins for everything Shadow Trading talks to, on one local port.

    fake_services.py --port 18080 [--scenario FILE] [--log FILE] [--trend up|down|flat]

* A Binance-compatible market data API (/api/v3/klines, /api/v3/ticker/24hr)
  with made-up but self-consistent candles: the same request always sees the
  same history, and the last candle is still forming.
* NVIDIA's model service (/nvidia/v1/chat/completions and /nvidia/v1/models):
  the OpenAI-compatible protocol, one test key, a catalog of models. A request
  is checked the way the real service would: the key, the model, the shape of
  the body.

The scenario file (JSON, re-read on every request) says how each model
answers, by model id, with "*" for every model that is not named:
{"*": {"action": "buy", "confidence": 80, "stop_pct": -1.0, "target_pct": 2.5},
"moonshotai/kimi-k3.1-pro": {"status": 429}, "z-ai/glm-5.3-flash": {"raw":
"text"}}. Options of an answer: "fenced" (wrap in a code fence), "think"
(thinking in <think> tags first), "refusal", "empty", "delay" (seconds).
Without a scenario every model answers hold.
"missing": [ids] takes models out of the catalog.
"market": {"status": 451} makes the exchange refuse; "market": {"move_pct": -3}
moves the current price (the forming candle and the tickers) by that much.

Every request is appended to the log file as one JSON line. The key is never
written there: only whether the right one was sent.

Nothing here touches a real exchange or a real model service.
"""
import argparse
import base64
import hashlib
import json
import math
import struct
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

TEST_KEY = "nvapi-test-00000000000000000000000"
# The application's built-in panel, by role.
PANEL = {
    "analysis": ["mistralai/mistral-large-4-1t-a52b", "z-ai/glm-5.3-flash", "google/gemma-4-31b-it"],
    "reasoning": ["moonshotai/kimi-k3.1-pro", "nvidia/nemotron-3-ultra-550b-a55b", "stepfun-ai/step-5-preview"],
    "vision": ["nvidia/nemotron-3.5-super-vl-120b-a12b", "nvidia/nemotron-3-nano-omni-30b-a3b-reasoning",
               "meta/muse-glimmer-30b"],
    "compute": ["minimaxai/minimax-m3.1-flash", "nvidia/nemotron-3.5-bolt-e4b",
                "nvidia/nemotron-3.5-lightning-30b-a3b"],
}
PANEL_MODELS = [model for models in PANEL.values() for model in models]
# The rest of the catalog: models that are not on the panel, some of them not chat models.
OTHER_MODELS = ["google/gemma-3-12b-it", "poolside/laguna-xs-2.1", "nvidia/embed-qa-4",
                "nvidia/nemotron-3.5-content-safety"]
INTERVAL_MS = {"1m": 60_000, "5m": 300_000, "15m": 900_000, "1h": 3_600_000, "4h": 14_400_000,
               "1d": 86_400_000}
BASE_PRICE = {"BTCUSDT": 92000.0, "ETHUSDT": 3200.0, "SOLUSDT": 140.0, "PAXGUSDT": 4050.0, "EURUSDT": 1.158}
# The order a list of tickers comes back in, whatever order was asked for.
LISTING_ORDER = ["BTCUSDT", "ETHUSDT", "EURUSDT", "SOLUSDT", "PAXGUSDT"]

ARGS = None
START_HOURS = time.time() / 3600.0
LOG_LOCK = threading.Lock()


def record(entry):
    if not ARGS.log:
        return
    with LOG_LOCK, open(ARGS.log, "a", encoding="utf-8") as f:
        f.write(json.dumps(entry, sort_keys=True) + "\n")


def scenario():
    if not ARGS.scenario:
        return {}
    try:
        with open(ARGS.scenario, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


# ---- market ---------------------------------------------------------------

def unit(symbol, k, salt):
    """A repeatable number in [0, 1) for one candle of one market."""
    digest = hashlib.sha256(f"{symbol}:{k}:{salt}".encode()).digest()
    return int.from_bytes(digest[:8], "big") / 2 ** 64


def level(symbol, x, step_ms):
    """The price path at candle position x (a float: candle index plus fraction)."""
    base = BASE_PRICE.get(symbol, 100.0)
    hours = x * step_ms / 3_600_000.0
    # The trend is measured from when the server started, so prices stay near
    # the base price: lower in the past for "up", higher for "down".
    drift = {"up": 0.0009, "down": -0.0009, "flat": 0.0}[ARGS.trend] * (hours - START_HOURS)
    wave = 0.006 * math.sin(hours / 9.0) + 0.0025 * math.sin(hours / 2.3)
    return base * math.exp(drift + wave)


def close_of(symbol, k, step_ms):
    """The close of a finished candle: the path plus a little noise."""
    return level(symbol, k + 1, step_ms) * (1 + (unit(symbol, k, "c") - 0.5) * 0.0012)


def candle(symbol, k, step_ms, now_ms):
    open_time = k * step_ms
    close_time = open_time + step_ms - 1
    forming = close_time >= now_ms
    fraction = (now_ms - open_time) / step_ms if forming else 1.0
    start = close_of(symbol, k - 1, step_ms)  # a candle opens where the one before closed
    if forming:
        end = start + level(symbol, k + fraction, step_ms) - level(symbol, k, step_ms)
        # A test can push the current price: {"market": {"move_pct": -3}}.
        end *= 1 + scenario().get("market", {}).get("move_pct", 0) / 100
    else:
        end = close_of(symbol, k, step_ms)
    wick = abs(start) * 0.0006
    high = max(start, end) + wick * unit(symbol, k, "h") * fraction
    low = min(start, end) - wick * unit(symbol, k, "l") * fraction
    volume = (50 + 400 * unit(symbol, k, "v")) * fraction
    text = lambda v: f"{v:.8f}"
    return [open_time, text(start), text(high), text(low), text(end), text(volume), close_time,
            text(volume * end), 100, text(volume / 2), text(volume * end / 2), "0"]


def klines(query, now_ms):
    symbol = query.get("symbol", [""])[0]
    interval = query.get("interval", [""])[0]
    if symbol not in BASE_PRICE or interval not in INTERVAL_MS:
        return 400, {"code": -1121, "msg": "Invalid symbol."}
    limit = max(1, min(1000, int(query.get("limit", ["500"])[0])))
    step = INTERVAL_MS[interval]
    last = now_ms // step
    return 200, [candle(symbol, k, step, now_ms) for k in range(last - limit + 1, last + 1)]


def tickers(query, now_ms):
    # ?symbol=X answers with one object, ?symbols=[...] with a list. As on the
    # real service, one unknown name refuses the whole list without saying
    # which, and the list comes back in the service's order, not the asker's.
    single = "symbol" in query
    try:
        symbols = [query["symbol"][0]] if single else json.loads(query.get("symbols", ["[]"])[0])
    except ValueError:
        return 400, {"code": -1100, "msg": "Illegal characters found in parameter 'symbols'."}
    if any(symbol not in BASE_PRICE for symbol in symbols):
        return 400, {"code": -1121, "msg": "Invalid symbol."}
    out = []
    for symbol in sorted(symbols, key=LISTING_ORDER.index):
        step = 60_000
        now = level(symbol, now_ms / step, step) * (1 + scenario().get("market", {}).get("move_pct", 0) / 100)
        day_ago = level(symbol, (now_ms - 86_400_000) / step, step)
        out.append({"symbol": symbol, "lastPrice": f"{now:.8f}",
                    "priceChangePercent": f"{(now - day_ago) / day_ago * 100:.3f}",
                    "highPrice": f"{max(now, day_ago) * 1.004:.8f}", "lowPrice": f"{min(now, day_ago) * 0.996:.8f}",
                    "quoteVolume": "123456789.00000000"})
    return 200, out[0] if single else out


# ---- models ------------------------------------------------------------------

def catalog():
    missing = set(scenario().get("missing", []))
    return sorted(m for m in PANEL_MODELS + OTHER_MODELS if m not in missing)


def plan_for(model):
    plans = scenario()
    return plans.get(model, plans.get("*", {"action": "hold"}))


def verdict_text(model, snapshot_price):
    plan = plan_for(model)
    if "raw" in plan:
        return plan["raw"]
    action = plan.get("action", "hold")
    answer = {"action": action, "confidence": plan.get("confidence", 0 if action == "hold" else 75),
              "stop_loss": 0, "take_profit": 0,
              "reasoning": plan.get("reasoning", f"Test verdict of {model}: {action}.")}
    if action in ("buy", "sell"):
        side = 1 if action == "buy" else -1
        answer["stop_loss"] = round(snapshot_price * (1 + side * plan.get("stop_pct", -1.0) / 100), 6)
        answer["take_profit"] = round(snapshot_price * (1 + side * plan.get("target_pct", 2.5) / 100), 6)
    text = json.dumps(answer)
    if plan.get("fenced"):
        text = "Here is my answer.\n```json\n" + text + "\n```"
    if plan.get("think"):
        # Thinking with braces of its own in front of the answer.
        text = "<think>The trend is {up} and the stop {below}; so {\"action\": \"maybe\"}?</think>\n" + text
    return text


def price_in(text):
    """The price in the snapshot the application sent, 0 for a connection test."""
    start = text.find("{")
    if start < 0:
        return 0.0
    try:
        return float(json.loads(text[start:]).get("price", 0))
    except ValueError:
        return 0.0


def png_size(data_url):
    """'WxH' of a data:image/png;base64 URL, or a word saying what is wrong."""
    prefix = "data:image/png;base64,"
    if not data_url.startswith(prefix):
        return "not-a-png-data-url"
    try:
        raw = base64.b64decode(data_url[len(prefix):], validate=True)
    except ValueError:
        return "bad-base64"
    if raw[:8] != b"\x89PNG\r\n\x1a\n" or raw[12:16] != b"IHDR":
        return "not-a-png"
    width, height = struct.unpack(">II", raw[16:24])
    return f"{width}x{height}"


def chat(body):
    """(status, answer, notes) for a chat completions request."""
    notes = {}
    if not isinstance(body, dict) or "model" not in body or not isinstance(body.get("messages"), list):
        return 400, {"error": {"message": "model and messages are required"}}, notes
    model = body["model"]
    notes["model"] = model
    if model not in catalog():
        return 404, {"status": 404, "title": "Not Found", "detail": f"Function for {model} not found"}, notes
    if body.get("stream"):
        return 400, {"error": {"message": "this stand-in does not stream"}}, notes
    if not isinstance(body.get("max_tokens"), int) or not 1 <= body["max_tokens"] <= 16384:
        return 400, {"error": {"message": "max_tokens must be between 1 and 16384"}}, notes
    user = ""
    system = ""
    images = []
    for message in body["messages"]:
        content = message.get("content")
        if message.get("role") == "system" and isinstance(content, str):
            system += content
        if message.get("role") != "user":
            continue
        if isinstance(content, str):
            user += content
            continue
        for part in content:
            if part.get("type") == "text":
                user += part.get("text", "")
            elif part.get("type") == "image_url":
                images.append(png_size(part.get("image_url", {}).get("url", "")))
    notes["images"] = images
    notes["system"] = bool(system)
    # Which skill briefs the application put into the instructions.
    notes["skills"] = sorted(name for name, title in (("ta", "Skill: TA Indicators"), ("smc", "Skill: SMC Patterns"),
                                                      ("risk", "Skill: Risk Engine"), ("chart", "Skill: Chart Reading"))
                             if title in system)
    notes["temperature"] = body.get("temperature")
    plan = plan_for(model)
    message = {"role": "assistant", "content": verdict_text(model, price_in(user))}
    if plan.get("refusal"):
        message = {"role": "assistant", "content": None, "refusal": "I can't help with that."}
    if plan.get("empty"):
        message = {"role": "assistant", "content": "", "reasoning_content": "I thought about it at length."}
    return 200, {"id": "chatcmpl-test", "object": "chat.completion", "model": model,
                 "choices": [{"index": 0, "finish_reason": "stop", "message": message}],
                 "usage": {"prompt_tokens": 1, "completion_tokens": 1, "total_tokens": 2}}, notes


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # the JSON log is the log
        pass

    def reply(self, status, payload):
        data = payload if isinstance(payload, bytes) else json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def key_check(self):
        """'ok', 'missing' or 'wrong' — never the key."""
        sent = self.headers.get("Authorization", "").removeprefix("Bearer ").strip()
        return "missing" if not sent else "ok" if sent == TEST_KEY else "wrong"

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        query = urllib.parse.parse_qs(parsed.query)
        now_ms = int(time.time() * 1000)
        if parsed.path.startswith("/api/v3/"):
            market = scenario().get("market", {})
            if "status" in market:
                record({"kind": "market", "path": parsed.path, "status": market["status"]})
                return self.reply(market["status"], {"code": 0, "msg": "Service unavailable from a restricted location."})
            if parsed.path == "/api/v3/klines":
                status, payload = klines(query, now_ms)
            elif parsed.path == "/api/v3/ticker/24hr":
                status, payload = tickers(query, now_ms)
            else:
                status, payload = 404, {"code": -1, "msg": "not found"}
            record({"kind": "market", "path": parsed.path, "query": parsed.query, "status": status})
            return self.reply(status, payload)
        if parsed.path == "/nvidia/v1/models":
            # Public on the real service: listed with or without a key.
            record({"kind": "models", "auth": self.key_check()})
            return self.reply(200, {"object": "list", "data": [{"id": m, "object": "model"} for m in catalog()]})
        self.reply(404, {"error": {"message": "not found"}})

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length)
        if parsed.path != "/nvidia/v1/chat/completions":
            return self.reply(404, {"error": {"message": "not found"}})
        auth = self.key_check()
        try:
            body = json.loads(raw)
        except ValueError:
            body = None
        model = body.get("model", "") if isinstance(body, dict) else ""
        entry = {"kind": "chat", "auth": auth, "bytes": len(raw), "model": model}
        if auth != "ok":
            entry["status"] = 401
            record(entry)
            # The refusal quotes part of what was sent, as services do.
            return self.reply(401, {"status": 401, "title": "Unauthorized",
                                    "detail": "Authentication failed for key nvapi-…abcd"})
        plan = plan_for(model)
        if plan.get("delay"):
            time.sleep(float(plan["delay"]))
        if "status" in plan:
            entry["status"] = plan["status"]
            record(entry)
            return self.reply(plan["status"], {"status": plan["status"], "detail": plan.get("message", "scenario error")})
        status, payload, notes = chat(body)
        entry.update(notes)
        entry["status"] = status
        if status == 400:
            entry["rejected"] = payload["error"]["message"]
        if isinstance(body, dict):
            entry["params"] = sorted(k for k in body if k != "messages")
        record(entry)
        self.reply(status, payload)


def main():
    global ARGS
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--scenario", default="")
    parser.add_argument("--log", default="")
    parser.add_argument("--trend", choices=("up", "down", "flat"), default="up")
    ARGS = parser.parse_args()
    server = ThreadingHTTPServer(("127.0.0.1", ARGS.port), Handler)
    server.daemon_threads = True
    print(f"fake services on http://127.0.0.1:{ARGS.port}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
