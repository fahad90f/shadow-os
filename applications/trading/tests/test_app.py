#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end tests of shadow-trading through its --dump mode, against
tests/fake_services.py. No real exchange, no real model service, no real key.

    SHADOW_TRADING_BIN=<build>/bin/shadow-trading python3 -B tests/test_app.py -v

The binary needs its Qt libraries (LD_LIBRARY_PATH for a staged build). Every
test gets its own stand-in server, settings and data directories.
"""
import json
import os
import shutil
import socket
import stat
import statistics
import subprocess
import sys
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.environ.get("SHADOW_TRADING_BIN", "")
sys.path.insert(0, HERE)
from fake_services import PANEL, PANEL_MODELS, SEATS, TEST_KEY  # noqa: E402  (the stand-in's own test key)

BUY = {"action": "buy", "confidence": 85, "stop_pct": -1.0, "target_pct": 2.5}
SELL = {"action": "sell", "confidence": 85, "stop_pct": -1.0, "target_pct": 2.5}
HOLD = {"action": "hold"}
# Auto trade on. The analyses are the ones a test asks for: left on, the
# application would also analyze whenever a real quarter-hour passes mid-test.
AUTO = "[engine]\nauto_trade=true\nauto_analyze=false\n"
# The forex markets: currency pairs and gold from the stand-in for Kraken's public API.
FOREX = "[market]\nkind=forex\n"


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


@unittest.skipUnless(BIN and os.access(BIN, os.X_OK), "set SHADOW_TRADING_BIN to the shadow-trading binary")
class AppTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="trading-test.")
        self.addCleanup(shutil.rmtree, self.dir, True)
        self.port = free_port()
        self.scenario = os.path.join(self.dir, "scenario.json")
        self.log = os.path.join(self.dir, "requests.jsonl")
        self.server = None
        self.key = None  # NVIDIA_API_KEY of the application; None = not set
        self.stream = False  # True: the application is given the stand-in's live stream
        self.conf_dir = os.path.join(self.dir, "config", "shadow")
        self.data_dir = os.path.join(self.dir, "data", "shadow", "trading")
        self.last_output = ""

    # ---- helpers ---------------------------------------------------------
    def serve(self, plan=None, trend="up"):
        with open(self.scenario, "w", encoding="utf-8") as f:
            json.dump(plan or {}, f)
        self.server = subprocess.Popen(
            [sys.executable, "-B", os.path.join(HERE, "fake_services.py"), "--port", str(self.port),
             "--scenario", self.scenario, "--log", self.log, "--trend", trend],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(self.stop_server)
        for _ in range(100):
            with socket.socket() as s:
                s.settimeout(0.2)
                if s.connect_ex(("127.0.0.1", self.port)) == 0:
                    return
            time.sleep(0.05)
        self.fail("the stand-in services did not start")

    def stop_server(self):
        if self.server and self.server.poll() is None:
            self.server.terminate()
            self.server.wait(5)

    def settings(self, text):
        os.makedirs(self.conf_dir, exist_ok=True)
        with open(os.path.join(self.conf_dir, "trading.conf"), "w", encoding="utf-8") as f:
            f.write(text)

    def env(self):
        base = f"http://127.0.0.1:{self.port}"
        env = {k: v for k, v in os.environ.items()
               if k in ("PATH", "LD_LIBRARY_PATH", "QML_IMPORT_PATH", "QML2_IMPORT_PATH", "LANG", "LC_ALL")}
        env.update({
            "HOME": self.dir, "XDG_CONFIG_HOME": os.path.join(self.dir, "config"),
            "XDG_DATA_HOME": os.path.join(self.dir, "data"), "XDG_RUNTIME_DIR": self.dir,
            "QT_QPA_PLATFORM": "offscreen",
            "SHADOW_TRADING_DATA_URL": base, "SHADOW_TRADING_NVIDIA_URL": base + "/nvidia/v1",
        })
        if self.key is not None:
            env["NVIDIA_API_KEY"] = self.key
        # The stand-in's live stream, for the tests that ask for it; the others
        # get their prices the slow way, every five seconds.
        env["SHADOW_TRADING_STREAM_URL"] = f"ws://127.0.0.1:{self.port}" if self.stream else "off"
        return env

    def dump(self, topic, extra_env=None):
        out = os.path.join(self.dir, "out.json")
        env = self.env()
        env.update(extra_env or {})
        with open(out, "w", encoding="utf-8") as f:
            done = subprocess.run([BIN, "--dump", topic], env=env, stdout=f, stderr=subprocess.PIPE,
                                  timeout=150, text=True)
        with open(out, encoding="utf-8") as f:
            text = f.read()
        self.assertEqual(done.returncode, 0, done.stderr[-2000:])
        self.last_output = text + done.stderr
        return json.loads(text)

    def requests(self, kind=None, model=None, review=False):
        """Logged requests. Of the chat requests, the first calls; review=True
        gives the team review's requests instead, review=None both."""
        if not os.path.exists(self.log):
            return []
        with open(self.log, encoding="utf-8") as f:
            entries = [json.loads(line) for line in f if line.strip()]
        return [e for e in entries
                if (kind is None or e["kind"] == kind) and (model is None or e.get("model") == model)
                and (review is None or e["kind"] != "chat" or bool(e.get("review")) == review)]

    def verdicts(self, signal):
        return {v["source"]: v for v in signal["verdicts"]}

    def expected_consensus(self, signal):
        """The consensus worked out here, independently, from the verdicts."""
        ok = [v for v in signal["verdicts"] if v["ok"]]
        buy = sum(min(max(v["confidence"], 0), 100) for v in ok if v["action"] == "buy")
        sell = sum(min(max(v["confidence"], 0), 100) for v in ok if v["action"] == "sell")
        if not ok or buy == sell:
            return "hold", 0.0
        return ("buy" if buy > sell else "sell"), abs(buy - sell) / len(ok)

    def change_scenario(self, plan):
        with open(self.scenario + ".new", "w", encoding="utf-8") as f:
            json.dump(plan, f)
        os.replace(self.scenario + ".new", self.scenario)

    def act(self, actions, plan, settings="", moves=(), seconds=None):
        """Run a session with the scripted actions; 'moves' is [(after seconds, scenario)]."""
        self.serve(plan)
        self.key = TEST_KEY
        if settings:
            self.settings(settings)

        def mover():
            started = time.time()
            for after, scenario in moves:
                time.sleep(max(0, after - (time.time() - started)))
                self.change_scenario(scenario)

        thread = threading.Thread(target=mover, daemon=True)
        thread.start()
        extra = {"SHADOW_TRADING_SESSION_ACTIONS": actions}
        if seconds:
            extra["SHADOW_TRADING_SESSION_SECONDS"] = str(seconds)
        out = self.dump("session", extra)
        thread.join()
        return out

    # ---- settings and the key ---------------------------------------------------
    def test_config_dump_never_contains_the_key(self):
        self.serve()
        self.key = TEST_KEY
        config = self.dump("config")
        self.assertNotIn(TEST_KEY, self.last_output)
        self.assertNotIn(TEST_KEY[:14], self.last_output)
        self.assertEqual(config["key"], {"hasKey": True, "hint": TEST_KEY[-4:], "source": "environment"})
        self.assertEqual(config["mode"], "paper")
        self.assertFalse(config["autoTrade"])  # off until the user turns it on
        self.assertEqual(config["risk"]["risk_per_trade_pct"], 1)
        # The built-in panel: fourteen NVIDIA-hosted models, six of them the analysis team.
        self.assertEqual({role: len(models) for role, models in PANEL.items()},
                         {"analysis": 6, "reasoning": 3, "vision": 3, "compute": 2})
        self.assertEqual([m["model"] for m in config["panel"]], PANEL_MODELS)
        for role, models in PANEL.items():
            for member in config["panel"]:
                if member["model"] in models:
                    self.assertEqual(member["role"], role)
                    self.assertEqual(member["vision"], role == "vision")
        names = {m["model"]: (m["name"], m["shortName"]) for m in config["panel"]}
        self.assertEqual(names["mistralai/mistral-large-4-1t-a52b"], ("Mistral Large 4", "Mistral"))
        self.assertEqual(names["nvidia/nemotron-3.5-super-vl-120b-a12b"], ("Nemotron 3.5 Super VL", "Nemotron"))
        self.assertEqual(names["z-ai/glm-5.3-flash"], ("GLM 5.3 Flash", "GLM"))
        self.assertEqual(names["minimaxai/minimax-m3.1-flash"][1], "MiniMax")
        # Two sizes of one model are told apart by their size.
        self.assertEqual(names["google/gemma-3-12b-it"][0], "Gemma 3 12B")
        self.assertEqual(names["google/gemma-3-4b-it"][0], "Gemma 3 4B")

    def test_a_key_of_another_company_is_never_sent_to_nvidia(self):
        self.serve({"*": BUY})
        for foreign in ("sk-ant-api03-0000000000000000000000000", "sk-proj-000000000000000000000000000",
                        "AIzaSy00000000000000000000000000000000"):
            self.key = foreign
            config = self.dump("config")
            self.assertFalse(config["key"]["hasKey"], foreign[:6])
            signal = self.dump("signal")["signal"]
            self.assertEqual([v["source"] for v in signal["verdicts"]], ["rules"])
            self.assertNotIn(foreign, self.last_output)
        self.assertEqual(self.requests("chat"), [])
        # The catalog is public and was fetched without any key.
        self.assertTrue(all(e["auth"] == "missing" for e in self.requests("models")))

    def test_a_saved_key_file_is_used_and_made_private(self):
        self.serve({"*": BUY})
        key_dir = os.path.join(self.conf_dir, "trading")
        os.makedirs(key_dir)
        path = os.path.join(key_dir, "nvidia.key")
        with open(path, "w", encoding="utf-8") as f:
            f.write(TEST_KEY + "\n")
        os.chmod(path, 0o644)
        config = self.dump("config")
        self.assertEqual(config["key"]["source"], "saved")
        self.assertEqual(stat.S_IMODE(os.stat(path).st_mode), 0o600)
        signal = self.dump("signal")["signal"]
        self.assertEqual(signal["voters"], 15)
        self.assertTrue(all(c["auth"] == "ok" for c in self.requests("chat", review=None)))

    def test_the_assistants_nvidia_key_is_shared(self):
        self.serve({"*": BUY})
        os.makedirs(self.conf_dir, exist_ok=True)
        assistant = os.path.join(self.conf_dir, "assistant.key")
        with open(assistant, "w", encoding="utf-8") as f:
            f.write("sk-ant-api03-0000000000000000000000000\n")  # the assistant set up with Claude
        self.assertFalse(self.dump("config")["key"]["hasKey"])
        with open(assistant, "w", encoding="utf-8") as f:
            f.write(TEST_KEY + "\n")                              # the assistant set up with NVIDIA
        self.assertEqual(self.dump("config")["key"], {"hasKey": True, "hint": TEST_KEY[-4:], "source": "assistant"})
        self.dump("signal")
        self.assertEqual(len(self.requests("chat")), 14)

    def test_settings_outside_their_range_fall_back(self):
        self.serve()
        self.settings("[risk]\nrisk_per_trade_pct=75\nmin_confidence=abc\nmax_open_positions=2\n"
                      "[paper]\nstarting_balance=-5\n[market]\nsymbol=DROP TABLE\ninterval=7m\n")
        config = self.dump("config")
        self.assertEqual(config["risk"]["risk_per_trade_pct"], 1)
        self.assertEqual(config["risk"]["min_confidence"], 60)
        self.assertEqual(config["risk"]["max_open_positions"], 2)
        self.assertEqual(config["paper"]["starting_balance"], 10000)
        self.assertEqual((config["symbol"], config["interval"]), ("BTCUSDT", "15m"))

    def test_the_panel_can_be_chosen_in_the_settings(self):
        self.serve({"*": BUY})
        self.key = TEST_KEY
        self.settings("[panel]\nanalysis=moonshotai/kimi-k3.1-pro, not a model, ../../etc/passwd\n"
                      "reasoning=\nvision=nvidia/cosmos-reason2-8b, moonshotai/kimi-k3.1-pro\ncompute=\n")
        config = self.dump("config")
        self.assertEqual([(m["model"], m["role"]) for m in config["panel"]],
                         [("moonshotai/kimi-k3.1-pro", "analysis"), ("nvidia/cosmos-reason2-8b", "vision")])
        signal = self.dump("signal")["signal"]
        self.assertEqual(set(self.verdicts(signal)), {"rules", "moonshotai/kimi-k3.1-pro", "nvidia/cosmos-reason2-8b"})
        self.assertEqual(self.requests("chat", "moonshotai/kimi-k3.1-pro")[0]["images"], [])
        self.assertEqual(self.requests("chat", "nvidia/cosmos-reason2-8b")[0]["images"], ["960x540"])

    def test_a_model_switched_off_is_not_asked(self):
        off = ["z-ai/glm-5.3-flash", "nvidia/nemotron-3.5-bolt-e4b"]
        self.serve({"*": BUY})
        self.key = TEST_KEY
        self.settings("[panel]\noff=" + ", ".join(off) + "\n")
        signal = self.dump("signal")["signal"]
        asked = sorted(c["model"] for c in self.requests("chat"))
        self.assertEqual(asked, sorted(m for m in PANEL_MODELS if m not in off))
        self.assertEqual(signal["voters"], 13)

    # ---- market ------------------------------------------------------------------
    def test_market_data_is_read(self):
        self.serve()
        market = self.dump("market")
        self.assertEqual(market["dataState"], "live")
        names = [m["name"] for m in market["watchlist"]]
        self.assertEqual(names, ["BTC/USDT", "ETH/USDT", "SOL/USDT", "PAXG/USDT", "EUR/USDT"])
        self.assertTrue(all(m["hasPrice"] and m["price"] > 0 for m in market["watchlist"]))
        self.assertTrue(market["watchlist"][0]["selected"])
        quote = market["quote"]
        self.assertLessEqual(quote["low"], min(quote["open"], quote["close"]))
        self.assertGreaterEqual(quote["high"], max(quote["open"], quote["close"]))
        self.assertFalse(quote["closed"])        # the newest candle is still forming
        self.assertEqual(quote["candles"], 300)
        self.assertTrue(market["readings"]["taValid"])
        self.assertTrue(market["readings"]["smcValid"])
        self.assertEqual(market["readings"]["trend"], "bullish")  # the stand-in's prices rise
        self.assertEqual(self.requests("chat"), [])  # looking at prices asks no model anything

    def test_no_market_data_is_said_plainly(self):
        self.serve({"market": {"status": 451}, "*": BUY})
        self.key = TEST_KEY
        market = self.dump("market")
        self.assertEqual(market["dataState"], "error")
        self.assertIn("refuses requests from this location", market["dataError"])
        self.assertFalse(market["watchlist"][0]["hasPrice"])
        out = self.dump("signal")
        self.assertEqual(out["signal"]["state"], "none")
        self.assertEqual(self.requests("chat"), [])  # nothing to analyze, nobody asked

    def test_the_watchlist_in_the_settings_file_is_used(self):
        self.serve()
        # With commas (QSettings hands those over as a list), spaces and lower case.
        self.settings("[market]\nwatchlist=ETHUSDT,solusdt, BTCUSDT\n")
        config = self.dump("config")
        self.assertEqual(config["watchlist"], ["ETHUSDT", "SOLUSDT", "BTCUSDT"])
        self.assertEqual(config["symbol"], "ETHUSDT")  # the first one, when none is chosen
        # One market, a name that cannot be a market, a repeated one.
        self.settings("[market]\nwatchlist=SOLUSDT\n")
        self.assertEqual(self.dump("config")["watchlist"], ["SOLUSDT"])
        self.settings("[market]\nwatchlist=SOLUSDT, no/such, SOLUSDT, BTCUSDT\n")
        self.assertEqual(self.dump("config")["watchlist"], ["SOLUSDT", "BTCUSDT"])
        # Nothing usable: the built-in list.
        self.settings("[market]\nwatchlist=?, x\n")
        self.assertEqual(len(self.dump("config")["watchlist"]), 5)

    def test_an_unknown_market_does_not_take_the_prices_away(self):
        # The real service refuses a whole list of tickers for one unknown
        # name, without saying which one.
        out = self.act("", {}, seconds=7,
                       settings="[market]\nwatchlist=BTCUSDT, NOPEUSDT, ETHUSDT\n[engine]\nauto_analyze=false\n")
        market = out["market"]
        self.assertEqual(market["dataState"], "live", market["dataError"])
        listed = {m["symbol"]: m for m in market["watchlist"]}
        self.assertEqual(list(listed), ["BTCUSDT", "NOPEUSDT", "ETHUSDT"])
        for symbol in ("BTCUSDT", "ETHUSDT"):
            self.assertTrue(listed[symbol]["hasPrice"] and listed[symbol]["listed"], symbol)
            self.assertGreater(listed[symbol]["price"], 0)
        self.assertFalse(listed["NOPEUSDT"]["hasPrice"])
        self.assertFalse(listed["NOPEUSDT"]["listed"])
        # Said once, by name, however many rounds of prices went by.
        said = [a["text"] for a in out["account"]["activity"] if "NOPEUSDT" in a["text"]]
        self.assertEqual(len(said), 1, said)
        self.assertIn("is not a market the data service lists", said[0])
        # The list was refused once; each market was then asked on its own;
        # and the unknown one was left out from then on.
        prices = [r for r in self.requests("market") if r["path"] == "/api/v3/ticker/24hr"]
        refused = [r for r in prices if r["status"] == 400]
        self.assertEqual(len([r for r in refused if "symbols=" in r["query"]]), 1, prices)
        self.assertEqual([r["query"] for r in refused if "symbols=" not in r["query"]], ["symbol=NOPEUSDT"])
        self.assertEqual(sorted(r["query"] for r in prices if r["status"] == 200 and "symbols=" not in r["query"]),
                         ["symbol=BTCUSDT", "symbol=ETHUSDT"])
        later = [r for r in prices if r["status"] == 200 and "symbols=" in r["query"]]
        self.assertGreaterEqual(len(later), 1, prices)
        self.assertTrue(all("NOPEUSDT" not in r["query"] for r in later))
        # Its 24-hour line was asked for once and not again.
        lines = [r for r in self.requests("market") if r["path"] == "/api/v3/klines" and "NOPEUSDT" in r["query"]]
        self.assertLessEqual(len(lines), 1, lines)

    def test_an_unknown_market_on_the_chart_is_named(self):
        out = self.act("analyze", {"*": BUY}, seconds=12,
                       settings="[market]\nwatchlist=NOPEUSDT, BTCUSDT\n[engine]\nauto_analyze=false\n")
        self.assertEqual(out["actions"], [{"action": "analyze", "result": "not started"}])
        market = out["market"]
        self.assertEqual(market["dataState"], "error")
        self.assertIn("NOPEUSDT is not a market the data service lists", market["dataError"])
        listed = {m["symbol"]: m for m in market["watchlist"]}
        self.assertTrue(listed["NOPEUSDT"]["selected"])
        self.assertFalse(listed["NOPEUSDT"]["listed"])
        self.assertTrue(listed["BTCUSDT"]["hasPrice"])  # the other market's price still arrives
        # Three rounds of prices later it has still been said only once: the
        # working prices do not clear the chart's error for it to come back.
        errors = [a["text"] for a in out["account"]["activity"] if a["kind"] == "error"]
        self.assertEqual(len(errors), 1, errors)
        self.assertEqual(out["signal"]["state"], "none")
        self.assertEqual(self.requests("chat"), [])  # nothing to analyze, nobody asked
        asked = [r for r in self.requests("market") if r["path"] == "/api/v3/klines" and "NOPEUSDT" in r["query"]]
        self.assertLessEqual(len(asked), 3, asked)  # chart, higher timeframe and 24-hour line: once each

    def test_prices_are_matched_by_name_not_by_position(self):
        # The service answers a list in its own order (EUR before SOL here).
        self.serve()
        market = self.dump("market")
        prices = {m["symbol"]: m["price"] for m in market["watchlist"]}
        self.assertLess(prices["EURUSDT"], 5)
        self.assertTrue(50 < prices["SOLUSDT"] < 1000, prices)
        self.assertGreater(prices["PAXGUSDT"], 1000)

    # ---- the live stream ------------------------------------------------------------
    def market_requests(self, path):
        return [r for r in self.requests("market") if r["path"] == path]

    def test_the_live_stream_feeds_the_chart_and_the_prices(self):
        self.stream = True
        # Two seconds in, the price jumps by 3 %. Nothing is asked for again
        # before the session ends, so only the stream can have told.
        out = self.act("", {}, seconds=4, settings="[engine]\nauto_analyze=false\n",
                       moves=[(2, {"market": {"move_pct": 3}})])
        market = out["market"]
        self.assertTrue(market["streaming"])
        self.assertEqual(market["dataState"], "live")
        streams = [r for r in self.requests("stream") if r["status"] == 101]
        self.assertEqual(len(streams), 1)
        self.assertEqual(streams[0]["streams"], ["btcusdt@aggTrade", "btcusdt@kline_15m", "btcusdt@miniTicker",
                                                 "ethusdt@miniTicker", "solusdt@miniTicker", "paxgusdt@miniTicker",
                                                 "eurusdt@miniTicker"])
        self.assertEqual(len(self.market_requests("/api/v3/ticker/24hr")), 1)   # the first look, then the stream
        quote = market["quote"]
        self.assertGreater(quote["close"] / quote["open"], 1.02)
        self.assertGreaterEqual(quote["high"], quote["close"])
        self.assertFalse(quote["closed"])
        self.assertEqual(quote["candles"], 300)              # the analysts' window is what it was
        prices = {m["symbol"]: m for m in market["watchlist"]}
        self.assertAlmostEqual(prices["BTCUSDT"]["price"] / quote["close"], 1, delta=0.002)
        self.assertTrue(all(m["hasPrice"] for m in market["watchlist"]))
        said = [a["text"] for a in out["account"]["activity"] if "Live prices" in a["text"]]
        self.assertEqual(len(said), 1, said)
        self.assertIn("connected", said[0])

    def test_a_stop_is_hit_by_the_stream_at_once(self):
        self.stream = True
        out = self.act("analyze", {"*": BUY}, settings=AUTO, seconds=6,
                       moves=[(4, {"*": BUY, "market": {"move_pct": -3}})])
        trades = out["account"]["trades"]
        self.assertEqual(len(trades), 1, out["account"]["activity"])
        self.assertEqual(trades[0]["reason"], "stop_loss")
        self.assertEqual(out["account"]["positions"], [])
        self.assertTrue(out["market"]["streaming"])
        # Nobody asked for the newest candles in the meantime: the stream told.
        self.assertEqual([r for r in self.market_requests("/api/v3/klines") if r["query"].endswith("limit=3")], [])

    def test_without_a_stream_prices_are_asked_for(self):
        self.stream = True
        out = self.act("", {"stream": {"off": True}}, seconds=7, settings="[engine]\nauto_analyze=false\n")
        self.assertFalse(out["market"]["streaming"])
        self.assertEqual(out["market"]["dataState"], "live")
        self.assertEqual(out["market"]["dataError"], "")
        self.assertGreaterEqual(len(self.market_requests("/api/v3/ticker/24hr")), 2)   # at the start and 5 s later
        self.assertGreaterEqual(len([r for r in self.requests("stream") if r["status"] == 404]), 2)  # it keeps trying
        self.assertFalse(any("Live prices" in a["text"] for a in out["account"]["activity"]))

    def test_a_stream_that_drops_comes_back(self):
        self.stream = True
        normal, dropping = {}, {"stream": {"drop_after": 0}}
        out = self.act("", normal, seconds=9, settings="[engine]\nauto_analyze=false\n",
                       moves=[(2, dropping), (4, normal)])
        self.assertTrue(out["market"]["streaming"])
        log = self.requests("stream")
        self.assertGreaterEqual(len([r for r in log if r["status"] == "dropped"]), 1, log)
        self.assertGreaterEqual(len([r for r in log if r["status"] == 101]), 2, log)
        # Coming and going is not written down each time.
        said = [a["text"] for a in out["account"]["activity"] if "Live prices" in a["text"]]
        self.assertEqual(len(said), 1, said)

    def test_the_real_services_stream_messages_are_understood(self):
        # Messages recorded from the real public stream, replayed byte for byte.
        self.stream = True
        recorded = os.path.join(HERE, "fixtures", "real-stream.jsonl")
        # They arrive a moment after the history and the first prices were read.
        out = self.act("", {"stream": {"replay": recorded, "replay_after": 1.5, "silent": True}}, seconds=3,
                       settings="[engine]\nauto_analyze=false\n")
        last = {}
        with open(recorded, encoding="utf-8") as f:
            for line in f:
                message = json.loads(line)
                last[message["stream"]] = message["data"]
        market = out["market"]
        self.assertTrue(market["streaming"])
        prices = {m["symbol"]: m for m in market["watchlist"]}
        for symbol in ("BTCUSDT", "ETHUSDT", "EURUSDT"):
            ticker = last[symbol.lower() + "@miniTicker"]
            self.assertEqual(prices[symbol]["price"], float(ticker["c"]), symbol)
            self.assertAlmostEqual(prices[symbol]["changePct"],
                                   (float(ticker["c"]) - float(ticker["o"])) / float(ticker["o"]) * 100, places=6)
        self.assertGreater(prices["BTCUSDT"]["price"], 1000)
        # The last recorded trade is the price of the forming candle.
        self.assertEqual(market["quote"]["close"], float(last["btcusdt@aggTrade"]["p"]))
        self.assertEqual(market["quote"]["candles"], 300)   # the recorded candle is older than the chart: left out

    # ---- the chart's history and the markets ------------------------------------------
    def test_older_candles_are_loaded_for_the_chart_only(self):
        out = self.act("older,older", {}, settings="[engine]\nauto_analyze=false\n")
        self.assertEqual([a["result"] for a in out["actions"]], ["1000", "1000"])
        chart = out["market"]["chart"]
        self.assertEqual(chart["candles"], 2300)
        self.assertTrue(chart["hasOlder"])
        step = 900_000
        self.assertEqual((chart["last"] - chart["first"]) / step, 2299)   # one candle after the other, no gap
        self.assertEqual(out["market"]["quote"]["candles"], 300)          # what the analysts read is unchanged
        asked = [r["query"] for r in self.market_requests("/api/v3/klines") if "endTime" in r["query"]]
        self.assertEqual(len(asked), 2)

    def test_the_beginning_of_a_markets_history(self):
        # The stand-in has 2000 hours of history: 500 four-hour candles.
        out = self.act("older,older,older", {}, settings="[market]\ninterval=4h\n[engine]\nauto_analyze=false\n")
        got = [a["result"] for a in out["actions"]]
        self.assertIn(got[0], ("200", "201"))         # the rest of the history (its first candle may be partial)
        self.assertEqual(got[1:], ["0", "0"])
        self.assertEqual(out["market"]["chart"]["candles"], 300 + int(got[0]))
        self.assertFalse(out["market"]["chart"]["hasOlder"])
        asked = [r["query"] for r in self.market_requests("/api/v3/klines") if "endTime" in r["query"]]
        self.assertEqual(len(asked), 2)          # once it is known there is no more, nothing is asked

    def test_markets_are_added_and_removed_from_the_window(self):
        out = self.act("find:doge,add:doge/usdt,add:DOGEUSDT,add:NOPEUSDT,add:bad name!,remove:ETHUSDT,"
                       "remove:BTCUSDT,wait:1", {}, settings="[engine]\nauto_analyze=false\n")
        results = [a["result"] for a in out["actions"]]
        self.assertEqual(results[0], "DOGEUSDT")
        self.assertEqual(results[1], "")
        self.assertIn("already in the list", results[2])
        self.assertIn("not a market the data service lists", results[3])
        self.assertIn("not a market", results[4])
        self.assertEqual(results[5:7], ["", ""])
        listed = out["market"]["watchlist"]
        self.assertEqual([m["symbol"] for m in listed], ["SOLUSDT", "PAXGUSDT", "EURUSDT", "DOGEUSDT"])
        self.assertTrue(listed[0]["selected"])             # the market on the chart was removed: the first one is
        self.assertTrue(all(m["hasPrice"] for m in listed), listed)
        self.assertEqual(out["market"]["quote"]["candles"], 300)
        self.assertLess(out["market"]["quote"]["close"], 1000)   # SOL's candles, not BTC's
        config = self.dump("config")                       # kept for the next start
        self.assertEqual(config["watchlist"], ["SOLUSDT", "PAXGUSDT", "EURUSDT", "DOGEUSDT"])
        self.assertEqual(config["symbol"], "SOLUSDT")

    def test_what_cannot_be_removed_or_added(self):
        out = self.act("analyze,remove:BTCUSDT", {"*": BUY}, settings=AUTO)
        self.assertIn("open position", out["actions"][1]["result"])
        self.assertEqual(len(out["market"]["watchlist"]), 5)
        self.settings("[market]\nwatchlist=BTCUSDT\n[engine]\nauto_analyze=false\n")
        out = self.act("close-all,remove:BTCUSDT", {})
        self.assertIn("last market", out["actions"][1]["result"])
        full = ",".join(["BTCUSDT", "ETHUSDT", "SOLUSDT", "PAXGUSDT", "EURUSDT", "DOGEUSDT", "XRPUSDT", "BNBUSDT",
                         "ETHBTC", "AAAUSDT", "BBBUSDT", "CCCUSDT"])
        self.settings(f"[market]\nwatchlist={full}\n[engine]\nauto_analyze=false\n")
        out = self.act("add:DDDUSDT", {})
        self.assertIn("12 markets at most", out["actions"][0]["result"])

    # ---- the analysts ---------------------------------------------------------------
    def test_without_a_key_the_rules_decide_alone(self):
        self.serve({"*": BUY})
        signal = self.dump("signal")["signal"]
        self.assertEqual(signal["state"], "ready")
        self.assertEqual([v["source"] for v in signal["verdicts"]], ["rules"])
        self.assertEqual(signal["voters"], 1)
        action, confidence = self.expected_consensus(signal)
        self.assertEqual(signal["action"], action)
        self.assertAlmostEqual(signal["confidence"], confidence, places=6)
        self.assertEqual(self.requests("chat"), [])

    def test_every_model_is_asked_and_the_trade_is_placed(self):
        self.serve({"*": BUY})
        self.key = TEST_KEY
        self.settings(AUTO)
        out = self.dump("signal")
        signal = out["signal"]
        verdicts = self.verdicts(signal)
        self.assertEqual(set(verdicts), {"rules", *PANEL_MODELS})
        for model in PANEL_MODELS:
            self.assertTrue(verdicts[model]["ok"], verdicts[model])
            self.assertEqual((verdicts[model]["action"], verdicts[model]["confidence"]), ("buy", 85))
        for role, models in PANEL.items():
            for model in models:
                self.assertEqual(verdicts[model]["role"], role)
        self.assertEqual(signal["voters"], 15)
        action, confidence = self.expected_consensus(signal)
        self.assertEqual((signal["action"], action), ("buy", "buy"))
        self.assertAlmostEqual(signal["confidence"], confidence, places=6)
        self.assertGreaterEqual(signal["confidence"], 60)
        # Levels: the middle of what the agreeing analysts proposed.
        agreeing = [v for v in signal["verdicts"] if v["ok"] and v["action"] == "buy" and v["stop"] > 0]
        self.assertAlmostEqual(signal["stop"], statistics.median(v["stop"] for v in agreeing), places=4)
        self.assertAlmostEqual(signal["takeProfit"], statistics.median(v["takeProfit"] for v in agreeing), places=4)
        self.assertLess(signal["stop"], signal["entry"])
        self.assertGreater(signal["takeProfit"], signal["entry"])
        self.assertTrue(signal["traded"], signal["outcome"])

        positions = out["account"]["positions"]
        self.assertEqual(len(positions), 1)
        position = positions[0]
        self.assertEqual((position["symbol"], position["side"]), ("BTCUSDT", "buy"))
        self.assertAlmostEqual(position["stop"], signal["stop"], places=6)
        self.assertAlmostEqual(position["takeProfit"], signal["takeProfit"], places=6)
        # A 1 % stop would need the whole account to put 1 % at risk; with
        # three positions allowed, one takes a third of it at most.
        self.assertAlmostEqual(position["entry"] * position["quantity"], 10000 / 3, delta=40)
        at_risk = (position["entry"] - position["stop"]) * position["quantity"]
        self.assertAlmostEqual(at_risk, 100 / 3, delta=2)
        self.assertTrue(signal["risk"]["reducedByBalance"])
        account = out["account"]["account"]
        self.assertLess(account["balance"], 10000)  # the opening fee
        self.assertGreater(account["balance"], 9990)

        # What NVIDIA's service received: one question per model, each with the key.
        chats = self.requests("chat")
        self.assertEqual(sorted(c["model"] for c in chats), sorted(PANEL_MODELS))
        for chat in chats:
            self.assertEqual((chat["auth"], chat["status"]), ("ok", 200), chat)
            self.assertTrue(chat["system"])
            self.assertEqual(chat["temperature"], 0.2)
            self.assertNotIn("stream_options", chat["params"])
        # Only the models that read pictures get the chart, and every role gets its skill briefs.
        for role, models in PANEL.items():
            for model in models:
                chat = self.requests("chat", model)[0]
                self.assertEqual(chat["images"], ["960x540"] if role == "vision" else [], model)
        briefs = {"analysis": ["risk", "smc", "ta"], "reasoning": ["risk", "smc", "ta"],
                  "vision": ["chart", "risk", "smc"], "compute": ["risk", "ta"]}
        for role, models in PANEL.items():
            self.assertEqual(self.requests("chat", models[0])[0]["skills"], briefs[role], role)

        # The account survives a restart, and a second signal does not double the position.
        saved = os.path.join(self.data_dir, "paper.json")
        self.assertEqual(stat.S_IMODE(os.stat(saved).st_mode), 0o600)
        again = self.dump("signal")
        self.assertEqual(len(again["account"]["positions"]), 1)
        self.assertFalse(again["signal"]["traded"])
        checks = {c["key"]: c for c in again["signal"]["risk"]["checks"]}
        self.assertFalse(checks["positions"]["pass"])
        self.assertIn("Already in a position", again["signal"]["outcome"])

    def test_auto_trade_off_places_nothing(self):
        self.serve({"*": BUY})
        self.key = TEST_KEY
        out = self.dump("signal")
        self.assertEqual(out["signal"]["action"], "buy")
        self.assertTrue(out["signal"]["risk"]["allowed"])
        self.assertFalse(out["signal"]["traded"])
        self.assertTrue(out["signal"]["placeable"])
        self.assertEqual(out["account"]["positions"], [])

    def test_a_falling_market_is_sold_short(self):
        self.serve({"*": SELL}, trend="down")
        self.key = TEST_KEY
        self.settings(AUTO)
        out = self.dump("signal")
        signal = out["signal"]
        self.assertEqual(signal["action"], "sell")
        self.assertGreater(signal["stop"], signal["entry"])
        self.assertLess(signal["takeProfit"], signal["entry"])
        self.assertTrue(signal["traded"], signal["outcome"])
        position = out["account"]["positions"][0]
        self.assertEqual(position["side"], "sell")
        self.assertGreater(position["stop"], position["entry"])

    def test_an_open_position_stays_on_show_when_the_next_analysis_says_hold(self):
        # The trade is placed; then the analysts see nothing more and say hold.
        # The signal is hold, but the position is still there for the chart
        # and the panel, with its own entry, stop and target.
        out = self.act("analyze,wait:3,analyze,select:ETHUSDT,select:BTCUSDT", {"*": BUY}, settings=AUTO,
                       moves=[(2.5, {"*": HOLD})])
        self.assertEqual(out["signal"]["state"], "none")          # another market was looked at in between
        positions = out["account"]["positions"]
        self.assertEqual(len(positions), 1)
        shown = out["account"]["position"]
        self.assertEqual(shown["id"], positions[0]["id"])
        self.assertEqual((shown["symbol"], shown["side"]), ("BTCUSDT", "buy"))
        self.assertEqual((shown["entry"], shown["stop"], shown["takeProfit"]),
                         (positions[0]["entry"], positions[0]["stop"], positions[0]["takeProfit"]))
        said = [a["text"] for a in out["account"]["activity"]]
        self.assertTrue(any("no trade" in text for text in said), said)   # the second analysis did say hold

    def test_the_position_shown_is_the_selected_markets(self):
        out = self.act("analyze,select:ETHUSDT", {"*": BUY}, settings=AUTO)
        self.assertEqual(len(out["account"]["positions"]), 1)     # on BTC/USDT
        self.assertEqual(out["account"]["position"], {})          # ETH/USDT is on the chart: nothing open there

    # ---- forex -------------------------------------------------------------------

    def test_the_forex_markets_come_from_the_forex_service(self):
        self.serve()
        self.assertEqual(self.dump("config")["marketKind"], "crypto")       # unless the settings say otherwise
        self.settings(FOREX)
        config = self.dump("config")
        self.assertEqual((config["marketKind"], config["accountCurrency"]), ("forex", "USD"))
        self.assertEqual(config["watchlist"], ["EURUSD", "GBPUSD", "AUDUSD", "USDCAD", "USDCHF", "PAXGUSD"])
        self.assertEqual(config["symbol"], "EURUSD")
        before = len(self.requests("market"))
        market = self.dump("market")
        listed = {m["symbol"]: m for m in market["watchlist"]}
        self.assertEqual([listed[s]["name"] for s in ("EURUSD", "USDCHF", "PAXGUSD")],
                         ["EUR/USD", "USD/CHF", "PAXG/USD"])
        self.assertTrue(all(m["hasPrice"] for m in market["watchlist"]), market["watchlist"])
        self.assertAlmostEqual(listed["EURUSD"]["price"], 1.0982, delta=0.04)
        self.assertAlmostEqual(listed["USDCAD"]["price"], 1.4201, delta=0.05)   # keyed "ZUSDZCAD" by the service
        self.assertEqual(market["quote"]["candles"], 300)
        self.assertAlmostEqual(market["quote"]["close"], listed["EURUSD"]["price"], delta=0.01)
        asked = self.requests("market")[before:]
        self.assertEqual({r["path"] for r in asked}, {"/0/public/OHLC", "/0/public/Ticker"})
        self.assertIn("pair=EURUSD&interval=15", [r["query"] for r in asked])
        self.assertTrue(all(r["status"] == 200 for r in asked), asked)

    def test_the_forex_stream_is_subscribed_to_once_connected(self):
        self.stream = True
        out = self.act("", {}, seconds=4, settings=FOREX + "[engine]\nauto_analyze=false\n",
                       moves=[(2, {"market": {"move_pct": 2}})])
        market = out["market"]
        self.assertTrue(market["streaming"])
        subscribed = {r["channel"]: r for r in self.requests("subscribe")}
        self.assertEqual((subscribed["trade"]["symbols"], subscribed["trade"]["snapshot"]), (["EUR/USD"], False))
        self.assertEqual((subscribed["ohlc"]["symbols"], subscribed["ohlc"]["interval"]), (["EUR/USD"], 15))
        self.assertEqual(subscribed["ticker"]["symbols"],
                         ["EUR/USD", "GBP/USD", "AUD/USD", "USD/CAD", "USD/CHF", "PAXG/USD"])
        # The jump two seconds in can only have come through the stream.
        self.assertGreater(market["quote"]["close"], 1.0982 * 1.012)
        listed = {m["symbol"]: m for m in market["watchlist"]}
        self.assertGreater(listed["USDCHF"]["price"], 0.8311 * 1.012)
        self.assertEqual(len(self.market_requests("/0/public/Ticker")), 1)   # the first look, then the stream

    def test_recorded_messages_of_the_real_forex_stream_are_understood(self):
        # Messages recorded from the real public stream, replayed byte for byte.
        self.stream = True
        recorded = os.path.join(HERE, "fixtures", "real-kraken-stream.jsonl")
        out = self.act("", {"stream": {"replay": recorded, "replay_after": 1.5, "silent": True}}, seconds=3,
                       settings=FOREX + "[engine]\nauto_analyze=false\n")
        tickers, trade = {}, None
        with open(recorded, encoding="utf-8") as f:
            for line in f:
                message = json.loads(line)
                for row in message.get("data", []) if message.get("channel") == "ticker" else []:
                    tickers[row["symbol"]] = row
                if message.get("channel") == "trade":
                    trade = message["data"][-1]
        self.assertEqual(sorted(tickers), ["EUR/USD", "GBP/USD", "USD/CHF"])
        market = out["market"]
        self.assertTrue(market["streaming"])
        listed = {m["symbol"]: m for m in market["watchlist"]}
        for pair, symbol in (("EUR/USD", "EURUSD"), ("GBP/USD", "GBPUSD"), ("USD/CHF", "USDCHF")):
            self.assertEqual(listed[symbol]["price"], tickers[pair]["last"], pair)
            self.assertEqual(listed[symbol]["changePct"], tickers[pair]["change_pct"], pair)
        # The last recorded trade is the price of the forming candle.
        self.assertEqual(market["quote"]["close"], trade["price"])
        self.assertEqual(market["quote"]["candles"], 300)   # the recorded candle is older than the chart: left out

    def test_recorded_answers_of_the_real_forex_service_are_understood(self):
        # What the real service answered on 25 November 2026: its ticker of
        # every currency pair and gold token, and its 15-minute candles of EUR/USD.
        fixtures = os.path.join(HERE, "fixtures")
        with open(os.path.join(fixtures, "real-kraken-ticker.json"), encoding="utf-8") as f:
            ticker = json.load(f)["result"]
        with open(os.path.join(fixtures, "real-kraken-ohlc.json"), encoding="utf-8") as f:
            rows = json.load(f)["result"]["ZEURZUSD"]
        plan = {"kraken": {"ticker": os.path.join(fixtures, "real-kraken-ticker.json"),
                           "ohlc": os.path.join(fixtures, "real-kraken-ohlc.json")}}
        out = self.act("find:", plan, settings=FOREX + "[engine]\nauto_analyze=false\n")
        # Offered: the dollar pairs and gold with at least 500 trades that day.
        offered = out["actions"][0]["result"].split()
        self.assertEqual(offered, ["AUDUSD", "EURUSD", "GBPUSD", "PAXGUSD", "USDCAD", "USDCHF", "XAUTUSD"])
        self.assertLess(ticker["ZUSDZJPY"]["t"][1], 500)             # why USD/JPY is not among them
        self.assertGreater(ticker["EURGBP"]["t"][1], 500)            # a cross is left out whatever it trades
        # The candles: the newest 300 of the recording, to the last digit.
        quote = out["market"]["quote"]
        self.assertEqual(quote["candles"], 300)
        self.assertEqual((quote["open"], quote["high"], quote["low"], quote["close"]),
                         tuple(float(v) for v in rows[-1][1:5]))
        chart = out["market"]["chart"]
        self.assertEqual((chart["first"], chart["last"]), (rows[-300][0] * 1000, rows[-1][0] * 1000))

    def test_forex_offers_dollar_pairs_and_gold_that_trade_often_enough(self):
        out = self.act("find:,find:jpy,add:XAUTUSD,add:USDJPY,add:EURGBP,add:XBTUSD,add:BTCUSDT,remove:AUDUSD,wait:1",
                       {}, settings=FOREX + "[engine]\nauto_analyze=false\n")
        results = {a["action"]: a["result"] for a in out["actions"]}
        # Of everything the service lists: not the pair with a few trades a
        # day (USD/JPY), not the cross (EUR/GBP), not the coin (XBT/USD).
        self.assertEqual(results["find:"].split(),
                         ["AUDUSD", "EURUSD", "GBPUSD", "PAXGUSD", "USDCAD", "USDCHF", "XAUTUSD"])
        self.assertEqual(results["find:jpy"], "")
        self.assertEqual(results["add:XAUTUSD"], "")
        self.assertIn("too rarely", results["add:USDJPY"])
        for name in ("EURGBP", "XBTUSD", "BTCUSDT"):
            self.assertIn("only currency pairs with the US dollar", results["add:" + name], name)
        self.assertEqual(results["remove:AUDUSD"], "")
        listed = out["market"]["watchlist"]
        self.assertEqual([m["symbol"] for m in listed], ["EURUSD", "GBPUSD", "USDCAD", "USDCHF", "PAXGUSD", "XAUTUSD"])
        self.assertTrue(all(m["hasPrice"] for m in listed), listed)
        # The forex list is kept apart from the crypto list.
        self.assertEqual(self.dump("config")["watchlist"], ["EURUSD", "GBPUSD", "USDCAD", "USDCHF", "PAXGUSD", "XAUTUSD"])
        with open(os.path.join(self.conf_dir, "trading.conf"), encoding="utf-8") as f:
            saved = f.read()
        self.assertIn("forex_watchlist=", saved)
        self.assertNotIn("\nwatchlist=", saved)

    def test_a_forex_chart_goes_back_as_far_as_the_service_keeps(self):
        out = self.act("older", {}, settings=FOREX + "[engine]\nauto_analyze=false\n")
        chart = out["market"]["chart"]
        self.assertAlmostEqual(chart["candles"], 720, delta=2)     # all the service has of this interval
        self.assertTrue(chart["hasOlder"])                          # not known yet that this is all
        out = self.act("older,older", {}, settings=FOREX + "[engine]\nauto_analyze=false\n")
        chart = out["market"]["chart"]
        self.assertAlmostEqual(chart["candles"], 720, delta=2)
        self.assertFalse(chart["hasOlder"])
        self.assertEqual(out["market"]["quote"]["candles"], 300)   # the analysts' window is unchanged

    def test_a_trade_on_a_dollar_base_pair_is_counted_in_dollars(self):
        # USD/CHF on the dollar account: the quantity is dollars, and a profit
        # in francs is turned back into dollars at the price it is taken at.
        plan = {"*": BUY}
        settings = "[market]\nkind=forex\nforex_symbol=USDCHF\n" + AUTO
        out = self.act("analyze", plan, settings, moves=[(4, dict(plan, market={"move_pct": 0.3}))], seconds=9)
        self.assertEqual(out["signal"]["symbol"], "USDCHF")
        position = out["account"]["positions"][0]
        self.assertEqual((position["symbol"], position["name"], position["side"]), ("USDCHF", "USD/CHF", "buy"))
        # A third of the account at most, in dollars: the quantity itself.
        self.assertAlmostEqual(position["value"], position["quantity"], places=6)
        self.assertAlmostEqual(position["quantity"], 10000 / 3, delta=40)
        # A one per cent stop on a third of the account: a third of a per cent of it is at risk, in dollars.
        at_stop = (position["entry"] - position["stop"]) * position["quantity"] / position["stop"]
        self.assertAlmostEqual(at_stop, 100 / 3, delta=1.5)
        self.assertTrue(out["signal"]["risk"]["reducedByBalance"])
        expected = (position["price"] - position["entry"]) * position["quantity"] / position["price"]
        self.assertAlmostEqual(position["pnl"], expected, places=6)
        self.assertGreater(position["pnl"], 0)
        # And at the target.
        out = self.act("wait:1", plan, settings, moves=[(1, dict(plan, market={"move_pct": 3.5}))], seconds=8)
        self.assertEqual(out["account"]["positions"], [])
        trade = out["account"]["trades"][0]
        self.assertEqual(trade["reason"], "take_profit")
        gross = (trade["exit"] - trade["entry"]) * trade["quantity"] / trade["exit"]
        fees = 2 * trade["quantity"] * 0.001
        self.assertAlmostEqual(trade["fees"], fees, places=6)
        self.assertAlmostEqual(trade["pnl"], gross - fees, places=6)
        self.assertAlmostEqual(out["account"]["account"]["balance"], 10000 + gross - fees, places=4)

    def test_switching_between_crypto_and_forex(self):
        out = self.act("kind:forex,kind:forex,analyze,kind:crypto,close-all,kind:crypto,kind:metals", {"*": BUY},
                       settings=AUTO)
        results = [a["result"] for a in out["actions"]]
        self.assertEqual(results[:2], ["", ""])
        self.assertIn("Close the open positions first", results[3])    # a position on EUR/USD is open
        self.assertEqual(results[5], "")
        self.assertIn("Unknown kind", results[6])
        trade = out["account"]["trades"][0]
        self.assertEqual((trade["symbol"], trade["name"]), ("EURUSD", "EUR/USD"))
        self.assertEqual([m["symbol"] for m in out["market"]["watchlist"]],
                         ["BTCUSDT", "ETHUSDT", "SOLUSDT", "PAXGUSDT", "EURUSDT"])
        self.assertGreater(out["market"]["quote"]["close"], 1000)         # BTC/USDT is on the chart again
        said = [a["text"] for a in out["account"]["activity"]]
        self.assertTrue(any("Showing the forex markets" in text for text in said), said)
        self.assertTrue(any("Showing the crypto markets" in text for text in said), said)
        config = self.dump("config")
        self.assertEqual((config["marketKind"], config["accountCurrency"]), ("crypto", "USDT"))
        paths = {r["path"] for r in self.requests("market")}
        self.assertTrue({"/api/v3/klines", "/0/public/OHLC"} <= paths, paths)

    # ---- the analysis team -------------------------------------------------------

    def test_the_analysis_team_reads_from_six_seats_and_reviews_its_calls(self):
        self.serve({"*": BUY})
        self.key = TEST_KEY
        out = self.dump("signal")
        signal = out["signal"]
        team = PANEL["analysis"]
        self.assertEqual(len(team), 6)
        # First calls: each member of the team is told its own seat, nobody else has one.
        first = {c["model"]: c for c in self.requests("chat")}
        self.assertEqual([first[model]["seat"] for model in team], SEATS)
        for model in PANEL_MODELS:
            if model not in team:
                self.assertEqual(first[model]["seat"], "", model)
        # The review: every member once more, with its own call and the five others', by seat.
        reviews = self.requests("chat", review=True)
        self.assertEqual(sorted(r["model"] for r in reviews), sorted(team))
        for review in reviews:
            seat = SEATS[team.index(review["model"])]
            self.assertEqual((review["status"], review["seat"]), (200, seat), review)
            self.assertNotIn("malformed", review)
            self.assertEqual(review["own"], [seat, "buy"])
            self.assertEqual(sorted(review["teammates"]), sorted([other, "buy"] for other in SEATS if other != seat))
            self.assertEqual(review["images"], [])
        # One request per model, plus one per member of the team.
        self.assertEqual(len(self.requests("chat", review=None)), len(PANEL_MODELS) + 6)
        verdicts = self.verdicts(signal)
        for index, model in enumerate(team):
            verdict = verdicts[model]
            self.assertEqual((verdict["seatTitle"], verdict["reviewed"], verdict["changed"]), (SEATS[index], True, False))
            self.assertEqual((verdict["action"], verdict["firstAction"]), ("buy", "buy"))
        for model in PANEL_MODELS:
            if model not in team:
                self.assertEqual((verdicts[model]["seat"], verdicts[model]["reviewed"]), ("", False), model)
        self.assertEqual(signal["team"], {"members": 6, "buy": 6, "sell": 0, "hold": 0, "reviewed": 6, "changed": 0})
        # Listed as the panel is: the fixed rules, then the team by seat, then the other roles.
        self.assertEqual([v["source"] for v in signal["verdicts"]], ["rules", *PANEL_MODELS])
        # A member still has one vote: fifteen analysts, as without the review.
        self.assertEqual(signal["voters"], 15)
        said = [a["text"] for a in out["account"]["activity"] if a["text"].startswith("Analysis team")]
        self.assertEqual(said, ["Analysis team (6 analysts): 6 buy, 0 sell, 0 hold. "
                                "Nobody changed its call in the team review."])
        # The seats are in the configuration too, for the Model Panel.
        panel = {m["model"]: m for m in self.dump("config")["panel"]}
        self.assertEqual([panel[model]["seatTitle"] for model in team], SEATS)

    def test_a_member_can_change_its_call_in_the_review_and_the_final_calls_count(self):
        team = PANEL["analysis"]
        # Two of the team call sell first; one of them is convinced by its teammates, one is not.
        plan = {"*": BUY, team[1]: dict(SELL, review=dict(BUY, confidence=70, reasoning="The structure seat is right.")),
                team[4]: SELL}
        self.serve(plan)
        self.key = TEST_KEY
        out = self.dump("signal")
        signal = out["signal"]
        verdicts = self.verdicts(signal)
        changed = verdicts[team[1]]
        self.assertEqual((changed["action"], changed["confidence"], changed["firstAction"], changed["firstConfidence"]),
                         ("buy", 70, "sell", 85))
        self.assertTrue(changed["reviewed"] and changed["changed"])
        self.assertEqual(changed["reasoning"], "The structure seat is right.")
        self.assertGreater(changed["stop"], 0)
        self.assertLess(changed["stop"], signal["entry"])            # the final call's levels: a buy's
        kept = verdicts[team[4]]
        self.assertEqual((kept["action"], kept["reviewed"], kept["changed"]), ("sell", True, False))
        self.assertEqual(signal["team"], {"members": 6, "buy": 5, "sell": 1, "hold": 0, "reviewed": 6, "changed": 1})
        # What its teammates were shown: the first calls.
        review = self.requests("chat", team[0], review=True)[0]
        self.assertIn([SEATS[1], "sell"], review["teammates"])
        self.assertIn([SEATS[4], "sell"], review["teammates"])
        # The consensus is made of the final calls.
        action, confidence = self.expected_consensus(signal)
        self.assertEqual((signal["action"], action), ("buy", "buy"))
        self.assertAlmostEqual(signal["confidence"], confidence, places=6)
        # Thirteen models buy, one of the team still sells; the fixed rules are the fifteenth voice.
        models = [v for v in signal["verdicts"] if v["source"] != "rules"]
        self.assertEqual(sorted(v["action"] for v in models), ["buy"] * 13 + ["sell"])
        self.assertEqual(signal["agreeing"] + signal["opposing"] + signal["holding"], 15)
        self.assertEqual(signal["opposing"], 1)
        said = [a["text"] for a in out["account"]["activity"] if a["text"].startswith("Analysis team")]
        self.assertEqual(said, ["Analysis team (6 analysts): 5 buy, 1 sell, 0 hold after the team review; "
                                "1 changed its call."])

    def test_a_member_that_fails_in_the_review_keeps_its_first_call(self):
        team = PANEL["analysis"]
        plan = {"*": BUY, team[0]: dict(BUY, review={"status": 500}), team[2]: dict(BUY, review={"raw": "Agreed!"}),
                team[3]: dict(BUY, review={"empty": True}), team[5]: {"status": 429}}
        self.serve(plan)
        self.key = TEST_KEY
        signal = self.dump("signal")["signal"]
        verdicts = self.verdicts(signal)
        for index in (0, 2, 3):
            verdict = verdicts[team[index]]
            self.assertTrue(verdict["ok"], verdict)
            self.assertEqual((verdict["action"], verdict["confidence"], verdict["reviewed"]), ("buy", 85, False))
            self.assertGreater(verdict["stop"], 0)
        # The one that never answered was not asked again and is not shown to its teammates.
        self.assertFalse(verdicts[team[5]]["ok"])
        self.assertEqual(self.requests("chat", team[5], review=True), [])
        review = self.requests("chat", team[1], review=True)[0]
        self.assertEqual(len(review["teammates"]), 4)
        self.assertEqual(signal["team"], {"members": 5, "buy": 5, "sell": 0, "hold": 0, "reviewed": 2, "changed": 0})
        self.assertEqual(signal["voters"], 14)
        panel = {m["model"]: m for m in self.dump("config")["panel"]}
        self.assertNotEqual(panel[team[0]]["state"], "error")

    def test_the_team_review_can_be_switched_off(self):
        self.serve({"*": BUY})
        self.key = TEST_KEY
        self.settings("[engine]\nteam_review=false\n")
        self.assertFalse(self.dump("config")["teamReview"])
        out = self.dump("signal")
        signal = out["signal"]
        self.assertEqual(self.requests("chat", review=True), [])
        self.assertEqual(len(self.requests("chat")), 14)
        self.assertEqual(signal["team"], {"members": 6, "buy": 6, "sell": 0, "hold": 0, "reviewed": 0, "changed": 0})
        # The seats stay: they are how the team reads the market, review or not.
        self.assertEqual([self.requests("chat", model)[0]["seat"] for model in PANEL["analysis"]], SEATS)
        said = [a["text"] for a in out["account"]["activity"] if a["text"].startswith("Analysis team")]
        self.assertEqual(said, ["Analysis team (6 analysts): 6 buy, 0 sell, 0 hold."])

    def test_a_team_of_one_has_nobody_to_review_with_and_keeps_its_seat(self):
        team = PANEL["analysis"]
        self.serve({"*": BUY})
        self.key = TEST_KEY
        self.settings("[panel]\noff=" + ", ".join(team[:2] + team[3:]) + "\n")
        self.assertTrue(self.dump("config")["teamReview"])
        signal = self.dump("signal")["signal"]
        self.assertEqual(self.requests("chat", review=True), [])
        # Its seat is its place on the panel, not among the ones that are switched on.
        self.assertEqual(self.requests("chat", team[2])[0]["seat"], SEATS[2])
        self.assertEqual(signal["team"]["members"], 1)
        self.assertEqual(signal["voters"], 10)

    # ---- slow and failing models -------------------------------------------------

    def test_a_model_that_fails_once_is_asked_again_and_votes(self):
        slow, limited, broken, down = PANEL_MODELS[6:10]
        plan = {"*": BUY,
                slow: dict(BUY, first={"delay": 5}),                         # no answer in time, then one
                limited: dict(BUY, first={"status": 429, "retry_after": 1}),  # "too many requests", then an answer
                broken: dict(BUY, first={"status": 503}),                     # a server error, then an answer
                down: {"status": 500}}                                        # a server error both times
        self.serve(plan)
        self.key = TEST_KEY
        self.settings("[nvidia]\ntimeout_seconds=3\n[engine]\nteam_review=false\n")
        signal = self.dump("signal")["signal"]
        verdicts = self.verdicts(signal)
        for model in (slow, limited, broken):
            self.assertTrue(verdicts[model]["ok"], verdicts[model])
            self.assertEqual(verdicts[model]["action"], "buy")
            self.assertEqual([c["attempt"] for c in self.requests("chat", model)], [1, 2], model)
        self.assertFalse(verdicts[down]["ok"])
        self.assertIn("has a problem", verdicts[down]["error"])
        self.assertEqual(len(self.requests("chat", down)), 2)                 # asked twice, not more
        # The ones that answered at once were asked once.
        self.assertEqual(len(self.requests("chat", PANEL_MODELS[0])), 1)
        self.assertEqual(signal["voters"], 14)
        self.assertGreaterEqual(verdicts[slow]["seconds"], 3)                 # its time includes the first try

    def test_a_model_that_never_answers_is_left_out_after_two_tries(self):
        silent = PANEL_MODELS[9]
        self.serve({"*": BUY, silent: dict(BUY, delay=6)})
        self.key = TEST_KEY
        self.settings("[nvidia]\ntimeout_seconds=2\n[engine]\nteam_review=false\n")
        self.assertEqual(self.dump("config")["modelTimeout"], 2)
        signal = self.dump("signal")["signal"]
        verdict = self.verdicts(signal)[silent]
        self.assertFalse(verdict["ok"])
        self.assertEqual(verdict["error"], "No answer in 2 s, asked twice.")
        self.assertEqual(len(self.requests("chat", silent)), 2)
        self.assertEqual(signal["voters"], 14)                                # the others decide without it
        self.assertEqual(signal["action"], "buy")

    def test_only_so_many_questions_are_with_the_service_at_a_time(self):
        self.serve({"*": dict(BUY, delay=0.4)})
        self.key = TEST_KEY
        config = self.dump("config")
        self.assertEqual((config["modelTimeout"], config["modelParallel"]), (180, 6))
        self.settings("[nvidia]\nparallel=3\n")
        signal = self.dump("signal")["signal"]
        self.assertEqual(signal["voters"], 15)                                # everybody still gets its turn
        chats = self.requests("chat", review=None)
        self.assertEqual(len(chats), 20)
        self.assertEqual(max(c["inflight"] for c in chats), 3)

    def test_a_split_panel_means_no_trade(self):
        plan = {model: (BUY if index % 2 == 0 else SELL) for index, model in enumerate(PANEL_MODELS)}
        self.serve(plan)
        self.key = TEST_KEY
        self.settings(AUTO)
        out = self.dump("signal")
        signal = out["signal"]
        action, confidence = self.expected_consensus(signal)
        self.assertAlmostEqual(signal["confidence"], confidence, places=6)
        self.assertLess(signal["confidence"], 60)
        self.assertEqual(signal["agreeing"] + signal["opposing"] + signal["holding"], signal["voters"])
        self.assertFalse(signal["traded"])
        self.assertFalse(signal["risk"]["allowed"])
        self.assertEqual(out["account"]["positions"], [])

    def test_failing_models_are_left_out_not_guessed(self):
        models = PANEL_MODELS
        plan = {"*": HOLD,
                models[0]: {"status": 429}, models[1]: {"status": 500}, models[2]: {"raw": "I think it goes up!"},
                models[3]: {"refusal": True}, models[4]: {"empty": True}, models[5]: {"status": 404},
                models[6]: {"raw": "<think>still thinking about {this}"},
                models[7]: {"raw": "{\"action\": \"moon\", \"confidence\": 99}"}}
        self.serve(plan)
        self.key = TEST_KEY
        signal = self.dump("signal")["signal"]
        verdicts = self.verdicts(signal)
        for model in models[:8]:
            self.assertFalse(verdicts[model]["ok"], model)
            self.assertNotEqual(verdicts[model]["error"], "")
            self.assertEqual(verdicts[model]["confidence"], 0)
        self.assertIn("Too many requests", verdicts[models[0]]["error"])
        self.assertIn("has a problem", verdicts[models[1]]["error"])
        self.assertIn("JSON", verdicts[models[2]]["error"])
        self.assertIn("declined", verdicts[models[3]]["error"])
        self.assertIn("empty answer", verdicts[models[4]]["error"])
        self.assertIn("does not offer this model", verdicts[models[5]]["error"])
        self.assertIn("JSON", verdicts[models[6]]["error"])
        self.assertIn("no action", verdicts[models[7]]["error"])
        for model in models[8:]:
            self.assertTrue(verdicts[model]["ok"], model)
        self.assertEqual(signal["voters"], 7)  # the rules and the six models that answered
        self.assertEqual(self.requests("chat", review=True), [])  # nobody of the team answered: no review

    def test_a_rejected_key_is_reported_without_repeating_what_the_service_said(self):
        self.serve({"*": BUY})
        self.key = "nvapi-wrong-key-00000000000000000000"
        signal = self.dump("signal")["signal"]
        verdicts = self.verdicts(signal)
        for model in PANEL_MODELS:
            self.assertFalse(verdicts[model]["ok"])
            self.assertIn("did not accept the API key", verdicts[model]["error"])
        self.assertNotIn("nvapi-wrong", self.last_output)
        self.assertNotIn("abcd", self.last_output)  # the service's echo of the key is not passed on
        self.assertEqual(signal["voters"], 1)

    def test_a_model_the_service_no_longer_offers_is_left_out(self):
        gone = "z-ai/glm-5.3-flash"
        self.serve({"*": BUY, "missing": [gone]})
        self.key = TEST_KEY
        signal = self.dump("signal")["signal"]
        verdicts = self.verdicts(signal)
        # Either it was not asked (the catalog arrived first) or it was and NVIDIA said no.
        if gone in verdicts:
            self.assertFalse(verdicts[gone]["ok"])
            self.assertIn("does not offer this model", verdicts[gone]["error"])
        self.assertEqual(signal["voters"], 14)

    def test_answers_in_a_code_fence_or_after_thinking_are_read(self):
        models = PANEL_MODELS
        self.serve({"*": HOLD, models[0]: dict(BUY, fenced=True), models[1]: dict(BUY, think=True),
                    models[2]: dict(SELL, fenced=True, think=True)})
        self.key = TEST_KEY
        verdicts = self.verdicts(self.dump("signal")["signal"])
        self.assertEqual((verdicts[models[0]]["ok"], verdicts[models[0]]["action"]), (True, "buy"))
        self.assertEqual((verdicts[models[1]]["ok"], verdicts[models[1]]["action"]), (True, "buy"))
        self.assertEqual((verdicts[models[2]]["ok"], verdicts[models[2]]["action"]), (True, "sell"))

    # ---- the account over time ---------------------------------------------------------
    def test_the_stop_closes_the_trade_when_the_price_falls(self):
        plan = {"*": BUY}
        out = self.act("analyze", plan, AUTO, moves=[(4, dict(plan, market={"move_pct": -3.0}))], seconds=13)
        self.assertEqual(out["account"]["positions"], [])
        trades = out["account"]["trades"]
        self.assertEqual(len(trades), 1)
        trade = trades[0]
        self.assertEqual((trade["side"], trade["reason"]), ("buy", "stop_loss"))
        self.assertLess(trade["exit"], trade["entry"])
        # The price went 3 % down past a 1 % stop: the fill is the worse price, not the stop.
        self.assertLess(trade["exit"], out["signal"]["stop"])
        self.assertLess(trade["pnl"], -100)
        account = out["account"]["account"]
        self.assertAlmostEqual(account["equity"], 10000 + trade["pnl"], places=6)
        self.assertEqual((account["trades"], account["wins"], account["losses"]), (1, 0, 1))
        self.assertGreater(account["maxDrawdownPct"], 1)
        texts = [a["text"] for a in out["account"]["activity"]]
        self.assertTrue(any("stopped out" in t for t in texts), texts)
        self.assertEqual(len(self.dump("account")["trades"]), 1)  # saved

    def test_the_target_closes_the_trade_when_the_price_rises(self):
        plan = {"*": BUY}
        out = self.act("analyze", plan, AUTO, moves=[(4, dict(plan, market={"move_pct": 3.5}))], seconds=13)
        self.assertEqual(out["account"]["positions"], [])
        trade = out["account"]["trades"][0]
        self.assertEqual(trade["reason"], "take_profit")
        self.assertAlmostEqual(trade["exit"], out["signal"]["takeProfit"], places=6)  # a limit order fills at its price
        self.assertGreater(trade["pnl"], 70)  # 2.5 % of a third of the account, less fees
        self.assertEqual(out["account"]["account"]["wins"], 1)
        self.assertAlmostEqual(out["account"]["account"]["winRatePct"], 100)

    def test_a_position_stays_open_while_the_price_is_between_its_levels(self):
        plan = {"*": BUY}
        out = self.act("analyze", plan, AUTO, moves=[(4, dict(plan, market={"move_pct": 0.3}))], seconds=10)
        self.assertEqual(len(out["account"]["positions"]), 1)
        self.assertEqual(out["account"]["trades"], [])
        self.assertGreater(out["account"]["positions"][0]["pnl"], 0)  # the price went up a little

    def test_a_closing_candle_starts_an_analysis_by_itself(self):
        # One-minute candles: within 70 seconds one closes, and that alone,
        # with nobody asking, has to produce an analysis and its trade.
        out = self.act("", {"*": BUY}, "[engine]\nauto_trade=true\n[market]\ninterval=1m\n", seconds=70)
        self.assertEqual(out["signal"]["state"], "ready")
        self.assertEqual(out["signal"]["interval"], "1m")
        self.assertEqual(len(out["account"]["positions"]) + len(out["account"]["trades"]), 1)
        chats = self.requests("chat")
        self.assertGreaterEqual(len(chats), 14)
        self.assertLessEqual(len(chats), 28)  # one candle closed, two at most: not an analysis per price update
        intervals = {q.split("interval=")[1].split("&")[0] for q in
                     (e.get("query", "") for e in self.requests("market")) if "interval=" in q}
        # The chart's candles, the higher timeframe for context, and the watchlist's 24-hour lines.
        self.assertEqual(intervals, {"1m", "15m", "1h"})

    def test_auto_analyze_off_waits_to_be_asked(self):
        out = self.act("", {"*": BUY}, AUTO + "[market]\ninterval=1m\n", seconds=70)
        self.assertEqual(out["signal"]["state"], "none")
        self.assertEqual(self.requests("chat"), [])
        self.assertEqual(out["account"]["positions"], [])

    def test_a_signal_can_be_placed_by_hand_once(self):
        out = self.act("analyze,place,place", {"*": BUY}, "[engine]\nauto_analyze=false\n")  # auto trade is off
        results = [a["result"] for a in out["actions"]]
        self.assertEqual(results[:2], ["", ""])   # analyzed, placed
        self.assertNotEqual(results[2], "")       # not twice
        self.assertEqual(len(out["account"]["positions"]), 1)
        self.assertTrue(out["signal"]["traded"])
        self.assertFalse(out["signal"]["placeable"])

    def test_nothing_can_be_placed_without_a_signal(self):
        out = self.act("analyze,place", {"*": HOLD}, "[panel]\nanalysis=moonshotai/kimi-k3.1-pro\nreasoning=\n"
                                                    "vision=\ncompute=\n[risk]\nmin_agreeing=5\n"
                                                    "[engine]\nauto_analyze=false\n")
        if out["signal"]["action"] == "hold":
            self.assertIn("no trade signal", out["actions"][1]["result"])
        else:  # the rules alone found a trade; the risk rules still refuse it
            self.assertNotEqual(out["actions"][1]["result"], "")
        self.assertEqual(out["account"]["positions"], [])

    def test_a_position_closed_by_hand_becomes_a_trade(self):
        out = self.act("analyze,wait:6,close-all", {"*": BUY}, AUTO)
        self.assertEqual(out["actions"][2]["result"], "1")
        self.assertEqual(out["account"]["positions"], [])
        trade = out["account"]["trades"][0]
        self.assertEqual(trade["reason"], "manual")
        self.assertAlmostEqual(out["account"]["account"]["equity"], 10000 + trade["pnl"], places=6)

    def test_reset_starts_the_account_over(self):
        out = self.act("analyze,reset", {"*": BUY}, AUTO + "[paper]\nstarting_balance=25000\n")
        self.assertEqual(out["account"]["positions"], [])
        self.assertEqual(out["account"]["trades"], [])
        self.assertEqual(out["account"]["account"]["equity"], 25000)
        self.assertEqual(self.dump("account")["account"]["equity"], 25000)

    def test_the_drawdown_limit_halts_trading_until_resumed(self):
        plan = {"*": BUY}
        # A 1 % drawdown limit. The price gaps 3 % through a 1 % stop, so the
        # trade (a third of the account) loses a little over 1 % of the
        # account; the daily limit is set wide so only the drawdown limit is in play.
        out = self.act("analyze,wait:9,analyze", plan,
                       AUTO + "[risk]\nmax_drawdown_pct=1\ncooldown_minutes=0\nmax_daily_loss_pct=20\n",
                       moves=[(4, dict(plan, market={"move_pct": -3.0}))])
        account = out["account"]["account"]
        self.assertEqual(len(out["account"]["trades"]), 1)
        self.assertEqual(out["account"]["trades"][0]["reason"], "stop_loss")
        self.assertTrue(account["halted"])
        self.assertEqual(out["account"]["positions"], [])  # the second signal was not traded
        checks = {c["key"]: c for c in out["signal"]["risk"]["checks"]}
        self.assertFalse(checks["drawdown"]["pass"])
        self.assertIn("halted", checks["drawdown"]["detail"])
        self.assertTrue(any("Trading halted" in a["text"] for a in out["account"]["activity"]))
        # Halted survives a restart; resuming lets the next signal through.
        resumed = self.dump("session", {"SHADOW_TRADING_SESSION_ACTIONS": "resume,analyze"})
        self.assertFalse(resumed["account"]["account"]["halted"])
        self.assertEqual(len(resumed["account"]["positions"]), 1, resumed["signal"].get("outcome"))

    def test_the_daily_loss_limit_stops_new_trades(self):
        plan = {"*": BUY}
        # The gap makes the first trade lose 3 % of its size, a little over 1 % of the account.
        out = self.act("analyze,wait:9,analyze", plan,
                       AUTO + "[risk]\ncooldown_minutes=0\nmax_daily_loss_pct=1\n",
                       moves=[(4, dict(plan, market={"move_pct": -3.0}))])
        account = out["account"]["account"]
        self.assertEqual(len(out["account"]["trades"]), 1)
        self.assertLess(account["todayPnlPct"], -1)
        self.assertTrue(account["dailyLossReached"])
        self.assertFalse(account["halted"])  # 1 % is not the 10 % drawdown limit
        self.assertEqual(out["account"]["positions"], [])
        self.assertIn("Daily loss limit", out["signal"]["outcome"])

    def test_a_reversed_signal_closes_the_position(self):
        out = self.act("analyze,wait:5,analyze", {"*": BUY}, AUTO,
                       moves=[(3, {"*": dict(SELL, confidence=95)})])
        self.assertEqual(out["signal"]["action"], "sell")
        trades = out["account"]["trades"]
        self.assertEqual(len(trades), 1)
        self.assertEqual((trades[0]["side"], trades[0]["reason"]), ("buy", "signal_flip"))
        # The long is gone; whether the short opens depends on the cooldown after a losing close.
        for position in out["account"]["positions"]:
            self.assertEqual(position["side"], "sell")

    def test_an_unreadable_account_file_is_kept_aside(self):
        self.serve()
        os.makedirs(self.data_dir)
        path = os.path.join(self.data_dir, "paper.json")
        with open(path, "w", encoding="utf-8") as f:
            f.write("{ this is not json")
        account = self.dump("account")
        self.assertEqual(account["account"]["equity"], 10000)
        self.assertTrue(os.path.exists(path + ".unreadable"))
        self.assertIn("could not be read", account["activity"][0]["text"])


if __name__ == "__main__":
    unittest.main()
