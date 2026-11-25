# Shadow Trading (`shadow-trading`)

A trading desk for Shadow OS. Live prices and a chart on the left; on the
right, what a panel of analysts (models hosted by NVIDIA plus fixed rules)
says about the market, the trade that follows from it, and every trade that
was made.

- **Application id:** `org.shadow.trading`
- **Binary / package:** `shadow-trading` / `shadow-trading`, single instance
- **Mode:** **paper trading**. Orders are simulated against live prices on an
  account kept on this computer. No order leaves the computer and no real
  money is involved. There is no real-money mode in this version (see
  [Limits](#limits)).

This is not financial advice. Language models and indicators describe the
past; neither predicts prices reliably, and several models agreeing is not
evidence that a trade will win. Simulated results usually look better than
real ones.

## How a trade is decided

1. **Prices.** In real time: the data service pushes every trade of the
   selected market, its candles and the watchlist's 24-hour tickers over one
   WebSocket connection (`marketstream.*`), and the chart is redrawn at most
   every 80 ms. Stops and targets are checked on those prices. The history
   comes from ordinary requests (`marketdata.*`), which are also the
   fallback: without the stream, prices are fetched every 5 seconds, and the
   chart's header says which of the two is in use ("Real time" or "Every
   5 s"). A stream that drops is reconnected after 1, 2, 5, 10 and then every
   30 seconds.
2. **Skills** (`skills/`, plain C++17, no Qt):
   - *TA Indicators*: EMA 20/50/200, RSI 14, MACD 12/26/9, Bollinger bands
     20/2, ATR 14, volume against its 20-candle average.
   - *SMC Patterns*: swing points, breaks of structure and changes of
     character, order blocks, fair value gaps, liquidity sweeps, and where
     price sits in the current range.
   These are fixed formulas on closed candles. They measure; they do not
   predict.
3. **Analysts.** When a candle closes (or on *Analyze now*) every model on
   the panel gets the same JSON snapshot (the last 40 candles, the readings
   above, the higher timeframe's trend, any open position) and answers with
   one JSON object: buy, sell or hold, a confidence, a stop and a target, and
   its reasoning. Every model is one of **NVIDIA's hosted models**, reached
   through one endpoint (`https://integrate.api.nvidia.com/v1`,
   `/chat/completions`) with one key. The panel has four roles, and each role
   is briefed with its skills (`prompts/skill-*.md`, compiled into the binary
   and added to the model's instructions):

   | Role | Focus | Skill briefs | Built-in models |
   |---|---|---|---|
   | Analysis (the team) | six seats, one per model, see below | TA Indicators, SMC Patterns, Risk Engine | Mistral Large 4, GLM 5.3 Flash, Gemma 3 12B, Nemotron 3.5 Lightning, Laguna XS 2.1, Gemma 3 4B |
   | Reasoning | argues against the obvious trade first | SMC Patterns, TA Indicators, Risk Engine | Kimi K3.1 Pro, Nemotron 3 Ultra, Step 5 Preview |
   | Vision | also gets a picture of the chart | Chart Reading, SMC Patterns, Risk Engine | Nemotron 3.5 Super VL, Nemotron 3 Nano Omni, Cosmos Reason 2 |
   | Compute | works the numbers | TA Indicators, Risk Engine | MiniMax M3.1 Flash, Nemotron 3.5 Bolt |

   All fourteen ids were in NVIDIA's public model list in late November 2026
   (which then held fourteen general chat models in all; two of the earlier
   panel, Gemma 4 and Muse Glimmer, were no longer listed). The Model Panel
   switches models on and off, tests each one, adds any other chat model from
   NVIDIA's list and restores the built-in panel. A model NVIDIA no longer
   lists is shown as unavailable and not asked.

   **The analysis team.** The analysis models work as a team in two steps.
   - *Seats.* Each member reads the snapshot from one seat, by its place on
     the panel: trend, momentum, volatility, structure, volume and candles,
     higher timeframe. Six models are six different looks at the market, not
     one look six times. A seventh model shares the first seat, and so on.
   - *Team review.* When the team's first calls are in, every member that
     answered is asked once more, with its own call and its teammates' calls
     and reasoning (by seat, without model names), and makes its final call.
     It is told that agreement is not evidence and that keeping its call is a
     normal outcome. The final call replaces the first one; a member that
     fails in the review keeps its first call. Each member still has one
     vote. The review costs one more request per member and can be switched
     off (*Model Panel → Analysis → Team review*, or `team_review=false`).

   The AI Analysis page shows each member's seat, the team's calls, and who
   changed its call in the review. The review makes the team's members less
   independent of each other: agreement after it says less than the same
   agreement before it would.

   A **rule-based analyst** (a fixed scoring of the readings, `core/signal.cpp`)
   always votes, so the application works with no key at all. A model that
   fails, refuses or answers with something other than the JSON object is left
   out of that analysis and shown with the reason; its vote is never guessed.
   An answer wrapped in a code fence or preceded by `<think>` text is read.
4. **Consensus** (`core/signal.cpp`). The side with more confidence behind it
   wins if enough analysts are on it. *Confidence* = (confidence of the
   agreeing analysts − confidence of the opposing ones) ÷ number of analysts
   that answered. It measures agreement, **not** the chance of profit. The
   stop and target are the middle of what the agreeing analysts proposed; the
   entry is the market price at that moment.
5. **Risk Engine** (`core/risk.cpp`). Fixed arithmetic that no model takes
   part in: minimum confidence and agreement, a stop on the right side and
   within range, minimum reward to risk, one position per market and a
   maximum in total, a daily loss limit, a drawdown limit that halts trading
   until you resume it, a cooldown after a loss, and the order size: a set
   share of the account lost if the stop is hit, never more than the balance
   covers, and never more than one position's share of the account (a third
   with three positions allowed), so a tight stop cannot put everything into
   one trade.
6. **Execution Engine** (`core/paper.cpp`). With *Auto trade* on, a signal
   that passes the rules is placed on the paper account: a market order with
   fee and slippage, long or short. The stop and target are checked on every
   price update; a stop fills at the stop or, if price has gone past it, at
   the worse price; a target fills at its price; when one update covers both,
   the stop is assumed first. A signal that clearly reverses closes the open
   position. With *Auto trade* off (the default) the signal is shown and
   *Place paper trade* places it by hand.

Everything on screen is real: prices from the exchange, verdicts from the
models that answered, statistics from the trades the account made.

## The window

The window is always dark with a violet accent, whatever the system theme
(`Shadow::Appearance::pin()`; the user's settings are neither read for these
two nor changed).

- **Live Trading**: the markets with 24-hour lines, chart, the signal (action,
  confidence, entry, stop, target, reasoning), models and skills, live trades,
  portfolio, risk.
  **Markets**: the **+** next to the cards opens *Add a market*: search what
  the data service lists and add up to
  12; the new market goes onto the chart at once. The **×** on a card (or
  Delete with the card focused) removes it; the last market, and one with an
  open position, cannot be removed. More markets than fit scroll sideways.
  The chart has two views: **TradingView** (TradingView's own embeddable
  chart, loaded from tradingview.com in a web view; drawing tools and
  indicators are TradingView's) and **AI chart** (drawn from the candles the
  analysts read, with EMA 20/50, Bollinger bands and the trade's entry, stop
  and target). The AI chart moves like any trading chart:

  | Do | With |
  |---|---|
  | Move through time | drag; ← →; Shift + wheel; a sideways swipe |
  | Zoom (20 to 2,000 candles) | wheel, around the pointer; + and −; the two buttons |
  | Back to the newest candles | the *Live* button; Home; double-click (also resets the zoom) |
  | Read a candle | point at it: a crosshair with its time and the price at the pointer, and its open, high, low and close in the header |

  Older candles are fetched as the view gets near the oldest one loaded, 1,000
  at a time, back to the start of the market's history (at most 20,000 are
  kept). While the view is in the past it stays on the same candles as new
  ones arrive.
- **AI Analysis**: every analyst's verdict, the risk rules one by one, the
  current readings, the activity log.
- **Trade History**, **Portfolio** (account value over time, open positions,
  reset), **Risk Manager** (all limits), **Model Panel** (the key, the models
  of each role, connection tests), **Settings**, **About**.

`shadow-trading <page>` opens a page: `live`, `analysis`, `history`,
`portfolio`, `risk`, `models`, `settings`, `about`.

## The key

No key is needed to start. The NVIDIA key (get one at build.nvidia.com; keys
start with `nvapi-`) is saved from the Model Panel into
`~/.config/shadow/trading/nvidia.key`, readable only by you (mode 600; the
directory 700). It is read at run time, sent only to NVIDIA's endpoint in the
request's `Authorization` header, and never logged or shown again (only its
last four characters). A rejected key is reported in the application's own
words: a service may quote part of the key back in its error text, and that
text is not passed on.

Where the key comes from, first match wins:

1. the environment variable `NVIDIA_API_KEY`;
2. the file saved from the Model Panel;
3. the key saved in Shadow Assistant (`~/.config/shadow/assistant.key`), when
   it is an NVIDIA key.

A key that plainly belongs to another company (`sk-ant-…`, `sk-…`, `AIza…`),
wherever it was found, is passed over and never sent to NVIDIA.

Every analysis is one request per enabled model, and one more per member of
the analysis team for its review (fourteen plus six with the built-in panel).
They count against your NVIDIA account. *Settings → Analyze when a
candle closes* turns the automatic analyses off.

## Files

| Path | What |
|---|---|
| `~/.config/shadow/trading.conf` | settings (INI, see below) |
| `~/.config/shadow/trading/nvidia.key` | the NVIDIA API key |
| `~/.local/share/shadow/trading/paper.json` | the paper account: balance, open positions, trade history, equity curve (mode 600). A file that cannot be read is moved to `paper.json.unreadable`, never overwritten |

```ini
[market]
; Binance-compatible public market data; no account, no key.
; data_url=https://data-api.binance.vision
; The live stream (WebSocket). "off": no stream, prices every 5 seconds.
; Unset with a data_url of your own: no stream either.
; stream_url=wss://data-stream.binance.vision
; Up to 12 markets, by the names the data service uses; the window's "+"
; and "×" write this line. A name the service does not list is marked "Not
; listed" on its card and left out of the price requests; the other markets
; are not affected.
; watchlist=BTCUSDT,ETHUSDT,SOLUSDT,PAXGUSDT,EURUSDT
symbol=BTCUSDT
interval=15m

[engine]
auto_trade=false
auto_analyze=true
; The analysis team reads each other's first calls and then makes its final ones.
team_review=true

[risk]
risk_per_trade_pct=1
max_daily_loss_pct=3
max_drawdown_pct=10
min_reward_risk=1.5
min_confidence=60
min_agreeing=2
max_open_positions=3
cooldown_minutes=30

[paper]
starting_balance=10000
fee_pct=0.1
slippage_pct=0.02

[panel]
; Model ids per role, comma-separated (unset = the built-in panel).
; analysis=mistralai/mistral-large-4-1t-a52b, z-ai/glm-5.3-flash
; reasoning=
; vision=
; compute=
; Models that stay on the panel but are not asked:
; off=

[nvidia]
; base_url=https://integrate.api.nvidia.com/v1
```

A value outside its allowed range is ignored and the default used; a model id
that is not of the form `publisher/model` is ignored.

## Data sources and runtime dependencies

- Market data: `GET <data_url>/api/v3/klines` (with `endTime` for older
  candles), `/api/v3/ticker/24hr` and, for *Add a market*,
  `/api/v3/ticker/price` (QtNetwork); the live stream
  `<stream_url>/stream?streams=<market>@aggTrade/<market>@kline_<interval>/<each market>@miniTicker`
  (QtWebSockets). Gold and the euro are shown as they trade on that exchange:
  the PAX Gold token (`PAXGUSDT`) and `EURUSDT`, not spot XAU/USD or EUR/USD.
- Models: HTTPS requests to NVIDIA's endpoint (QtNetwork), one connection pool
  per request in flight so all models are asked at once.
- Packages: `shadow-ui`, `libqt6websockets6`; for the TradingView view `qml6-module-qtwebengine`
  (recommended; without it only the AI chart is offered and the application
  says so).
- No privileged operation, no helper, no polkit action.

## Diagnostics and tests

`shadow-trading --dump <topic>` prints what the window would show, as JSON:
`config` (never the key), `account`, `market`, `signal` (runs one analysis),
`session` (runs the engine for `SHADOW_TRADING_SESSION_SECONDS` and the
scripted `SHADOW_TRADING_SESSION_ACTIONS`, then reports).

| Test | What it covers | Run |
|---|---|---|
| `tests/test_skills.cpp` | indicators and patterns against hand-worked values (3276 checks) | build command in the file's header; plain `g++`, sanitizers |
| `tests/test_core.cpp` | consensus, rule-based analyst, risk rules, paper account (309 checks) | as above |
| `tests/test_app.py` | the real binary against `tests/fake_services.py` (49 tests): the watchlist and markets the data service does not list, adding and removing markets, the live stream (prices, a stop hit between two polls, a stream that drops or is switched off, recorded messages of the real service), older candles back to the start of history, the panel and its skill briefs, the analysis team's seats and its review (a changed call, a failed review, review off, a team of one), signals, trade placement, stops and targets as the price moves, the candle-close loop, manual placing and closing, halts, key handling, failing models, bad files | `SHADOW_TRADING_BIN=… python3 -B tests/test_app.py` |

`tests/fake_services.py` stands in for the exchange and for NVIDIA's model
service on one local port, checks each request (the key, the model, the shape
of the body) and records it. `tests/with-fakes.sh shadow-trading` runs the
application against it by hand. Test hooks: `SHADOW_TRADING_DATA_URL`,
`SHADOW_TRADING_NVIDIA_URL`, `SHADOW_TRADING_STREAM_URL`,
`SHADOW_TRADING_ANALYZE_ON_START=1`.

## Limits

- **No real-money trading.** The Execution Engine has one back end, the paper
  account. A live back end is added per exchange and tested on that
  exchange's test network before it is offered.
- **One market is analyzed at a time**: the one on the chart. Positions on
  other watchlist markets keep their stops and targets (followed through the
  ticker prices), but no new signal is produced for them until selected.
- **The application has to be running.** Analyses, stops and targets are
  handled by the running application; while it is closed nothing is watched,
  and a stop that was passed meanwhile fills at the first price seen
  afterwards.
- Markets that are not on the chart get one price a second from the stream
  (every 5 seconds without it), so a move and its reversal inside that
  interval can be missed there; on the chart every trade is seen.
- The built-in data service lists crypto pairs only. Gold and the euro are
  the PAX Gold token and EUR/USDT as they trade there; stocks, indices and
  spot forex are not available from it.
- The risk checks' detail texts and the rule-based analyst's reasoning come
  from the Qt-free core in English and are not translated.
- **Checked against the real services:** the market data requests and the
  live stream of the built-in data service (its messages were recorded and
  are replayed in the tests, `tests/fixtures/real-stream.jsonl`), and NVIDIA's
  public model list, to confirm the panel's ids.
- **Not verified here** (no key in the build environment): real answers from
  NVIDIA's models. That every one of the fourteen answers in the JSON format,
  that the three vision models accept the chart picture, and how the team
  behaves in its review were exercised only against the stand-in; a model
  that does not cope is shown with its error on the Model Panel and left out.
  The TradingView chart was checked only up to its "could not be reached"
  fallback to the AI chart. The chart's mouse handling was driven in a
  virtual display (drag, wheel, pointer), not by hand on a real screen.
