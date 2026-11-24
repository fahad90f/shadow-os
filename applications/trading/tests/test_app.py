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
from fake_services import PANEL, PANEL_MODELS, TEST_KEY  # noqa: E402  (the stand-in's own test key)

BUY = {"action": "buy", "confidence": 85, "stop_pct": -1.0, "target_pct": 2.5}
SELL = {"action": "sell", "confidence": 85, "stop_pct": -1.0, "target_pct": 2.5}
HOLD = {"action": "hold"}
# Auto trade on. The analyses are the ones a test asks for: left on, the
# application would also analyze whenever a real quarter-hour passes mid-test.
AUTO = "[engine]\nauto_trade=true\nauto_analyze=false\n"


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

    def requests(self, kind=None, model=None):
        if not os.path.exists(self.log):
            return []
        with open(self.log, encoding="utf-8") as f:
            entries = [json.loads(line) for line in f if line.strip()]
        return [e for e in entries
                if (kind is None or e["kind"] == kind) and (model is None or e.get("model") == model)]

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
        # The built-in panel: twelve NVIDIA-hosted models, three per role.
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
        self.assertEqual(signal["voters"], 13)
        self.assertTrue(all(c["auth"] == "ok" for c in self.requests("chat")))

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
        self.assertEqual(len(self.requests("chat")), 12)

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
                      "reasoning=\nvision=meta/muse-glimmer-30b, moonshotai/kimi-k3.1-pro\ncompute=\n")
        config = self.dump("config")
        self.assertEqual([(m["model"], m["role"]) for m in config["panel"]],
                         [("moonshotai/kimi-k3.1-pro", "analysis"), ("meta/muse-glimmer-30b", "vision")])
        signal = self.dump("signal")["signal"]
        self.assertEqual(set(self.verdicts(signal)), {"rules", "moonshotai/kimi-k3.1-pro", "meta/muse-glimmer-30b"})
        self.assertEqual(self.requests("chat", "moonshotai/kimi-k3.1-pro")[0]["images"], [])
        self.assertEqual(self.requests("chat", "meta/muse-glimmer-30b")[0]["images"], ["960x540"])

    def test_a_model_switched_off_is_not_asked(self):
        off = ["z-ai/glm-5.3-flash", "nvidia/nemotron-3.5-bolt-e4b"]
        self.serve({"*": BUY})
        self.key = TEST_KEY
        self.settings("[panel]\noff=" + ", ".join(off) + "\n")
        signal = self.dump("signal")["signal"]
        asked = sorted(c["model"] for c in self.requests("chat"))
        self.assertEqual(asked, sorted(m for m in PANEL_MODELS if m not in off))
        self.assertEqual(signal["voters"], 11)

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
        self.assertEqual(signal["voters"], 13)
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
        self.assertEqual(signal["voters"], 5)  # the rules and the four models that answered

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
        self.assertEqual(signal["voters"], 12)

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
        self.assertGreaterEqual(len(chats), 12)
        self.assertLessEqual(len(chats), 24)  # one candle closed, two at most: not an analysis per price update
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
