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
from fake_services import OANDA_TOKEN, PANEL, PANEL_MODELS, SEATS, TEST_KEY  # noqa: E402  (the stand-in's own test keys)

BUY = {"action": "buy", "confidence": 85, "stop_pct": -1.0, "target_pct": 2.5}
SELL = {"action": "sell", "confidence": 85, "stop_pct": -1.0, "target_pct": 2.5}
HOLD = {"action": "hold"}
# Auto trade on. The analyses are the ones a test asks for: left on, the
# application would also analyze whenever a real quarter-hour passes mid-test.
AUTO = "[engine]\nauto_trade=true\nauto_analyze=false\n"
# The forex markets from the stand-in for Kraken's public API (no key): currency
# pairs and the gold token. The application's own default is OANDA (OANDA_KIND).
FOREX = "[market]\nkind=forex\nforex_source=kraken\n"


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
        # The stand-in exchange of most tests speaks the crypto service's API;
        # None = what the application does by itself (forex, from OANDA).
        self.market_kind = "crypto"
        self.oanda = None  # SHADOW_TRADING_OANDA_TOKEN; None = not set
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
        if self.market_kind is not None:
            env["SHADOW_TRADING_MARKET_KIND"] = self.market_kind
        if self.oanda is not None:
            env["SHADOW_TRADING_OANDA_TOKEN"] = self.oanda
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
        self.last_stderr = done.stderr
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
        # A setup is in play, so a key that counted would have the team review it.
        self.serve({"*": BUY, "market": self.BULL})
        for foreign in ("sk-ant-api03-0000000000000000000000000", "sk-proj-000000000000000000000000000",
                        "AIzaSy00000000000000000000000000000000"):
            self.key = foreign
            config = self.dump("config")
            self.assertFalse(config["key"]["hasKey"], foreign[:6])
            signal = self.dump("signal")["signal"]
            self.assertEqual(signal["sbt"]["status"], "entered")
            # No key: nobody reviewed it, the strategy's rules are the only voice.
            self.assertEqual(signal["verdicts"], [])
            self.assertFalse(signal["sbt"]["review"]["done"])
            self.assertEqual((signal["voters"], signal["lead"]), (1, "sbt"))
            self.assertNotIn(foreign, self.last_output)
        self.assertEqual(self.requests("chat"), [])
        # The catalog is public and was fetched without any key.
        self.assertTrue(all(e["auth"] == "missing" for e in self.requests("models")))

    def test_a_saved_key_file_is_used_and_made_private(self):
        self.serve({"*": BUY, "market": self.BULL})
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
        self.assertEqual(signal["voters"], 14)  # every model reviewed the setup with the saved key
        self.assertTrue(signal["sbt"]["review"]["done"])
        self.assertTrue(all(c["auth"] == "ok" for c in self.requests("chat", review=None)))

    def test_the_assistants_nvidia_key_is_shared(self):
        self.serve({"*": BUY, "market": self.BULL})   # a setup in play for the team to review
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
        self.serve({"*": BUY, "market": self.BULL})
        self.key = TEST_KEY
        self.settings("[panel]\nanalysis=moonshotai/kimi-k3.1-pro, not a model, ../../etc/passwd\n"
                      "reasoning=\nvision=nvidia/cosmos-reason2-8b, moonshotai/kimi-k3.1-pro\ncompute=\n")
        config = self.dump("config")
        self.assertEqual([(m["model"], m["role"]) for m in config["panel"]],
                         [("moonshotai/kimi-k3.1-pro", "analysis"), ("nvidia/cosmos-reason2-8b", "vision")])
        signal = self.dump("signal")["signal"]
        # The two models of the chosen panel reviewed the setup, nobody else.
        self.assertEqual(set(self.verdicts(signal)), {"moonshotai/kimi-k3.1-pro", "nvidia/cosmos-reason2-8b"})
        self.assertEqual(sorted(c["model"] for c in self.requests("chat")),
                         ["moonshotai/kimi-k3.1-pro", "nvidia/cosmos-reason2-8b"])
        self.assertEqual(self.requests("chat", "moonshotai/kimi-k3.1-pro")[0]["images"], [])
        self.assertEqual(self.requests("chat", "nvidia/cosmos-reason2-8b")[0]["images"], ["960x540"])

    def test_a_model_switched_off_is_not_asked(self):
        off = ["z-ai/glm-5.3-flash", "nvidia/nemotron-3.5-bolt-e4b"]
        self.serve({"*": BUY, "market": self.BULL})
        self.key = TEST_KEY
        self.settings("[panel]\noff=" + ", ".join(off) + "\n")
        signal = self.dump("signal")["signal"]
        asked = sorted(c["model"] for c in self.requests("chat"))
        self.assertEqual(asked, sorted(m for m in PANEL_MODELS if m not in off))
        self.assertEqual(signal["voters"], 12)
        self.assertFalse(set(off) & set(self.verdicts(signal)))

    # ---- market ------------------------------------------------------------------
    def test_market_data_is_read(self):
        self.serve(trend="steep")   # rising whatever the time of day
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
        # The setup's long is placed at the start; four seconds in the price
        # falls 3 %, past its stop (about 1.9 % under the entry).
        out = self.act("analyze", {"*": BUY, "market": self.BULL}, settings=AUTO, seconds=6,
                       moves=[(4, {"*": BUY, "market": dict(self.BULL, move_pct=-3)})])
        trades = out["account"]["trades"]
        self.assertEqual(len(trades), 1, out["account"]["activity"])
        self.assertEqual(trades[0]["reason"], "stop_loss")
        self.assertLess(trades[0]["exit"], trades[0]["entry"])
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
        out = self.act("analyze,remove:BTCUSDT", {"*": BUY, "market": self.BULL}, settings=AUTO)
        self.assertEqual(len(out["account"]["positions"]), 1)   # the setup's long on BTC/USDT
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
        # No key: SBT Model 1's rules are the one voice; no model is asked.
        self.serve({"*": BUY, "market": self.BULL})
        signal = self.dump("signal")["signal"]
        self.assertEqual(signal["state"], "ready")
        self.assertEqual(signal["sbt"]["status"], "entered")
        self.assertEqual(signal["verdicts"], [])
        self.assertEqual((signal["voters"], signal["agreeing"], signal["lead"]), (1, 1, "sbt"))
        self.assertEqual((signal["action"], signal["confidence"]), ("buy", 100))
        # The levels are the strategy's: the stop below the zone, Level 3 the target.
        self.assertAlmostEqual(signal["stop"], signal["sbt"]["stop"], places=6)
        self.assertAlmostEqual(signal["takeProfit"], 106.5 * self.SCALE, delta=0.01)
        self.assertTrue(signal["risk"]["allowed"], signal["risk"]["reason"])
        self.assertTrue(signal["placeable"])        # auto trade is off: it waits to be placed by hand
        self.assertEqual(self.requests("chat"), [])
        # Asking for an analysis asks nobody: there is no model to review with.
        out = self.dump("session", {"SHADOW_TRADING_SESSION_ACTIONS": "analyze"})
        self.assertEqual(out["actions"], [{"action": "analyze", "result": "not started"}])
        self.assertTrue(any("trades by its rules alone" in a["text"] for a in out["account"]["activity"]))
        self.assertEqual(self.requests("chat"), [])

    def test_every_model_is_asked_and_the_trade_is_placed(self):
        self.serve({"*": BUY, "market": self.BULL})
        self.key = TEST_KEY
        self.settings(AUTO)
        out = self.dump("signal")
        signal = out["signal"]
        sbt = signal["sbt"]
        verdicts = self.verdicts(signal)
        # The panel's models reviewed the setup; the strategy itself is not a verdict.
        self.assertEqual(set(verdicts), set(PANEL_MODELS))
        for model in PANEL_MODELS:
            self.assertTrue(verdicts[model]["ok"], verdicts[model])
            self.assertEqual((verdicts[model]["action"], verdicts[model]["confidence"]), ("buy", 85))
        for role, models in PANEL.items():
            for model in models:
                self.assertEqual(verdicts[model]["role"], role)
        self.assertEqual(signal["voters"], 14)
        action, confidence = self.expected_consensus(signal)
        self.assertEqual((signal["action"], action), ("buy", "buy"))
        self.assertAlmostEqual(signal["confidence"], confidence, places=6)
        self.assertGreaterEqual(signal["confidence"], 60)
        self.assertEqual((sbt["review"]["agreeing"], sbt["review"]["voters"]), (14, 14))
        self.assertTrue(sbt["review"]["confirmed"])
        # Levels: the strategy's, not what the models proposed (1 % and 2.5 % from the price).
        self.assertAlmostEqual(signal["stop"], sbt["stop"], places=6)
        self.assertAlmostEqual(signal["takeProfit"], 106.5 * self.SCALE, delta=0.01)
        proposed = [v["stop"] for v in signal["verdicts"] if v["ok"] and v["stop"] > 0]
        self.assertTrue(proposed)
        self.assertNotAlmostEqual(signal["stop"], statistics.median(proposed), delta=100)
        self.assertLess(signal["stop"], signal["entry"])
        self.assertGreater(signal["takeProfit"], signal["entry"])
        self.assertTrue(signal["traded"], signal["outcome"])

        positions = out["account"]["positions"]
        self.assertEqual(len(positions), 1)
        position = positions[0]
        self.assertEqual((position["symbol"], position["side"]), ("BTCUSDT", "buy"))
        # The entry candle's open, filled 0.02 % worse as a market order is.
        self.assertAlmostEqual(position["entry"], 101.6 * self.SCALE * 1.0002, delta=0.01)
        self.assertAlmostEqual(position["stop"], signal["stop"], places=6)
        self.assertAlmostEqual(position["takeProfit"], signal["takeProfit"], places=6)
        # A stop about 1.9 % away would need half the account to put 1 % at
        # risk; with three positions allowed, one takes a third of it at most.
        self.assertAlmostEqual(position["entry"] * position["quantity"], 10000 / 3, delta=40)
        at_risk = (position["entry"] - position["stop"]) * position["quantity"]
        stop_pct = (position["entry"] - position["stop"]) / position["entry"]
        self.assertAlmostEqual(at_risk, 10000 / 3 * stop_pct, delta=2)
        self.assertLess(at_risk, 100)                 # never more than the 1 % per trade
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

        # The account survives a restart, and the setup read again does not double the position:
        # it is known as the one that was traded, and cannot be placed a second time.
        saved = os.path.join(self.data_dir, "paper.json")
        self.assertEqual(stat.S_IMODE(os.stat(saved).st_mode), 0o600)
        again = self.dump("signal")
        self.assertEqual([p["id"] for p in again["account"]["positions"]], [position["id"]])
        self.assertEqual(again["account"]["trades"], [])
        self.assertTrue(again["signal"]["traded"])
        self.assertFalse(again["signal"]["placeable"])

    def test_auto_trade_off_places_nothing(self):
        self.serve({"*": BUY, "market": self.BULL})
        self.key = TEST_KEY
        out = self.dump("signal")
        self.assertTrue(out["signal"]["sbt"]["review"]["confirmed"])   # the team confirmed the entry
        self.assertEqual(out["signal"]["action"], "buy")
        self.assertTrue(out["signal"]["risk"]["allowed"])
        self.assertFalse(out["signal"]["traded"])
        self.assertTrue(out["signal"]["placeable"])
        self.assertIn("Auto trade is off", out["signal"]["outcome"])
        self.assertEqual(out["account"]["positions"], [])

    def test_a_falling_market_is_sold_short(self):
        # A bearish 1-2-3 and its retest, the team confirming the sell.
        self.serve({"*": SELL, "market": self.BEAR}, trend="down")
        self.key = TEST_KEY
        self.settings(AUTO)
        out = self.dump("signal")
        signal = out["signal"]
        self.assertEqual(signal["sbt"]["direction"], "bearish")
        self.assertEqual(signal["action"], "sell")
        self.assertGreater(signal["stop"], signal["entry"])
        self.assertLess(signal["takeProfit"], signal["entry"])
        self.assertTrue(signal["traded"], signal["outcome"])
        position = out["account"]["positions"][0]
        self.assertEqual(position["side"], "sell")
        self.assertGreater(position["stop"], position["entry"])
        self.assertAlmostEqual(position["takeProfit"], (200 - 106.5) * self.SCALE, delta=0.01)

    def test_an_open_position_stays_on_show_when_the_next_analysis_says_hold(self):
        # The trade is placed; then the team is asked about the setup again
        # and now says hold. That does not undo the trade: the position is
        # still there for the chart and the panel, with its own entry, stop
        # and target, also after another market was looked at in between.
        plan = {"*": BUY, "market": self.BULL}
        out = self.act("analyze,wait:3,analyze,select:ETHUSDT,select:BTCUSDT", plan, settings=AUTO,
                       moves=[(2.5, dict(plan, **{"*": HOLD}))])
        self.assertEqual(out["actions"][2]["result"], "")         # the second review did run
        self.assertEqual(out["signal"]["symbol"], "BTCUSDT")
        positions = out["account"]["positions"]
        self.assertEqual(len(positions), 1)
        shown = out["account"]["position"]
        self.assertEqual(shown["id"], positions[0]["id"])
        self.assertEqual((shown["symbol"], shown["side"]), ("BTCUSDT", "buy"))
        self.assertEqual((shown["entry"], shown["stop"], shown["takeProfit"]),
                         (positions[0]["entry"], positions[0]["stop"], positions[0]["takeProfit"]))
        said = [a["text"] for a in out["account"]["activity"]]
        self.assertTrue(any("not confirmed" in text for text in said), said)   # the second review did say hold

    def test_the_position_shown_is_the_selected_markets(self):
        # One position at most: ETH/USDT's own setup is refused for it, so
        # nothing is open on the market that is on the chart.
        out = self.act("analyze,select:ETHUSDT,wait:3", {"*": BUY, "market": self.BULL},
                       settings=AUTO + "[risk]\nmax_open_positions=1\n")
        positions = out["account"]["positions"]
        self.assertEqual([p["symbol"] for p in positions], ["BTCUSDT"])
        self.assertEqual(out["signal"]["symbol"], "ETHUSDT")
        self.assertEqual(out["account"]["position"], {})          # ETH/USDT is on the chart: nothing open there

    # ---- forex from OANDA: what the application opens on ------------------------

    def oanda_requests(self):
        return self.requests("oanda")

    def test_the_application_opens_on_gold_silver_and_the_euro_from_oanda(self):
        self.serve()
        self.market_kind = None
        self.oanda = OANDA_TOKEN
        config = self.dump("config")
        self.assertEqual((config["marketKind"], config["accountCurrency"]), ("forex", "USD"))
        self.assertEqual(config["watchlist"], ["XAUUSD", "XAGUSD", "EURUSD"])
        self.assertEqual(config["symbol"], "XAUUSD")
        self.assertEqual(config["tradingViewSymbol"], "FOREXCOM:XAUUSD")   # FOREX.com's chart on TradingView
        source = config["priceSource"]
        self.assertEqual((source["service"], source["needsToken"], source["hasToken"], source["tokenSource"],
                          source["hint"], source["account"]),
                         ("oanda", True, True, "environment", OANDA_TOKEN[-4:], "practice"))
        self.assertEqual(config["streamSource"], "")            # asked for every few seconds
        market = self.dump("market")
        self.assertNotIn(OANDA_TOKEN, self.last_output)
        self.assertNotIn(OANDA_TOKEN[:20], self.last_output)
        listed = {m["symbol"]: m for m in market["watchlist"]}
        self.assertEqual([listed[s]["name"] for s in ("XAUUSD", "XAGUSD", "EURUSD")], ["XAU/USD", "XAG/USD", "EUR/USD"])
        self.assertTrue(all(m["hasPrice"] for m in market["watchlist"]), market["watchlist"])
        self.assertAlmostEqual(listed["XAUUSD"]["price"], 4050, delta=200)
        self.assertAlmostEqual(listed["XAGUSD"]["price"], 51.2, delta=3)
        self.assertAlmostEqual(listed["EURUSD"]["price"], 1.0982, delta=0.06)
        self.assertGreaterEqual(market["quote"]["candles"], 300)
        asked = self.oanda_requests()
        paths = {r["path"] for r in asked}
        self.assertIn("/v3/instruments/XAU_USD/candles", paths)
        self.assertTrue({"/v3/instruments/XAG_USD/candles", "/v3/instruments/EUR_USD/candles"} <= paths, paths)
        chart = [r for r in asked if r["path"] == "/v3/instruments/XAU_USD/candles" and "granularity=M15" in r["query"]]
        self.assertTrue(chart and "price=M" in chart[0]["query"], asked)
        self.assertTrue(all(r["auth"] == "ok" and r["unix"] and r["status"] == 200 for r in asked), asked)
        self.assertEqual(market["dataState"], "live", market.get("dataError"))

    def test_a_settings_file_of_the_crypto_days_still_opens_on_forex(self):
        # An earlier version's Crypto/Forex switch wrote kind=crypto and its own
        # lists; there is no switch any more, so they are not followed.
        self.serve()
        self.market_kind = None
        self.oanda = OANDA_TOKEN
        self.settings("[market]\nkind=crypto\nwatchlist=BTCUSDT,ETHUSDT\nsymbol=BTCUSDT\n"
                      "forex_watchlist=EURUSD,GBPUSD,PAXGUSD\nforex_symbol=PAXGUSD\n")
        config = self.dump("config")
        self.assertEqual(config["marketKind"], "forex")
        self.assertEqual(config["watchlist"], ["XAUUSD", "XAGUSD", "EURUSD"])
        self.assertEqual(config["tradingViewSymbol"], "FOREXCOM:XAUUSD")
        self.assertNotIn("BINANCE", json.dumps(config))

    def test_the_tradingview_widget_shows_forex_com_with_the_markets_and_its_symbol_search(self):
        self.serve()
        self.market_kind = None
        self.oanda = OANDA_TOKEN
        out = self.act("add:GBPUSD,select:XAGUSD", {})
        self.assertEqual([a["result"] for a in out["actions"]], ["", ""])
        widget = self.dump("config")["tradingView"]
        self.assertEqual(widget["symbol"], "FOREXCOM:XAGUSD")
        self.assertEqual(widget["interval"], "15")
        # The markets of the desk are the widget's watchlist; its search adds any other chart.
        self.assertEqual(widget["watchlist"], ["FOREXCOM:XAUUSD", "FOREXCOM:XAGUSD", "FOREXCOM:EURUSD", "FOREXCOM:GBPUSD"])
        self.assertTrue(widget["allow_symbol_change"])
        self.assertFalse(widget["hide_side_toolbar"])     # the drawing tools
        self.assertEqual(widget["studies"], [])           # no RSI, EMA or other indicator lines

    def test_without_an_oanda_token_the_application_says_where_to_add_one(self):
        self.serve()
        self.market_kind = None
        market = self.dump("market")
        self.assertEqual(market["dataState"], "error")
        self.assertIn("Settings → Prices", market["dataError"])
        self.assertEqual(self.oanda_requests(), [])              # nothing is asked without a token
        config = self.dump("config")
        self.assertEqual((config["priceSource"]["needsToken"], config["priceSource"]["hasToken"]), (True, False))

    def test_a_token_oanda_refuses_is_reported_and_never_shown(self):
        self.serve()
        self.market_kind = None
        wrong = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
        self.oanda = wrong
        market = self.dump("market")
        self.assertEqual(market["dataState"], "error")
        self.assertIn("OANDA did not accept the token", market["dataError"])
        self.assertTrue(self.oanda_requests() and all(r["auth"] == "wrong" for r in self.oanda_requests()))
        self.assertNotIn(wrong, self.last_output)

    def test_an_oanda_token_is_saved_for_its_owner_only_and_used_at_once(self):
        self.market_kind = None
        out = self.act("oanda-token:nvapi-0000000000000000000000000,oanda-token:" + OANDA_TOKEN, {})
        results = [a["result"] for a in out["actions"]]
        self.assertIn("NVIDIA key", results[0])
        self.assertEqual(results[1], "")
        path = os.path.join(self.conf_dir, "trading", "oanda.token")
        self.assertEqual(os.stat(path).st_mode & 0o777, 0o600)
        with open(path, encoding="utf-8") as f:
            self.assertEqual(f.read().strip(), OANDA_TOKEN)
        # Nowhere in what the application said (the test's own list of actions repeats it).
        said = json.dumps({k: v for k, v in out.items() if k != "actions"}) + self.last_stderr
        self.assertNotIn(OANDA_TOKEN, said)
        self.assertNotIn(OANDA_TOKEN[:20], said)
        self.assertEqual(out["market"]["dataState"], "live", out["market"].get("dataError"))
        self.assertGreaterEqual(out["market"]["quote"]["candles"], 300)

    def test_oanda_offers_gold_silver_and_the_dollar_pairs(self):
        self.market_kind = None
        self.oanda = OANDA_TOKEN
        out = self.act("find:,add:BTCUSDT,add:EURGBP,remove:XAGUSD,add:XAGUSD,add:GBPUSD,select:XAGUSD,older", {})
        results = [a["result"] for a in out["actions"]]
        self.assertEqual(results[0].split(), ["XAUUSD", "XAGUSD", "EURUSD", "GBPUSD", "AUDUSD", "NZDUSD",
                                              "USDJPY", "USDCHF", "USDCAD"])
        self.assertIn("cannot be added", results[1])
        self.assertIn("cannot be added", results[2])
        self.assertEqual(results[3:6], ["", "", ""])
        self.assertGreater(int(results[7]), 0)                   # older candles, asked with 'to'
        self.assertTrue(any("to=" in r["query"] for r in self.oanda_requests() if r["path"].startswith("/v3/instruments/XAG_USD")))
        self.assertEqual(self.dump("config")["symbol"], "XAGUSD")

    def test_the_strategy_chart_page_is_self_contained_and_gets_the_candles(self):
        self.market_kind = None
        self.oanda = OANDA_TOKEN
        out = self.act("", {})
        chart = out["market"]["strategyChart"]
        self.assertTrue(chart["library"] and chart["script"], chart)
        self.assertGreater(chart["pageBytes"], 150_000)          # the library is in the page, not fetched
        full, tail = chart["full"], chart["tail"]
        self.assertTrue(full["full"])
        self.assertEqual(full["key"], "XAUUSD:15m")
        self.assertEqual(len(full["candles"]), out["market"]["chart"]["candles"])
        times = [row[0] for row in full["candles"]]
        self.assertEqual(times, sorted(set(times)))              # seconds, ascending, no repeats
        self.assertEqual(times[1] - times[0], 900)
        # Asked again with what the page has: only the newest candle.
        self.assertFalse(tail["full"])
        self.assertEqual([row[0] for row in tail["candles"]], times[-1:])

    # ---- forex from Kraken ----------------------------------------------------------

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
        plan = {"*": BUY, "market": self.BULL}
        settings = "[market]\nkind=forex\nforex_source=kraken\nforex_symbol=USDCHF\n" + AUTO
        out = self.act("analyze", plan, settings, moves=[(4, dict(plan, market=dict(self.BULL, move_pct=0.3)))],
                       seconds=9)
        self.assertEqual(out["signal"]["symbol"], "USDCHF")
        position = out["account"]["positions"][0]
        self.assertEqual((position["symbol"], position["name"], position["side"]), ("USDCHF", "USD/CHF", "buy"))
        # The setup's entry on USD/CHF, filled 0.02 % worse as a market order is.
        self.assertAlmostEqual(position["entry"], 101.6 * 0.8311 / 100 * 1.0002, delta=2e-5)  # prices have 5 decimals
        # A third of the account at most, in dollars: the quantity itself.
        self.assertAlmostEqual(position["value"], position["quantity"], places=6)
        self.assertAlmostEqual(position["quantity"], 10000 / 3, delta=40)
        # The stop (about 1.9 % under the entry) on a third of the account: what
        # is at risk, in dollars, is that much of the third, and less than the 1 % allowed.
        at_stop = (position["entry"] - position["stop"]) * position["quantity"] / position["stop"]
        self.assertAlmostEqual(at_stop, 10000 / 3 * (position["entry"] - position["stop"]) / position["stop"], delta=1.5)
        self.assertLess(at_stop, 100)
        self.assertTrue(out["signal"]["risk"]["reducedByBalance"])
        expected = (position["price"] - position["entry"]) * position["quantity"] / position["price"]
        self.assertAlmostEqual(position["pnl"], expected, places=6)
        self.assertGreater(position["pnl"], 0)
        # And at the target, Level 3, about 4.8 % above the entry.
        out = self.act("wait:1", plan, settings, moves=[(1, dict(plan, market=dict(self.BULL, move_pct=6.0)))],
                       seconds=8)
        self.assertEqual(out["account"]["positions"], [])
        trade = out["account"]["trades"][0]
        self.assertEqual(trade["reason"], "take_profit")
        gross = (trade["exit"] - trade["entry"]) * trade["quantity"] / trade["exit"]
        fees = 2 * trade["quantity"] * 0.001
        self.assertAlmostEqual(trade["fees"], fees, places=6)
        self.assertAlmostEqual(trade["pnl"], gross - fees, places=6)
        self.assertAlmostEqual(out["account"]["account"]["balance"], 10000 + gross - fees, places=4)

    def test_switching_between_crypto_and_forex(self):
        out = self.act("kind:forex,kind:forex,analyze,kind:crypto,close-all,kind:crypto,kind:metals",
                       {"*": BUY, "market": self.BULL}, settings=AUTO + "[market]\nforex_source=kraken\n")
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
        self.serve({"*": BUY, "market": self.BULL})
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
        # Listed as the panel is: the team by seat, then the other roles.
        self.assertEqual([v["source"] for v in signal["verdicts"]], PANEL_MODELS)
        # A member still has one vote: fourteen models, as without the review.
        self.assertEqual(signal["voters"], 14)
        # Every question, first call and review alike, was about the setup.
        self.assertTrue(all(c.get("sbt") == "bullish" for c in self.requests("chat", review=None)))
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
                team[4]: SELL, "market": self.BULL}
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
        # Thirteen models buy, one of the team still sells: fourteen voices, and the setup is confirmed.
        self.assertEqual(sorted(v["action"] for v in signal["verdicts"]), ["buy"] * 13 + ["sell"])
        self.assertEqual(signal["agreeing"] + signal["opposing"] + signal["holding"], 14)
        self.assertEqual((signal["agreeing"], signal["opposing"]), (13, 1))
        self.assertTrue(signal["sbt"]["review"]["confirmed"])
        said = [a["text"] for a in out["account"]["activity"] if a["text"].startswith("Analysis team")]
        self.assertEqual(said, ["Analysis team (6 analysts): 5 buy, 1 sell, 0 hold after the team review; "
                                "1 changed its call."])

    def test_a_member_that_fails_in_the_review_keeps_its_first_call(self):
        team = PANEL["analysis"]
        plan = {"*": BUY, team[0]: dict(BUY, review={"status": 500}), team[2]: dict(BUY, review={"raw": "Agreed!"}),
                team[3]: dict(BUY, review={"empty": True}), team[5]: {"status": 429}, "market": self.BULL}
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
        self.assertEqual(signal["voters"], 13)
        panel = {m["model"]: m for m in self.dump("config")["panel"]}
        self.assertNotEqual(panel[team[0]]["state"], "error")

    def test_the_team_review_can_be_switched_off(self):
        self.serve({"*": BUY, "market": self.BULL})
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
        self.serve({"*": BUY, "market": self.BULL})
        self.key = TEST_KEY
        self.settings("[panel]\noff=" + ", ".join(team[:2] + team[3:]) + "\n")
        self.assertTrue(self.dump("config")["teamReview"])
        signal = self.dump("signal")["signal"]
        self.assertEqual(self.requests("chat", review=True), [])
        # Its seat is its place on the panel, not among the ones that are switched on.
        self.assertEqual(self.requests("chat", team[2])[0]["seat"], SEATS[2])
        self.assertEqual(signal["team"]["members"], 1)
        self.assertEqual(signal["voters"], 9)   # the one member and the eight models of the other roles

    # ---- slow and failing models -------------------------------------------------

    def test_a_model_that_fails_once_is_asked_again_and_votes(self):
        slow, limited, broken, down = PANEL_MODELS[6:10]
        plan = {"*": BUY,
                slow: dict(BUY, first={"delay": 5}),                         # no answer in time, then one
                limited: dict(BUY, first={"status": 429, "retry_after": 1}),  # "too many requests", then an answer
                broken: dict(BUY, first={"status": 503}),                     # a server error, then an answer
                down: {"status": 500},                                        # a server error both times
                "market": self.BULL}
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
        self.assertEqual(signal["voters"], 13)
        self.assertGreaterEqual(verdicts[slow]["seconds"], 3)                 # its time includes the first try

    def test_a_model_that_never_answers_is_left_out_after_two_tries(self):
        silent = PANEL_MODELS[9]
        self.serve({"*": BUY, silent: dict(BUY, delay=6), "market": self.BULL})
        self.key = TEST_KEY
        self.settings("[nvidia]\ntimeout_seconds=2\n[engine]\nteam_review=false\n")
        self.assertEqual(self.dump("config")["modelTimeout"], 2)
        signal = self.dump("signal")["signal"]
        verdict = self.verdicts(signal)[silent]
        self.assertFalse(verdict["ok"])
        self.assertEqual(verdict["error"], "No answer in 2 s, asked twice.")
        self.assertEqual(len(self.requests("chat", silent)), 2)
        self.assertEqual(signal["voters"], 13)                                # the others decide without it
        self.assertEqual(signal["sbt"]["review"]["agreeing"], 13)
        self.assertTrue(signal["sbt"]["review"]["confirmed"])

    def test_only_so_many_questions_are_with_the_service_at_a_time(self):
        self.serve({"*": dict(BUY, delay=0.4), "market": self.BULL})
        self.key = TEST_KEY
        config = self.dump("config")
        self.assertEqual((config["modelTimeout"], config["modelParallel"]), (180, 6))
        self.settings("[nvidia]\nparallel=3\n")
        signal = self.dump("signal")["signal"]
        self.assertEqual(signal["voters"], 14)                                # everybody still gets its turn
        chats = self.requests("chat", review=None)
        self.assertEqual(len(chats), 20)                                      # fourteen first calls, six reviews
        self.assertEqual(max(c["inflight"] for c in chats), 3)

    def test_a_split_panel_means_no_trade(self):
        # Half the panel confirms the bullish setup, half calls it a sell: the
        # team does not confirm it, so the strategy's entry is not traded.
        plan = {model: (BUY if index % 2 == 0 else SELL) for index, model in enumerate(PANEL_MODELS)}
        plan["market"] = self.BULL
        self.serve(plan)
        self.key = TEST_KEY
        self.settings(AUTO)
        out = self.dump("signal")
        signal = out["signal"]
        sbt = signal["sbt"]
        self.assertEqual(sbt["status"], "entered")             # the rules were met
        action, confidence = self.expected_consensus(signal)
        self.assertEqual((action, confidence), ("hold", 0.0))  # seven against seven
        self.assertAlmostEqual(signal["confidence"], confidence, places=6)
        self.assertLess(signal["confidence"], 60)
        self.assertTrue(sbt["review"]["done"])
        self.assertFalse(sbt["review"]["confirmed"])
        self.assertEqual(signal["voters"], 14)
        self.assertFalse(signal["traded"])
        self.assertFalse(sbt["traded"])
        self.assertIn("did not confirm", sbt["outcome"])
        self.assertEqual(out["account"]["positions"], [])

    # Agreeing and opposing are counted against the setup's direction, also when the team turns it down.
    def test_the_counts_of_a_review_the_team_turns_down_add_up(self):
        plan = {model: (BUY if index < 4 else SELL) for index, model in enumerate(PANEL_MODELS)}
        plan["market"] = self.BULL
        self.serve(plan)
        self.key = TEST_KEY
        signal = self.dump("signal")["signal"]
        self.assertFalse(signal["sbt"]["review"]["confirmed"])
        # Four models agree with the bullish setup, ten oppose it.
        self.assertEqual((signal["voters"], signal["agreeing"], signal["opposing"], signal["holding"]), (14, 4, 10, 0))

    def test_failing_models_are_left_out_not_guessed(self):
        models = PANEL_MODELS
        plan = {"*": HOLD,
                models[0]: {"status": 429}, models[1]: {"status": 500}, models[2]: {"raw": "I think it goes up!"},
                models[3]: {"refusal": True}, models[4]: {"empty": True}, models[5]: {"status": 404},
                models[6]: {"raw": "<think>still thinking about {this}"},
                models[7]: {"raw": "{\"action\": \"moon\", \"confidence\": 99}"}, "market": self.BULL}
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
        self.assertEqual(signal["voters"], 6)  # the six models that answered
        self.assertEqual(self.requests("chat", review=True), [])  # nobody of the team answered: no review
        # Nobody guessed a buy for the ones that failed: the setup is not confirmed.
        self.assertFalse(signal["sbt"]["review"]["confirmed"])

    def test_a_rejected_key_is_reported_without_repeating_what_the_service_said(self):
        self.serve({"*": BUY, "market": self.BULL})
        self.key = "nvapi-wrong-key-00000000000000000000"
        self.settings(AUTO)
        out = self.dump("signal")
        signal = out["signal"]
        verdicts = self.verdicts(signal)
        for model in PANEL_MODELS:
            self.assertFalse(verdicts[model]["ok"])
            self.assertIn("did not accept the API key", verdicts[model]["error"])
        self.assertNotIn("nvapi-wrong", self.last_output)
        self.assertNotIn("abcd", self.last_output)  # the service's echo of the key is not passed on
        # Nobody could answer: nobody confirmed the setup, so its entry is not traded.
        self.assertEqual(signal["voters"], 0)
        self.assertFalse(signal["sbt"]["review"]["confirmed"])
        self.assertEqual(out["account"]["positions"], [])

    def test_a_model_the_service_no_longer_offers_is_left_out(self):
        gone = "z-ai/glm-5.3-flash"
        self.serve({"*": BUY, "missing": [gone], "market": self.BULL})
        self.key = TEST_KEY
        signal = self.dump("signal")["signal"]
        verdicts = self.verdicts(signal)
        # Either it was not asked (the catalog arrived first) or it was and NVIDIA said no.
        if gone in verdicts:
            self.assertFalse(verdicts[gone]["ok"])
            self.assertIn("does not offer this model", verdicts[gone]["error"])
        self.assertEqual(signal["voters"], 13)

    def test_answers_in_a_code_fence_or_after_thinking_are_read(self):
        models = PANEL_MODELS
        self.serve({"*": HOLD, models[0]: dict(BUY, fenced=True), models[1]: dict(BUY, think=True),
                    models[2]: dict(SELL, fenced=True, think=True), "market": self.BULL})
        self.key = TEST_KEY
        verdicts = self.verdicts(self.dump("signal")["signal"])
        self.assertEqual((verdicts[models[0]]["ok"], verdicts[models[0]]["action"]), (True, "buy"))
        self.assertEqual((verdicts[models[1]]["ok"], verdicts[models[1]]["action"]), (True, "buy"))
        self.assertEqual((verdicts[models[2]]["ok"], verdicts[models[2]]["action"]), (True, "sell"))

    # ---- the account over time ---------------------------------------------------------
    # The bullish setup's long is placed at the start (the forming candle is
    # its entry candle, at 101.6 scaled); its stop is about 1.9 % below the
    # entry, Level 3 about 4.8 % above it.
    def test_the_stop_closes_the_trade_when_the_price_falls(self):
        plan = {"*": BUY, "market": self.BULL}
        out = self.act("analyze", plan, AUTO, moves=[(4, dict(plan, market=dict(self.BULL, move_pct=-3.0)))],
                       seconds=13)
        self.assertEqual(out["account"]["positions"], [])
        trades = out["account"]["trades"]
        self.assertEqual(len(trades), 1)
        trade = trades[0]
        self.assertEqual((trade["side"], trade["reason"]), ("buy", "stop_loss"))
        self.assertLess(trade["exit"], trade["entry"])
        # The price went 3 % down past a stop 1.9 % away: the fill is the worse price, not the stop.
        self.assertLess(trade["exit"], out["signal"]["stop"])
        self.assertAlmostEqual(trade["exit"], 101.6 * 0.97 * self.SCALE * 0.9998, delta=0.01)   # with the slippage
        self.assertLess(trade["pnl"], -100)          # 3 % of a third of the account, and the fees
        account = out["account"]["account"]
        self.assertAlmostEqual(account["equity"], 10000 + trade["pnl"], places=6)
        self.assertEqual((account["trades"], account["wins"], account["losses"]), (1, 0, 1))
        self.assertGreater(account["maxDrawdownPct"], 1)
        texts = [a["text"] for a in out["account"]["activity"]]
        self.assertTrue(any("stopped out" in t for t in texts), texts)
        self.assertEqual(out["signal"]["sbt"]["status"], "stopped")   # the strategy saw it too
        self.assertEqual(len(self.dump("account")["trades"]), 1)  # saved

    def test_the_target_closes_the_trade_when_the_price_rises(self):
        plan = {"*": BUY, "market": self.BULL}
        out = self.act("analyze", plan, AUTO, moves=[(4, dict(plan, market=dict(self.BULL, move_pct=6.0)))],
                       seconds=13)
        self.assertEqual(out["account"]["positions"], [])
        trade = out["account"]["trades"][0]
        self.assertEqual(trade["reason"], "take_profit")
        self.assertAlmostEqual(trade["exit"], out["signal"]["takeProfit"], places=6)  # a limit order fills at its price
        self.assertAlmostEqual(trade["exit"], 106.5 * self.SCALE, delta=0.01)           # Level 3
        self.assertGreater(trade["pnl"], 140)  # 4.8 % of a third of the account, less fees
        self.assertEqual(out["account"]["account"]["wins"], 1)
        self.assertAlmostEqual(out["account"]["account"]["winRatePct"], 100)

    def test_a_position_stays_open_while_the_price_is_between_its_levels(self):
        plan = {"*": BUY, "market": self.BULL}
        out = self.act("analyze", plan, AUTO, moves=[(4, dict(plan, market=dict(self.BULL, move_pct=0.3)))],
                       seconds=10)
        self.assertEqual(len(out["account"]["positions"]), 1)
        self.assertEqual(out["account"]["trades"], [])
        self.assertGreater(out["account"]["positions"][0]["pnl"], 0)  # the price went up a little
        self.assertEqual(out["signal"]["sbt"]["status"], "entered")

    def live_bos(self):
        """A bullish 1-2-3 on one-minute candles whose BOS candle is the one
        forming when the session starts: it closes during the session, so the
        setup appears live, not in the history."""
        # The BOS candle must still be forming when the history is read.
        while time.time() % 60 > 45:
            time.sleep(0.5)
        return dict(self.BULL, stage="bos", pattern_interval="1m", at=int(time.time() * 1000))

    def test_a_new_setup_starts_a_review_by_itself_a_closing_candle_does_not(self):
        # One-minute candles: within 70 seconds one closes, the break of
        # structure. That new setup, with nobody asking, is reviewed by the
        # team; the candles that closed before it started nothing.
        market = self.live_bos()
        out = self.act("", {"*": BUY, "market": market}, "[engine]\nauto_trade=true\n[market]\ninterval=1m\n",
                       seconds=72)
        self.assertEqual(out["signal"]["interval"], "1m")
        sbt = out["signal"]["sbt"]
        self.assertEqual((sbt["direction"], sbt["status"]), ("bullish", "armed"))
        self.assertFalse(sbt["historical"])
        self.assertEqual(sbt["bos"]["time"], market["at"] // 60000 * 60000)   # the candle that closed during the session
        self.assertAlmostEqual(sbt["bos"]["price"], 104.4 * self.SCALE, delta=0.01)
        self.assertTrue(sbt["review"]["done"] and sbt["review"]["confirmed"])
        chats = self.requests("chat")
        self.assertEqual(len(chats), 14)   # one review of the new setup: not one per candle or price update
        self.assertTrue(all(c["sbt"] == "bullish" for c in chats))
        # Confirmed, but there is no entry before the retest of the zone.
        self.assertEqual((out["signal"]["action"], out["signal"]["leaning"]), ("hold", "buy"))
        self.assertEqual(out["account"]["positions"], [])
        intervals = {q.split("interval=")[1].split("&")[0] for q in
                     (e.get("query", "") for e in self.requests("market")) if "interval=" in q}
        # The chart's candles, the higher timeframe for context, and the watchlist's 24-hour lines.
        self.assertEqual(intervals, {"1m", "15m", "1h"})

    def test_auto_analyze_off_waits_to_be_asked(self):
        market = self.live_bos()
        plan = {"*": BUY, "market": market}
        out = self.act("", plan, AUTO + "[market]\ninterval=1m\n", seconds=72)
        sbt = out["signal"]["sbt"]
        self.assertEqual((sbt["status"], sbt["historical"]), ("armed", False))
        self.assertEqual(sbt["bos"]["time"], market["at"] // 60000 * 60000)   # it appeared during the session
        self.assertFalse(sbt["review"]["done"])
        self.assertEqual(self.requests("chat"), [])
        self.assertEqual(out["account"]["positions"], [])
        # Asked, the team reviews it.
        signal = self.dump("signal")["signal"]
        self.assertTrue(signal["sbt"]["review"]["done"])
        self.assertEqual(len(self.requests("chat")), 14)

    def test_a_signal_can_be_placed_by_hand_once(self):
        out = self.act("analyze,place,place", {"*": BUY, "market": self.BULL},
                       "[engine]\nauto_analyze=false\n")  # auto trade is off
        results = [a["result"] for a in out["actions"]]
        self.assertEqual(results[:2], ["", ""])   # reviewed, placed
        self.assertNotEqual(results[2], "")       # not twice
        self.assertEqual(len(out["account"]["positions"]), 1)
        self.assertAlmostEqual(out["account"]["positions"][0]["takeProfit"], 106.5 * self.SCALE, delta=0.01)
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
        out = self.act("analyze,wait:6,close-all", {"*": BUY, "market": self.BULL}, AUTO)
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
        plan = {"*": BUY, "market": self.BULL}
        # A 1 % drawdown limit. The price gaps 3 % through the stop (1.9 %
        # away), so the trade (a third of the account) loses a little over 1 %
        # of the account; the daily limit is set wide so only the drawdown
        # limit is in play. Then the price is back, and ETH/USDT has a setup
        # of its own that the team confirms: it is not traded while halted.
        out = self.act("analyze,wait:10,select:ETHUSDT,analyze", plan,
                       AUTO + "[risk]\nmax_drawdown_pct=1\ncooldown_minutes=0\nmax_daily_loss_pct=20\n",
                       moves=[(4, dict(plan, market=dict(self.BULL, move_pct=-3.0))), (8, plan)])
        account = out["account"]["account"]
        self.assertEqual(len(out["account"]["trades"]), 1)
        self.assertEqual(out["account"]["trades"][0]["reason"], "stop_loss")
        self.assertTrue(account["halted"])
        self.assertEqual(out["account"]["positions"], [])  # the second setup was not traded
        signal = out["signal"]
        self.assertEqual(signal["symbol"], "ETHUSDT")
        self.assertTrue(signal["sbt"]["review"]["confirmed"])
        checks = {c["key"]: c for c in signal["risk"]["checks"]}
        self.assertFalse(checks["drawdown"]["pass"])
        self.assertIn("halted", checks["drawdown"]["detail"])
        self.assertIn("Not traded", signal["outcome"])
        self.assertTrue(any("Trading halted" in a["text"] for a in out["account"]["activity"]))
        # Halted survives a restart; resuming lets the next setup through.
        self.assertTrue(self.dump("account")["account"]["halted"])
        resumed = self.dump("session", {"SHADOW_TRADING_SESSION_ACTIONS": "resume,select:SOLUSDT,analyze"})
        self.assertFalse(resumed["account"]["account"]["halted"])
        self.assertEqual([p["symbol"] for p in resumed["account"]["positions"]], ["SOLUSDT"],
                         resumed["signal"].get("outcome"))

    def test_the_daily_loss_limit_stops_new_trades(self):
        plan = {"*": BUY, "market": self.BULL}
        # The gap makes the first trade lose 3 % of its size, a little over 1 %
        # of the account. Then the price is back, and ETH/USDT's own setup,
        # confirmed by the team, is refused for the day.
        out = self.act("analyze,wait:10,select:ETHUSDT,analyze", plan,
                       AUTO + "[risk]\ncooldown_minutes=0\nmax_daily_loss_pct=1\n",
                       moves=[(4, dict(plan, market=dict(self.BULL, move_pct=-3.0))), (8, plan)])
        account = out["account"]["account"]
        self.assertEqual(len(out["account"]["trades"]), 1)
        self.assertLess(account["todayPnlPct"], -1)
        self.assertTrue(account["dailyLossReached"])
        self.assertFalse(account["halted"])  # 1 % is not the 10 % drawdown limit
        self.assertEqual(out["account"]["positions"], [])
        self.assertEqual(out["signal"]["symbol"], "ETHUSDT")
        self.assertTrue(out["signal"]["sbt"]["review"]["confirmed"])
        self.assertIn("Daily loss limit", out["signal"]["outcome"])

    def test_a_setup_the_other_way_closes_the_position(self):
        # The bullish setup's long is open on BTC/USDT. Then BTC/USDT's chart
        # is a bearish 1-2-3 instead, whose entry candle has climbed to 100
        # (scaled): above the long's stop, below the short's. When the chart
        # is read again, the bearish entry closes the long first, then sells.
        bull = {"*": BUY, "market": self.BULL}
        bear = {"*": SELL, "market": dict(self.BEAR, move_pct=(100 / 98.4 - 1) * 100)}
        out = self.act("wait:3,select:ETHUSDT,select:BTCUSDT,wait:2", bull,
                       AUTO + "[sbt]\nai_confirm=false\n[risk]\ncooldown_minutes=0\n", moves=[(2, bear)])
        self.assertEqual((out["signal"]["symbol"], out["signal"]["action"]), ("BTCUSDT", "sell"))
        self.assertEqual(out["signal"]["sbt"]["direction"], "bearish")
        trades = [t for t in out["account"]["trades"] if t["symbol"] == "BTCUSDT"]
        self.assertEqual(len(trades), 1)
        self.assertEqual((trades[0]["side"], trades[0]["reason"]), ("buy", "signal_flip"))
        self.assertAlmostEqual(trades[0]["exit"], 100.0 * self.SCALE * 0.9998, delta=1)   # with the slippage
        self.assertLess(trades[0]["pnl"], 0)                 # bought at 101.6, closed at 100
        short = [p for p in out["account"]["positions"] if p["symbol"] == "BTCUSDT"]
        self.assertEqual([p["side"] for p in short], ["sell"])
        self.assertGreater(short[0]["stop"], short[0]["entry"])
        self.assertAlmostEqual(short[0]["takeProfit"], (200 - 106.5) * self.SCALE, delta=0.01)
        self.assertTrue(any("signal reversed" in a["text"] for a in out["account"]["activity"]))

    # ---- SBT Model 1: where every trade comes from -------------------------------
    BULL = {"pattern": "sbt-bull"}
    BEAR = {"pattern": "sbt-bear"}
    # The pattern's levels at a price of 100 (tests/test_sbt.cpp), scaled to BTC/USDT's 92,000.
    SCALE = 920.0

    def sbt_requests(self):
        return [e for e in self.requests("chat", review=None) if not e.get("desk")]

    def test_sbt_bullish_setup_is_confirmed_by_the_team_and_traded(self):
        self.serve({"*": BUY, "market": self.BULL})
        self.key = TEST_KEY
        self.settings(AUTO)
        out = self.dump("signal")
        signal = out["signal"]
        sbt = signal["sbt"]
        self.assertEqual(signal["strategy"], "SBT Model 1")
        self.assertEqual((sbt["direction"], sbt["status"]), ("bullish", "entered"))
        self.assertAlmostEqual(sbt["level1"]["price"], 103.5 * self.SCALE, delta=0.01)
        self.assertAlmostEqual(sbt["level2"]["price"], 100.0 * self.SCALE, delta=0.01)
        self.assertAlmostEqual(sbt["bos"]["price"], 104.4 * self.SCALE, delta=0.01)
        self.assertAlmostEqual(sbt["level3"]["price"], 106.5 * self.SCALE, delta=0.01)
        self.assertAlmostEqual(sbt["zone"]["bottom"], 100.0 * self.SCALE, delta=0.01)
        self.assertAlmostEqual(sbt["zone"]["top"], 101.2 * self.SCALE, delta=0.01)   # the zone candle's open
        self.assertLess(sbt["previous"]["price"], sbt["level2"]["price"])          # Level 2 is the Higher Low
        self.assertTrue(sbt["zone"]["orderBlock"] and sbt["zone"]["demand"])
        self.assertAlmostEqual(sbt["entry"], 101.6 * self.SCALE, delta=0.01)  # the open of the next candle
        self.assertIn("order_block", [c["kind"] for c in sbt["confirmations"]])
        self.assertTrue(sbt["review"]["done"] and sbt["review"]["confirmed"])
        self.assertTrue(sbt["traded"])
        # The trade is the strategy's: its stop beyond the zone, Level 3 as the target.
        positions = out["account"]["positions"]
        self.assertEqual(len(positions), 1)
        p = positions[0]
        self.assertEqual(p["side"], "buy")
        self.assertAlmostEqual(p["takeProfit"], 106.5 * self.SCALE, delta=0.01)
        self.assertAlmostEqual(p["stop"], sbt["stop"], delta=0.01)
        self.assertLess(p["stop"], 100.0 * self.SCALE)
        # Every model was asked about this setup, not for trades of its own.
        asked = self.sbt_requests()
        self.assertEqual(len([e for e in asked if not e.get("review")]), len(PANEL_MODELS))
        reviews = [e for e in asked if e.get("sbt")]
        self.assertEqual(len(reviews), len(asked))
        self.assertEqual(len(reviews), len(PANEL_MODELS) + 6)  # first calls and the team's final calls
        self.assertTrue(all(e["sbt"] == "bullish" for e in reviews))
        self.assertTrue(all(e.get("system") for e in asked))
        self.assertEqual(sorted({e["seat"] for e in asked if e["seat"]}), sorted(SEATS))

    def test_past_trades_are_drawn_with_their_entry_take_profit_and_stop_loss(self):
        plan = {"*": BUY, "market": self.BULL}
        out = self.act("wait:9", plan, AUTO + "[sbt]\nai_confirm=false\n",
                       moves=[(1, dict(plan, market=dict(self.BULL, move_pct=6.0)))])
        history = out["market"]["sbtHistory"]
        self.assertGreaterEqual(history["candles"], 290)
        entries = [h for h in history["setups"] if h["direction"] == "bullish"]
        self.assertTrue(entries, history)
        h = entries[-1]
        self.assertAlmostEqual(h["entry"], 101.6 * self.SCALE, delta=0.01)
        self.assertEqual(h["result"], "target")   # Level 3 was reached after the move
        self.assertAlmostEqual(h["target"], 106.5 * self.SCALE, delta=0.01)
        self.assertLess(h["stop"], 100.0 * self.SCALE)   # beyond the Level 2 zone
        self.assertGreaterEqual(h["exitTime"], h["entryTime"])   # Level 3 can be reached in the entry candle itself
        # The paper account's own trade on this market: entry, stop, target and how it ended.
        trades = history["trades"]
        self.assertEqual(len(trades), 1)
        t = trades[0]
        self.assertEqual((t["side"], t["open"], t["reason"]), ("buy", False, "take_profit"))
        self.assertAlmostEqual(t["entryTime"], out["account"]["trades"][0]["openedAt"], delta=1)
        self.assertAlmostEqual(t["target"], h["target"], delta=0.01)
        self.assertAlmostEqual(t["stop"], h["stop"], delta=0.01)
        # Kept with the trade in the account's file, for the next start.
        with open(os.path.join(self.data_dir, "paper.json"), encoding="utf-8") as f:
            saved = json.load(f)["trades"][0]
        self.assertAlmostEqual(saved["take_profit"], t["target"], delta=0.01)
        self.assertAlmostEqual(saved["stop"], t["stop"], delta=0.01)

    def test_a_setup_made_while_the_market_gapped_is_not_traded(self):
        # The BOS candle opens with a gap up: nothing is set up from it, nothing is traded.
        self.serve({"*": BUY, "market": dict(self.BULL, stage="gap")})
        self.key = TEST_KEY
        self.settings(AUTO)
        out = self.dump("signal")
        sbt = out["signal"]["sbt"]
        self.assertEqual(sbt["status"], "skipped", sbt)
        self.assertIn("gap", sbt["note"])
        self.assertEqual(out["account"]["positions"], [])
        self.assertEqual(out["account"]["trades"], [])
        self.assertEqual(self.requests("chat"), [])          # and the AI team was not asked about it

    def test_sbt_bearish_setup_is_sold(self):
        self.serve({"*": SELL, "market": self.BEAR})
        self.key = TEST_KEY
        self.settings(AUTO)
        out = self.dump("signal")
        sbt = out["signal"]["sbt"]
        self.assertEqual((sbt["direction"], sbt["status"]), ("bearish", "entered"))
        self.assertAlmostEqual(sbt["level1"]["price"], (200 - 103.5) * self.SCALE, delta=0.01)
        self.assertAlmostEqual(sbt["level3"]["price"], (200 - 106.5) * self.SCALE, delta=0.01)
        self.assertAlmostEqual(sbt["zone"]["top"], 100.0 * self.SCALE, delta=0.01)
        self.assertAlmostEqual(sbt["zone"]["bottom"], (200 - 101.2) * self.SCALE, delta=0.01)   # open to high
        self.assertTrue(sbt["zone"]["orderBlock"] and sbt["zone"]["demand"])                    # order block + supply zone
        self.assertGreater(sbt["previous"]["price"], sbt["level2"]["price"])               # Level 2 is the Lower High
        p = out["account"]["positions"][0]
        self.assertEqual(p["side"], "sell")
        self.assertAlmostEqual(p["takeProfit"], (200 - 106.5) * self.SCALE, delta=0.01)
        self.assertGreater(p["stop"], 100.0 * self.SCALE)

    def test_sbt_setup_the_team_rejects_is_not_traded(self):
        self.serve({"*": HOLD, "market": self.BULL})
        self.key = TEST_KEY
        self.settings(AUTO)
        out = self.dump("signal")
        sbt = out["signal"]["sbt"]
        self.assertEqual(sbt["status"], "entered")  # the rules were met
        self.assertTrue(sbt["review"]["done"])
        self.assertFalse(sbt["review"]["confirmed"])
        self.assertFalse(sbt["traded"])
        self.assertIn("did not confirm", sbt["outcome"])
        self.assertEqual(out["account"]["positions"], [])

    def test_sbt_without_a_key_trades_by_the_rules_alone(self):
        self.serve({"*": BUY, "market": self.BULL})
        self.settings(AUTO)
        out = self.dump("signal")
        self.assertEqual(self.requests("chat"), [])
        sbt = out["signal"]["sbt"]
        self.assertTrue(sbt["traded"])
        self.assertFalse(sbt["review"]["done"])
        self.assertEqual(out["account"]["positions"][0]["side"], "buy")

    def test_sbt_confirmation_can_be_left_out(self):
        self.serve({"*": HOLD, "market": self.BULL})
        self.key = TEST_KEY
        self.settings(AUTO + "[sbt]\nai_confirm=false\n")
        out = self.dump("signal")
        # Traded at once by the rules; the team's later view does not undo it.
        self.assertTrue(out["signal"]["sbt"]["traded"])
        self.assertEqual(len(out["account"]["positions"]), 1)

    def test_sbt_armed_setup_is_reviewed_and_waits(self):
        self.serve({"*": BUY, "market": dict(self.BULL, stage="armed")})
        self.key = TEST_KEY
        self.settings(AUTO)
        out = self.dump("signal")
        signal = out["signal"]
        sbt = signal["sbt"]
        self.assertEqual(sbt["status"], "armed")
        self.assertTrue(sbt["level3Confirmed"])
        self.assertTrue(sbt["review"]["confirmed"])
        self.assertEqual(signal["action"], "hold")       # no entry before the retest
        self.assertEqual(signal["leaning"], "buy")
        self.assertIn("waiting for the retest", signal["note"])
        self.assertEqual(out["account"]["positions"], [])

    def test_sbt_close_through_the_zone_is_not_a_retest(self):
        self.serve({"*": BUY, "market": dict(self.BULL, stage="invalid")})
        self.key = TEST_KEY
        self.settings(AUTO)
        out = self.dump("signal")
        bull = [s for s in out["market"]["sbt"]["setups"] if s["direction"] == "bullish"]
        self.assertEqual(len(bull), 1)
        self.assertEqual(bull[0]["status"], "invalidated")
        self.assertEqual(bull[0]["note"], "a candle closed below the Level 2 zone")
        self.assertFalse(bull[0]["review"]["done"])
        # That close below Level 1's low is itself a bearish break: a new setup, waiting for its retest.
        self.assertTrue(all(e["sbt"] == "bearish" for e in self.sbt_requests()))
        self.assertEqual(out["account"]["positions"], [])

    def test_the_models_never_trade_on_their_own_view(self):
        # Every model says buy, but the made-up market has no entry of the strategy in it.
        self.serve({"*": BUY})
        self.key = TEST_KEY
        self.settings(AUTO)
        out = self.dump("signal")
        self.assertEqual(out["account"]["positions"], [])
        self.assertNotEqual(out["signal"].get("sbt", {}).get("status"), "entered")
        # The models were asked about SBT setups only, if about anything.
        self.assertTrue(all(e.get("sbt") for e in self.sbt_requests()))

    def test_sbt_level3_target_closes_the_trade(self):
        plan = {"*": BUY, "market": self.BULL}
        out = self.act("wait:9", plan, AUTO + "[sbt]\nai_confirm=false\n", moves=[(1, dict(plan, market=dict(self.BULL, move_pct=6.0)))])
        trades = out["account"]["trades"]
        self.assertEqual(len(trades), 1)
        self.assertEqual(trades[0]["reason"], "take_profit")
        self.assertGreater(trades[0]["pnl"], 0)

    def test_sbt_stop_closes_the_trade(self):
        plan = {"*": BUY, "market": self.BULL}
        out = self.act("wait:9", plan, AUTO + "[sbt]\nai_confirm=false\n", moves=[(1, dict(plan, market=dict(self.BULL, move_pct=-3.0)))])
        trades = out["account"]["trades"]
        self.assertEqual(len(trades), 1)
        self.assertEqual(trades[0]["reason"], "stop_loss")
        self.assertLess(trades[0]["pnl"], 0)

    def test_sbt_entry_is_never_made_twice(self):
        plan = {"*": BUY, "market": self.BULL}
        out = self.act("wait:3,close-all,wait:3,analyze", plan, AUTO + "[sbt]\nai_confirm=false\n")
        self.assertEqual(len(out["account"]["trades"]), 1)  # closed by hand, not opened again
        self.assertEqual(out["account"]["positions"], [])


    # One setup is placed by hand once, also after its position was closed (no cooldown to hide a second one).
    def test_sbt_setup_is_placed_by_hand_only_once(self):
        out = self.act("analyze,place,close-all,place", {"*": BUY, "market": self.BULL},
                       "[engine]\nauto_analyze=false\n[risk]\ncooldown_minutes=0\n")
        self.assertEqual([a["result"] for a in out["actions"]][:3], ["", "", "1"])
        self.assertNotEqual(out["actions"][3]["result"], "")   # this setup was placed already
        self.assertEqual(out["account"]["positions"], [])

    # A setup placed by hand says so at once, in the signal and in the market's setups.
    def test_sbt_setup_placed_by_hand_shows_as_traded(self):
        out = self.act("analyze,place,wait:6", {"*": BUY, "market": self.BULL}, "[engine]\nauto_analyze=false\n")
        self.assertEqual(len(out["account"]["positions"]), 1)
        self.assertTrue(out["signal"]["traded"])
        self.assertTrue(out["signal"]["sbt"]["traded"])
        self.assertTrue(out["market"]["sbt"]["current"]["traded"])

    # Reading the chart again (another market and back, or a restart) does not enter the same setup again.
    def test_sbt_setup_is_not_entered_again_when_its_market_is_read_again(self):
        settings = AUTO + "[sbt]\nai_confirm=false\n[risk]\ncooldown_minutes=0\n"
        out = self.act("wait:3,close-all,select:ETHUSDT,select:BTCUSDT,wait:2", {"*": BUY, "market": self.BULL},
                       settings)
        self.assertEqual([(t["symbol"], t["reason"]) for t in out["account"]["trades"]], [("BTCUSDT", "manual")])
        self.assertNotIn("BTCUSDT", [p["symbol"] for p in out["account"]["positions"]])
        # Nor after a restart.
        again = self.dump("signal")
        self.assertNotIn("BTCUSDT", [p["symbol"] for p in again["account"]["positions"]])

    # ---- the desk: the conversation analyst -----------------------------------------
    QUIET = "[engine]\nauto_analyze=false\n"

    def desk(self, steps, plan=None, settings=QUIET, key=True, moves=(), stream=False):
        """Run the desk with the scripted steps (see main.cpp); 'moves' is [(after seconds, scenario)]."""
        if self.server is None:
            self.serve(plan)
        else:
            self.change_scenario(plan or {})
        self.key = TEST_KEY if key else None
        self.stream = stream
        if settings:
            self.settings(settings)

        def mover():
            started = time.time()
            for after, scenario in moves:
                time.sleep(max(0, after - (time.time() - started)))
                self.change_scenario(scenario)

        thread = threading.Thread(target=mover, daemon=True)
        thread.start()
        out = self.dump("desk", {"SHADOW_TRADING_DESK_SCRIPT": json.dumps(steps)})
        thread.join()
        return out

    def mine(self, out):
        return [m for m in out["messages"] if m["role"] == "you"]

    def answers(self, out, kind=None):
        return [m for m in out["messages"] if m["role"] == "desk" and (kind is None or m.get("kind") == kind)]

    def notes_of(self, out, kind=None):
        return [m for m in out["messages"] if m["role"] == "note" and (kind is None or m.get("kind") == kind)]

    def events(self, out, name):
        return [e for e in out["log"] if e["event"] == name]

    def test_desk_builds_on_every_earlier_message(self):
        plan = {"desk": {"answer": {"delay": 1.0}}}
        out = self.desk([{"post": "BTC is approaching 95,000.", "token": "a"},
                         {"post": "RSI is also showing bearish divergence.", "token": "b"},
                         {"post": "What do you think about entering short?", "token": "c"},
                         {"idle": 60}], plan)
        posts = [s for s in out["steps"] if "post" in s["step"]]
        # Accepted at once: nothing is analysed before post() returns.
        for step in posts:
            self.assertTrue(step["result"]["accepted"])
            self.assertLess(step["us"], 50_000, step)
        ids = [int(step["result"]["id"]) for step in posts]
        self.assertEqual(ids, sorted(ids))
        mine = self.mine(out)
        self.assertEqual([m["id"] for m in mine], ids)
        self.assertTrue(all(m["state"] == "understood" for m in mine), mine)
        # Read at once, without a model: what each message said.
        self.assertIn("level 95,000 · approaching", mine[0]["tags"])
        self.assertIn("RSI bearish divergence", mine[1]["tags"])
        self.assertIn("weighing a short", mine[2]["tags"])
        # The level against the live price straight away, from the real quote.
        live = [n for n in self.notes_of(out, "market") if n["text"].startswith("Live BTC/USDT")]
        self.assertEqual(len(live), 1, out["messages"])
        self.assertIn("% above the price", live[0]["text"])
        # One answer, to the question, streamed, below it.
        answers = self.answers(out, "model")
        self.assertEqual(len(answers), 1, out["messages"])
        answer = answers[0]
        self.assertEqual(answer["reply_to"], ids[2])
        self.assertEqual(answer["state"], "done")
        self.assertTrue(answer["text"].startswith("Test answer from moonshotai/kimi-k3.1-pro"), answer["text"])
        self.assertNotIn("```", answer["text"])  # the state block is not shown
        self.assertEqual(answer["by"], "Kimi K3.1 Pro")
        rows = [m["id"] for m in out["messages"]]
        self.assertGreater(rows.index(answer["id"]), rows.index(ids[2]))
        # The answer saw what was said before, without being told again.
        asked = [e for e in self.requests("chat") if e.get("desk") == "answer"]
        self.assertEqual(len(asked), 1)
        self.assertTrue(asked[0]["stream"])
        context = asked[0]["context"]
        self.assertEqual(context["market"], "BTC/USDT")
        self.assertEqual([l["price"] for l in context["analysis"]["levels"]], [95000])
        self.assertIn("RSI bearish divergence", [o["text"] for o in context["analysis"]["observations"]])
        self.assertEqual(context["analysis"]["plan"]["side"], "short")
        self.assertTrue(context["live"]["available"])
        self.assertIn("rsi14", context["live"]["readings"])
        self.assertEqual([t["content"] for t in asked[0]["turns"]],
                         ["BTC is approaching 95,000.", "RSI is also showing bearish divergence."])
        self.assertEqual(asked[0]["question"], "What do you think about entering short?")
        # Read by the model incrementally: every message once, never the whole conversation again.
        reads = [e for e in self.requests("chat") if e.get("desk") == "read"]
        self.assertTrue(reads)
        seen = [i for e in reads for i in e["new_ids"]]
        self.assertEqual(sorted(seen), ids)
        self.assertEqual(len(seen), len(set(seen)))
        # The state the answer left behind, with who said what.
        a = out["analysis"]
        self.assertEqual(a["market"], "BTCUSDT")
        self.assertEqual((a["bias"]["side"], a["bias"]["source"], a["bias"]["by"]), ("bearish", "model", "Kimi K3.1 Pro"))
        self.assertEqual(a["conclusions"][0]["text"], "Test conclusion")
        self.assertEqual([e["text"] for e in a["bearish"] if e["source"] == "you"], ["RSI bearish divergence"])
        self.assertEqual((a["lean"]["side"], a["lean"]["bearish"]), ("bearish", 1))
        self.assertTrue(a["summary"].startswith("Test summary"))
        self.assertEqual(a["levels"][0]["kind"], "level")
        self.assertTrue(a["levels"][0]["hasDistance"])
        # The order of events: all three accepted before the answer was even sent.
        accepted = [e["t"] for e in self.events(out, "accepted")]
        self.assertLess(max(accepted), self.events(out, "answer-asked")[0]["t"])
        self.assertLess(self.events(out, "first-words")[0]["t"], self.events(out, "answered")[0]["t"])
        self.assertTrue(out["status"]["running"])
        self.assertEqual(out["tasks"], [])

    def test_desk_takes_messages_while_an_answer_is_slow(self):
        plan = {"desk": {"answer": {"delay": 4.0}}}
        out = self.desk([{"post": "should I short BTC here?", "token": "q1"},
                         {"wait": 0.5},
                         {"post": "ETH support at 3,100", "token": "s1"},
                         {"post": "and what about ETH?", "token": "q2"},
                         {"post": "should I short BTC here?", "token": "q1"},  # the same draft again
                         {"idle": 60}], plan)
        posts = [s for s in out["steps"] if "post" in s["step"]]
        self.assertFalse(posts[0]["result"]["duplicate"])
        self.assertTrue(posts[3]["result"]["duplicate"])
        self.assertEqual(posts[3]["result"]["id"], posts[0]["result"]["id"])
        for step in posts:
            self.assertLess(step["us"], 50_000, step)
        mine = self.mine(out)
        self.assertEqual([m["text"] for m in mine],
                         ["should I short BTC here?", "ETH support at 3,100", "and what about ETH?"])
        # Two questions, two answers, each below its own question.
        answers = self.answers(out, "model")
        self.assertEqual(sorted(a["reply_to"] for a in answers), [mine[0]["id"], mine[2]["id"]])
        self.assertTrue(all(a["state"] == "done" for a in answers))
        # The later messages were read while the first answer was still on its way.
        first_done = min(e["t"] for e in self.events(out, "answered"))
        later = [e for e in self.events(out, "read") if e["id"] in (mine[1]["id"], mine[2]["id"])]
        self.assertEqual(len(later), 2)
        self.assertTrue(all(e["t"] < first_done for e in later))
        # The conversation moved to ETH, and the chart followed it.
        self.assertEqual(out["analysis"]["market"], "ETHUSDT")
        self.assertEqual(out["symbol"], "ETHUSDT")
        self.assertIn("Chart switched to ETH/USDT", " ".join(n["text"] for n in self.notes_of(out, "info")))
        self.assertEqual([l["price"] for l in out["analysis"]["levels"]], [3100])
        # Both answers were under way at the same time.
        asked = sorted(e["t"] for e in self.events(out, "answer-asked"))
        self.assertLess(asked[1], first_done)

    def test_desk_answers_live_facts_and_needs_no_key_to_follow(self):
        out = self.desk([{"post": "BTC is approaching 95,000."},
                         {"post": "what is the price of BTC?"},
                         {"post": "where is rsi now"},
                         {"post": "should I short?"},
                         {"wait": 1.5}, {"idle": 20}], key=False)
        self.assertEqual(self.requests("chat"), [])  # no key: no model asked
        live = self.answers(out, "live")
        self.assertEqual(len(live), 2)
        self.assertEqual(live[0]["by"], "live data")
        self.assertTrue(live[0]["text"].startswith("BTC/USDT is at "), live[0]["text"])
        self.assertIn("RSI(14) on BTC/USDT", live[1]["text"])
        rsi = out["analysis"]["live"]["rsi14"]
        self.assertIn(f"{rsi:.1f}", live[1]["text"])
        info = self.answers(out, "info")
        self.assertEqual(len(info), 1)
        self.assertIn("no NVIDIA API key", info[0]["text"])
        # Still read and kept, without a model.
        self.assertEqual([l["price"] for l in out["analysis"]["levels"]], [95000])
        self.assertEqual(out["status"]["modelReading"], "no_key")
        self.assertTrue(all(m["state"] == "read" for m in self.mine(out)))

    def test_desk_level_alerts_follow_the_live_price(self):
        self.serve({})
        self.key = TEST_KEY
        price = self.dump("market")["quote"]["close"]
        level = round(price * 1.02)
        out = self.desk([{"post": f"BTC resistance at {level}"}, {"wait": 12}], {},
                        moves=[(4, {"market": {"move_pct": 4.0}})])
        crossed = [n for n in self.notes_of(out, "market") if "crossed above resistance" in n["text"]]
        self.assertEqual(len(crossed), 1, out["messages"])
        level_text = f"{level:,}"
        self.assertIn(level_text, crossed[0]["text"])
        # The price in the note is the live one it was compared with.
        self.assertIn("(live ", crossed[0]["text"])
        lv = out["analysis"]["levels"][0]
        self.assertTrue(lv["above"])
        self.assertGreater(lv["distancePct"], 0)

    def test_desk_asks_the_team_for_a_trade_question_and_shows_its_call_forming(self):
        # A bearish setup has just entered on BTC/USDT, so the team is already
        # reviewing it when the question comes: the desk waits for that call
        # instead of asking a second time.
        plan = {"*": SELL, "z-ai/glm-5.3-flash": dict(SELL, delay=3), "desk": {"answer": {"delay": 0.5}},
                "market": self.BEAR}
        settings = self.QUIET + "team_review=false\n"
        out = self.desk([{"post": "BTC: should I short here?"}, {"idle": 120}], plan, settings=settings)
        notes = [n["text"] for n in self.notes_of(out, "team")]
        self.assertTrue(any(t.startswith("Analysis team on BTC/USDT") and "SELL" in t for t in notes), notes)
        self.assertEqual(len([c for c in self.sbt_requests() if not c.get("review")]), len(PANEL_MODELS))  # once
        self.assertTrue(all(c["sbt"] == "bearish" for c in self.sbt_requests() if c["status"] == 200))
        # Its call formed while the slow member was still working.
        forming = self.events(out, "team-forming")
        self.assertTrue(forming)
        counts = [tuple(int(n) for n in e["detail"].split(" ")[0].split("/")) for e in forming]
        self.assertTrue(any(answered < asked for answered, asked in counts), forming)
        self.assertEqual(out["signal"]["action"], "sell")
        self.assertNotIn("provisional", out["signal"])  # gone once the call is final
        a = out["analysis"]
        self.assertEqual(a["team"]["action"], "sell")
        self.assertIn("Team: SELL", [c["text"] for c in out["signalChanges"]])

    # A setup nobody has reviewed yet is not a fresh call: the desk asks the team about it.
    def test_desk_asks_the_team_about_a_setup_nobody_reviewed_yet(self):
        out = self.desk([{"post": "BTC: should I short here?"}, {"idle": 60}],
                        {"*": SELL, "market": dict(self.BEAR, stage="armed")})
        self.assertEqual(out["signal"]["sbt"]["status"], "armed")
        notes = [n["text"] for n in self.notes_of(out, "team")]
        self.assertTrue(any(t.startswith("Asked the analysis team") for t in notes), notes)
        self.assertTrue(out["signal"]["sbt"]["review"]["done"])
        self.assertTrue(all(c["sbt"] == "bearish" for c in self.sbt_requests()))

    def test_desk_model_reading_adds_what_the_quick_reading_missed(self):
        plan = {"desk": {"read": {"summary": "Trader watches a BTC wedge.",
                                  "observations": [{"topic": "pattern", "text": "Rising wedge", "side": "bearish"}],
                                  "levels": [{"price": 96000, "kind": "resistance"}, {"price": 12, "kind": "level"}],
                                  "plan": {"side": "short", "entry": 0, "stop": 0, "target": 0},
                                  "needs_answer": [1]}}}
        out = self.desk([{"post": "BTC, I am eyeing that wedge near the highs"}, {"idle": 60}], plan)
        a = out["analysis"]
        wedge = [e for e in a["bearish"] if e["text"] == "Rising wedge"]
        self.assertEqual(len(wedge), 1)
        self.assertEqual((wedge[0]["source"], wedge[0]["by"]), ("you", "read by Nemotron 3.5 Bolt"))
        # A number far from the price is not taken as a level.
        self.assertEqual([l["price"] for l in a["levels"]], [96000])
        self.assertEqual(a["summary"], "Trader watches a BTC wedge.")
        self.assertEqual(a["plan"]["side"], "short")
        # Not a question by its words; the model saw that it wants an answer.
        self.assertTrue(self.events(out, "answer-from-reading"))
        self.assertEqual(len(self.answers(out, "model")), 1)
        self.assertEqual(self.mine(out)[0]["state"], "understood")
        self.assertIn("Rising wedge", self.mine(out)[0]["tags"])

    def test_desk_reading_stays_with_the_market_it_was_about(self):
        # The model's reading of the BTC message comes back after the talk moved to ETH.
        plan = {"desk": {"read": {"delay": 2.5,
                                  "first": {"delay": 2.5, "market": "BTC",
                                            "observations": [{"topic": "pattern", "text": "Rising wedge", "side": "bearish"}]},
                                  "market": "", "observations": []}}}
        out = self.desk([{"post": "BTC support at 95,000"}, {"wait": 0.8}, {"post": "ETH holding 3,500"},
                         {"idle": 30}], plan)
        self.assertEqual(out["analysis"]["market"], "ETHUSDT")  # the newer message keeps the focus
        wedge = [c for c in out["changes"] if c["text"].startswith("Rising wedge")]
        self.assertEqual(len(wedge), 1)
        self.assertEqual(wedge[0]["market"], "BTCUSDT")
        self.assertNotIn("Rising wedge", [e["text"] for e in out["analysis"]["bearish"]])

    def test_desk_stop_keeps_what_was_written(self):
        plan = {"desk": {"answer": {"chunk_ms": 150, "text": "word " * 120}}}
        out = self.desk([{"post": "Explain the BTC structure?"}, {"wait": 2.0}, {"stop": "last"}, {"idle": 20}], plan)
        answer = self.answers(out, "model")[0]
        self.assertEqual(answer["state"], "stopped")
        self.assertTrue(answer["text"].startswith("word word"))
        self.assertLess(len(answer["text"]), len("word " * 120))

    def test_desk_conversation_survives_a_restart(self):
        plan = {"desk": {"answer": {"delay": 30}}}
        first = self.desk([{"post": "BTC resistance at 95,000"}, {"post": "should I short BTC?"}, {"wait": 2}], plan)
        self.assertEqual(self.answers(first, "model")[0]["state"], "thinking")  # sent, no words yet
        path = os.path.join(self.data_dir, "desk.json")
        self.assertEqual(stat.S_IMODE(os.stat(path).st_mode), 0o600)
        with open(path, encoding="utf-8") as f:
            self.assertNotIn(TEST_KEY, f.read())
        self.change_scenario({})
        second = self.desk([{"idle": 30}, {"retry": "last"}, {"idle": 60}], {})
        texts = [m["text"] for m in self.mine(second)]
        self.assertEqual(texts, ["BTC resistance at 95,000", "should I short BTC?"])
        answers = self.answers(second, "model")
        self.assertEqual([a["state"] for a in answers], ["superseded", "done"])
        self.assertIn("closed before this answer was complete", answers[0].get("detail", ""))
        self.assertEqual([l["price"] for l in second["analysis"]["levels"]], [95000])
        self.assertEqual(second["analysis"]["market"], "BTCUSDT")
        # New messages go on from the old ids.
        self.assertGreater(answers[1]["id"], max(m["id"] for m in self.mine(second)))

    def test_desk_gold_is_found_where_it_is_listed(self):
        crypto = self.desk([{"post": "XAUUSD looks strong"}, {"post": "GBPUSD looks weak"}, {"idle": 10}], {})
        # Gold has a token on the crypto list; the pound does not.
        self.assertEqual(self.mine(crypto)[0]["market"], "PAXGUSDT")
        warning = self.notes_of(crypto, "warning")
        self.assertEqual(len(warning), 1)
        self.assertIn("GBP/USD is not on your crypto markets", warning[0]["text"])
        self.assertIn("under Forex", warning[0]["text"])
        self.assertEqual(crypto["analysis"]["market"], "GBPUSD")
        self.assertFalse(crypto["analysis"]["live"])
        forex = self.desk([{"post": "gold resistance at 3,600"}, {"idle": 10}], {},
                          settings=FOREX + "[engine]\nauto_analyze=false\n")
        self.assertEqual(forex["analysis"]["market"], "PAXGUSD")
        self.assertEqual(forex["symbol"], "PAXGUSD")

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
