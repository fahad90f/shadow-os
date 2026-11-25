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
A model's "first": {...} is how it answers the first time it is asked a
question (then its normal answer: a model that fails once and answers when
asked again); "retry_after" goes with a "status" and sets that header.
A model's "review": {...} is how it answers in the team review (the second
request the members of the analysis team get); without it, it repeats its
first answer.
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
# The application's built-in panel, by role. The six analysis models are the
# analysis team: each has a seat, in this order.
PANEL = {
    "analysis": ["mistralai/mistral-large-4-1t-a52b", "z-ai/glm-5.3-flash", "google/gemma-3-12b-it",
                 "nvidia/nemotron-3.5-lightning-30b-a3b", "poolside/laguna-xs-2.1", "google/gemma-3-4b-it"],
    "reasoning": ["moonshotai/kimi-k3.1-pro", "nvidia/nemotron-3-ultra-550b-a55b", "stepfun-ai/step-5-preview"],
    "vision": ["nvidia/nemotron-3.5-super-vl-120b-a12b", "nvidia/nemotron-3-nano-omni-30b-a3b-reasoning",
               "nvidia/cosmos-reason2-8b"],
    "compute": ["minimaxai/minimax-m3.1-flash", "nvidia/nemotron-3.5-bolt-e4b"],
}
PANEL_MODELS = [model for models in PANEL.values() for model in models]
SEATS = ["Trend", "Momentum", "Volatility", "Structure", "Volume and candles", "Higher timeframe"]
# The rest of the catalog: models that are not on the panel, some of them not chat models.
OTHER_MODELS = ["bigcode/starcoder2-15b", "nvidia/riva-translate-4b-instruct-v2", "nvidia/embed-qa-4",
                "nvidia/nemotron-3.5-content-safety"]
INTERVAL_MS = {"1m": 60_000, "5m": 300_000, "15m": 900_000, "1h": 3_600_000, "4h": 14_400_000,
               "1d": 86_400_000}
BASE_PRICE = {"BTCUSDT": 92000.0, "ETHUSDT": 3200.0, "SOLUSDT": 140.0, "PAXGUSDT": 4050.0, "EURUSDT": 1.158,
              # Listed by the service but not on the built-in watchlist: for adding a market.
              "DOGEUSDT": 0.2314, "XRPUSDT": 2.41, "BNBUSDT": 912.0, "ETHBTC": 0.0348}
# The forex service (Kraken's public API): symbol -> (the key its answers use,
# trades in 24 hours). Its oldest pairs have keys with a class letter before
# each currency. The answers below are shaped after recordings of the real
# service, which a scenario can also have sent as they were
# (tests/fixtures/real-kraken-*).
KRAKEN = {
    "EURUSD": ("ZEURZUSD", 81153), "GBPUSD": ("ZGBPZUSD", 22916), "USDCAD": ("ZUSDZCAD", 8511),
    "USDCHF": ("USDCHF", 7168), "AUDUSD": ("AUDUSD", 4958), "PAXGUSD": ("PAXGUSD", 6302),
    "XAUTUSD": ("XAUTUSD", 3813),
    "USDJPY": ("ZUSDZJPY", 33),      # listed, but a few stray trades a day: not offered
    "EURGBP": ("EURGBP", 1518),      # a cross: not offered
    "XBTUSD": ("XXBTZUSD", 90210),   # not a currency pair
}
BASE_PRICE.update({"EURUSD": 1.0982, "GBPUSD": 1.3124, "USDCAD": 1.4201, "USDCHF": 0.8311, "AUDUSD": 0.6630,
                   "PAXGUSD": 3541.4, "XAUTUSD": 3540.1, "USDJPY": 157.2, "EURGBP": 0.8367, "XBTUSD": 92000.0})
KRAKEN_MINUTES = {1: "1m", 5: "5m", 15: "15m", 60: "1h", 240: "4h", 1440: "1d"}
# The order a list of tickers comes back in, whatever order was asked for.
LISTING_ORDER = ["BTCUSDT", "ETHUSDT", "EURUSDT", "SOLUSDT", "PAXGUSDT", "ETHBTC", "BNBUSDT", "XRPUSDT", "DOGEUSDT"]
# The first candle the service has of any market, in hours before it started:
# asking for candles before that gives none (the beginning of the history).
HISTORY_HOURS = 2000

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
    if "endTime" in query:
        # Candles that opened at or before that time (older history).
        last = min(last, int(query["endTime"][0]) // step)
    oldest = int((START_HOURS - HISTORY_HOURS) * 3_600_000) // step
    return 200, [candle(symbol, k, step, now_ms) for k in range(max(oldest, last - limit + 1), last + 1)]


def all_prices(now_ms):
    step = 60_000
    return 200, [{"symbol": symbol, "price": f"{level(symbol, now_ms / step, step):.8f}"} for symbol in LISTING_ORDER]


# ---- the live stream ---------------------------------------------------------------

def stream_messages(streams, now_ms, last_sent):
    """What the combined stream pushes now: every trade stream gets a trade, kline and
    ticker streams one message a second ('last_sent': stream -> time of the last one)."""
    out = []
    for name in streams:
        market, _, kind = name.partition("@")
        symbol = market.upper()
        if symbol not in BASE_PRICE:
            continue
        move = 1 + scenario().get("market", {}).get("move_pct", 0) / 100
        price = level(symbol, now_ms / 60_000, 60_000) * move
        if kind == "aggTrade":
            data = {"e": "aggTrade", "E": now_ms, "s": symbol, "a": now_ms, "p": f"{price:.8f}", "q": "0.01000000",
                    "f": 1, "l": 1, "T": now_ms, "m": False, "M": True}
        elif now_ms - last_sent.get(name, 0) < 1000:
            continue
        elif kind.startswith("kline_") and kind[6:] in INTERVAL_MS:
            step = INTERVAL_MS[kind[6:]]
            k = now_ms // step
            row = candle(symbol, k, step, now_ms)
            # The forming candle ends at the price the trades are at.
            close = level(symbol, now_ms / step, step) * move
            data = {"e": "kline", "E": now_ms, "s": symbol,
                    "k": {"t": row[0], "T": row[6], "s": symbol, "i": kind[6:], "o": row[1], "c": f"{close:.8f}",
                          "h": f"{max(float(row[2]), close):.8f}", "l": f"{min(float(row[3]), close):.8f}",
                          "v": row[5], "n": 100, "x": False, "q": row[7], "V": row[9], "Q": row[10], "B": "0"}}
        elif kind == "miniTicker":
            day_ago = level(symbol, (now_ms - 86_400_000) / 60_000, 60_000)
            data = {"e": "24hrMiniTicker", "E": now_ms, "s": symbol, "c": f"{price:.8f}", "o": f"{day_ago:.8f}",
                    "h": f"{max(price, day_ago) * 1.004:.8f}", "l": f"{min(price, day_ago) * 0.996:.8f}",
                    "v": "1000.00000000", "q": "123456789.00000000"}
        else:
            continue
        last_sent[name] = now_ms
        out.append({"stream": name, "data": data})
    return out


def frame(text):
    """One unmasked WebSocket text frame (server to client)."""
    payload = text.encode()
    if len(payload) < 126:
        head = bytes([0x81, len(payload)])
    elif len(payload) < 65536:
        head = bytes([0x81, 126]) + struct.pack(">H", len(payload))
    else:
        head = bytes([0x81, 127]) + struct.pack(">Q", len(payload))
    return head + payload


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


# ---- the forex service -----------------------------------------------------------

def kraken_symbol(name):
    """'EUR/USD', 'EURUSD' or 'ZEURZUSD' -> 'EURUSD'; None for a pair the service does not have."""
    name = name.upper().replace("/", "")
    for symbol, (key, _) in KRAKEN.items():
        if name in (symbol, key):
            return symbol
    return None


UNKNOWN_PAIR = {"error": ["EQuery:Unknown asset pair"]}


def recorded(name):
    """{"kraken": {"ticker": FILE, "ohlc": FILE}}: an answer recorded from the real
    service, sent as it was received; None when the scenario names none."""
    path = scenario().get("kraken", {}).get(name)
    if not path:
        return None
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def kraken_ohlc(query, now_ms):
    if recorded("ohlc"):
        return 200, recorded("ohlc")
    symbol = kraken_symbol(query.get("pair", [""])[0])
    try:
        minutes = int(query.get("interval", ["1"])[0])
    except ValueError:
        minutes = 0
    if symbol is None:
        return 200, UNKNOWN_PAIR
    if minutes not in KRAKEN_MINUTES:
        return 200, {"error": ["EGeneral:Invalid arguments"]}
    step = minutes * 60_000
    last = now_ms // step
    rows = []
    # The newest 720 candles, and nothing before them, whatever 'since' says.
    for k in range(last - 720, last + 1):
        c = candle(symbol, k, step, now_ms)
        rows.append([c[0] // 1000, c[1][:-3], c[2][:-3], c[3][:-3], c[4][:-3], c[4][:-3], c[5], 100])
    return 200, {"error": [], "result": {KRAKEN[symbol][0]: rows, "last": rows[-2][0]}}


def kraken_now(symbol, now_ms):
    step = 60_000
    return level(symbol, now_ms / step, step) * (1 + scenario().get("market", {}).get("move_pct", 0) / 100)


def kraken_ticker(query, now_ms):
    wanted = [name for name in query.get("pair", [""])[0].split(",") if name]
    if not wanted and recorded("ticker"):   # everything the service lists
        return 200, recorded("ticker")
    symbols = [kraken_symbol(name) for name in wanted] if wanted else list(KRAKEN)
    if None in symbols:   # one unknown name refuses the whole list
        return 200, UNKNOWN_PAIR
    result = {}
    for symbol in symbols:
        key, trades = KRAKEN[symbol]
        now = kraken_now(symbol, now_ms)
        # "o" is today's opening price (midnight UTC), not the price 24 hours ago.
        opened = level(symbol, (now_ms - now_ms % 86_400_000) / 60_000, 60_000)
        text = lambda v: f"{v:.5f}"
        result[key] = {"a": [text(now * 1.00001), "100", "100.000"], "b": [text(now * 0.99999), "100", "100.000"],
                       "c": [text(now), "10.00000000"], "v": ["1000000.0", "2000000.0"],
                       "p": [text(now), text(now)], "t": [trades // 2, trades],
                       "l": [text(min(now, opened) * 0.996)] * 2, "h": [text(max(now, opened) * 1.004)] * 2,
                       "o": text(opened)}
    return 200, {"error": [], "result": result}


def kraken_pair(symbol):
    """'EURUSD' -> 'EUR/USD', as the stream names it."""
    for quote in ("USD", "GBP", "JPY", "CHF", "CAD"):
        if symbol.endswith(quote) and len(symbol) > len(quote):
            return symbol[:-len(quote)] + "/" + quote
    return symbol


def kraken_ticker_row(symbol, now_ms):
    now = kraken_now(symbol, now_ms)
    day_ago = level(symbol, (now_ms - 86_400_000) / 60_000, 60_000)
    return {"symbol": kraken_pair(symbol), "bid": now * 0.99999, "bid_qty": 100.0, "ask": now * 1.00001,
            "ask_qty": 100.0, "last": now, "volume": 2000000.0, "vwap": now, "low": min(now, day_ago) * 0.996,
            "high": max(now, day_ago) * 1.004, "change": now - day_ago,
            "change_pct": round((now - day_ago) / day_ago * 100, 2), "trades": KRAKEN[symbol][1]}


def kraken_stream_messages(subs, now_ms, last_sent):
    """What the forex stream pushes now for the subscriptions made so far."""
    out = []
    stamp = time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(now_ms / 1000)) + f".{now_ms % 1000:03d}000Z"
    for symbol in subs.get("trade", []):
        out.append({"channel": "trade", "type": "update", "data": [
            {"symbol": kraken_pair(symbol), "side": "buy", "price": round(kraken_now(symbol, now_ms), 6),
             "qty": 10.0, "ord_type": "limit", "trade_id": now_ms, "timestamp": stamp}]})
    if now_ms - last_sent.get("slow", 0) < 1000:
        return out
    last_sent["slow"] = now_ms
    for symbol, minutes in subs.get("ohlc", []):
        step = minutes * 60_000
        row = candle(symbol, now_ms // step, step, now_ms)
        close = kraken_now(symbol, now_ms)
        begin = time.strftime("%Y-%m-%dT%H:%M:%S", time.gmtime(row[0] / 1000)) + ".000000000Z"
        out.append({"channel": "ohlc", "type": "update", "timestamp": stamp, "data": [
            {"symbol": kraken_pair(symbol), "open": float(row[1]), "high": max(float(row[2]), close),
             "low": min(float(row[3]), close), "close": close, "trades": 100, "volume": float(row[5]),
             "vwap": close, "interval_begin": begin, "interval": minutes, "timestamp": stamp}]})
    for symbol in subs.get("ticker", []):
        out.append({"channel": "ticker", "type": "update", "data": [kraken_ticker_row(symbol, now_ms)]})
    out.append({"channel": "heartbeat"})
    return out


def client_frames(buffer):
    """The complete frames in 'buffer' (a bytearray of what a WebSocket client sent,
    masked as clients must), removed from it: the text of each text frame, None
    for a close frame."""
    texts = []
    while len(buffer) >= 2:
        opcode, length, at = buffer[0] & 0x0F, buffer[1] & 0x7F, 2
        if length == 126:
            if len(buffer) < 4:
                break
            length, at = int.from_bytes(buffer[2:4], "big"), 4
        elif length == 127:
            if len(buffer) < 10:
                break
            length, at = int.from_bytes(buffer[2:10], "big"), 10
        masked = bool(buffer[1] & 0x80)
        need = at + (4 if masked else 0) + length
        if len(buffer) < need:
            break
        mask = bytes(buffer[at:at + 4]) if masked else bytes(4)
        payload = bytes(b ^ mask[i % 4] for i, b in enumerate(buffer[need - length:need]))
        del buffer[:need]
        if opcode == 0x8:
            texts.append(None)
        elif opcode == 0x1:
            texts.append(payload.decode("utf-8", "replace"))
    return texts


# ---- models ------------------------------------------------------------------

def catalog():
    missing = set(scenario().get("missing", []))
    return sorted(m for m in PANEL_MODELS + OTHER_MODELS if m not in missing)


ASKED = {}          # (model, review) -> requests so far
IN_FLIGHT = [0]     # chat requests being answered right now
COUNT_LOCK = threading.Lock()


def plan_for(model, review=False, attempt=0):
    """How a model answers; in the team review, its "review" plan when it has
    one; and, the first time it is asked, its "first" plan when it has one."""
    plans = scenario()
    plan = plans.get(model, plans.get("*", {"action": "hold"}))
    if review:
        plan = plan.get("review", plan)
    if attempt == 1 and "first" in plan:
        plan = plan["first"]
    return plan


REVIEW_MARK = "\n\nTeam review. "


def user_text(body):
    """The text a chat request's user messages carry."""
    text = ""
    for message in body.get("messages", []) if isinstance(body, dict) else []:
        if not isinstance(message, dict) or message.get("role") != "user":
            continue
        content = message.get("content")
        if isinstance(content, str):
            text += content
        elif isinstance(content, list):
            text += "".join(part.get("text", "") for part in content if part.get("type") == "text")
    return text


def review_notes(user):
    """What a team review request carries: the member's own call and its teammates', by seat."""
    notes = {"review": REVIEW_MARK in user}
    if not notes["review"]:
        return notes
    tail = user[user.index(REVIEW_MARK):]
    try:
        own = json.loads(tail[tail.index("Your first call:\n") + 17:].split("\n", 1)[0])
        mates = json.loads(tail[tail.index("Your teammates' calls:\n") + 23:])
        notes.update({"own": [own["seat"], own["action"]], "teammates": [[m["seat"], m["action"]] for m in mates]})
    except (ValueError, KeyError, TypeError):
        notes["malformed"] = True
    return notes


def verdict_text(model, snapshot_price, plan):
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
        # The snapshot is the first JSON value; a team review has more text after it.
        return float(json.JSONDecoder().raw_decode(text[start:])[0].get("price", 0))
    except (ValueError, AttributeError):
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


def chat(body, attempt=0):
    """(status, answer, notes) for a chat completions request; 'attempt' counts
    the requests this model got for this question (1 = the first)."""
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
    # The seat of a member of the analysis team, as its instructions name it.
    mark = "Your seat on the analysis team: "
    notes["seat"] = system[system.index(mark) + len(mark):].split(".", 1)[0] if mark in system else ""
    notes.update(review_notes(user))
    plan = plan_for(model, notes["review"], attempt)
    message = {"role": "assistant", "content": verdict_text(model, price_in(user), plan)}
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

    def serve_stream(self, query):
        """The combined market stream: /stream?streams=a@aggTrade/a@kline_15m/b@miniTicker."""
        plan = scenario().get("stream", {})
        key = self.headers.get("Sec-WebSocket-Key", "")
        if plan.get("off") or self.headers.get("Upgrade", "").lower() != "websocket" or not key:
            record({"kind": "stream", "status": 404})
            return self.reply(404, {"code": -1, "msg": "no stream here"})
        streams = [name for name in query.get("streams", [""])[0].split("/") if name]
        record({"kind": "stream", "status": 101, "streams": streams})
        accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest())
        self.send_response(101, "Switching Protocols")
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept.decode())
        self.end_headers()
        self.close_connection = True
        started, last_sent = time.time(), {}
        try:
            # {"stream": {"replay": FILE}}: first the recorded messages of the real
            # service, one per line, exactly as they were received.
            if plan.get("replay"):
                time.sleep(plan.get("replay_after", 0))
                with open(plan["replay"], encoding="utf-8") as f:
                    for line in f:
                        if line.strip():
                            self.wfile.write(frame(line.strip()))
                self.wfile.flush()
            while True:
                plan = scenario().get("stream", {})
                # {"stream": {"drop_after": 2}}: the connection is cut after that many seconds.
                if "drop_after" in plan and time.time() - started >= plan["drop_after"]:
                    record({"kind": "stream", "status": "dropped"})
                    return
                if not plan.get("silent"):
                    for message in stream_messages(streams, int(time.time() * 1000), last_sent):
                        self.wfile.write(frame(json.dumps(message, separators=(",", ":"))))
                    self.wfile.flush()
                time.sleep(0.1)
        except (BrokenPipeError, ConnectionResetError, OSError):
            return

    def serve_kraken_stream(self):
        """The forex service's stream (/v2): the client subscribes after connecting."""
        plan = scenario().get("stream", {})
        key = self.headers.get("Sec-WebSocket-Key", "")
        if plan.get("off") or self.headers.get("Upgrade", "").lower() != "websocket" or not key:
            record({"kind": "stream", "status": 404})
            return self.reply(404, {"error": "no stream here"})
        record({"kind": "stream", "status": 101, "service": "kraken"})
        accept = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest())
        self.send_response(101, "Switching Protocols")
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", accept.decode())
        self.end_headers()
        self.close_connection = True
        send = lambda message: self.wfile.write(frame(json.dumps(message, separators=(",", ":"))))
        started, last_sent, subs, received, replayed = time.time(), {}, {}, bytearray(), False
        self.connection.settimeout(0.1)
        try:
            send({"channel": "status", "type": "update", "data": [{"system": "online", "api_version": "v2"}]})
            while True:
                try:
                    chunk = self.connection.recv(65536)
                    if not chunk:
                        return
                    received += chunk
                except TimeoutError:
                    pass
                now_ms = int(time.time() * 1000)
                for text in client_frames(received):
                    if text is None:
                        return
                    try:
                        params = json.loads(text).get("params", {})
                    except ValueError:
                        continue
                    channel, names = params.get("channel"), params.get("symbol", [])
                    record({"kind": "subscribe", "channel": channel, "symbols": names,
                            "interval": params.get("interval"), "snapshot": params.get("snapshot")})
                    for name in names:
                        symbol = kraken_symbol(name)
                        if symbol is None:
                            send({"error": f"Currency pair not supported {name}", "method": "subscribe",
                                  "success": False, "symbol": name})
                            continue
                        send({"method": "subscribe", "success": True,
                              "result": {"channel": channel, "symbol": name}})
                        if channel == "ohlc":
                            subs.setdefault("ohlc", []).append((symbol, int(params.get("interval", 1))))
                        else:
                            subs.setdefault(channel, []).append(symbol)
                        if channel == "ticker" and not plan.get("silent"):
                            send({"channel": "ticker", "type": "snapshot", "data": [kraken_ticker_row(symbol, now_ms)]})
                plan = scenario().get("stream", {})
                if plan.get("replay") and not replayed and subs and time.time() - started >= plan.get("replay_after", 0):
                    replayed = True
                    with open(plan["replay"], encoding="utf-8") as f:
                        for line in f:
                            if line.strip():
                                self.wfile.write(frame(line.strip()))
                if "drop_after" in plan and time.time() - started >= plan["drop_after"]:
                    record({"kind": "stream", "status": "dropped"})
                    return
                if not plan.get("silent"):
                    for message in kraken_stream_messages(subs, now_ms, last_sent):
                        send(message)
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, OSError):
            return

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        query = urllib.parse.parse_qs(parsed.query)
        now_ms = int(time.time() * 1000)
        if parsed.path == "/stream":
            return self.serve_stream(query)
        if parsed.path == "/v2":
            return self.serve_kraken_stream()
        if parsed.path.startswith("/0/public/"):
            market = scenario().get("market", {})
            if "status" in market:
                record({"kind": "market", "path": parsed.path, "status": market["status"]})
                return self.reply(market["status"], {"error": ["EService:Unavailable"]})
            if parsed.path == "/0/public/OHLC":
                status, payload = kraken_ohlc(query, now_ms)
            elif parsed.path == "/0/public/Ticker":
                status, payload = kraken_ticker(query, now_ms)
            else:
                status, payload = 404, {"error": ["EGeneral:Unknown method"]}
            record({"kind": "market", "path": parsed.path, "query": parsed.query, "status": status})
            return self.reply(status, payload)
        if parsed.path.startswith("/api/v3/"):
            market = scenario().get("market", {})
            if "status" in market:
                record({"kind": "market", "path": parsed.path, "status": market["status"]})
                return self.reply(market["status"], {"code": 0, "msg": "Service unavailable from a restricted location."})
            if parsed.path == "/api/v3/klines":
                status, payload = klines(query, now_ms)
            elif parsed.path == "/api/v3/ticker/price":
                status, payload = all_prices(now_ms)
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
        review = REVIEW_MARK in user_text(body)
        with COUNT_LOCK:
            attempt = ASKED[(model, review)] = ASKED.get((model, review), 0) + 1
            IN_FLIGHT[0] += 1
            entry["attempt"] = attempt
            entry["inflight"] = IN_FLIGHT[0]
        try:
            return self.answer_chat(body, model, review, attempt, entry)
        finally:
            with COUNT_LOCK:
                IN_FLIGHT[0] -= 1

    def answer_chat(self, body, model, review, attempt, entry):
        plan = plan_for(model, review, attempt)
        # A slow answer: the request is logged when it arrives, answered later
        # (to a client that may have stopped waiting).
        delay = float(plan.get("delay", 0))
        if "status" in plan:
            entry["status"] = plan["status"]
            entry["review"] = review
            record(entry)
            time.sleep(delay)
            if plan.get("retry_after"):
                data = json.dumps({"status": plan["status"], "detail": "slow down"}).encode()
                self.send_response(plan["status"])
                self.send_header("Content-Type", "application/json")
                self.send_header("Retry-After", str(plan["retry_after"]))
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
                return None
            return self.reply(plan["status"], {"status": plan["status"], "detail": plan.get("message", "scenario error")})
        status, payload, notes = chat(body, attempt)
        entry.update(notes)
        entry["status"] = status
        if status == 400:
            entry["rejected"] = payload["error"]["message"]
        if isinstance(body, dict):
            entry["params"] = sorted(k for k in body if k != "messages")
        record(entry)
        time.sleep(delay)
        try:
            self.reply(status, payload)
        except OSError:
            pass  # the client stopped waiting


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
