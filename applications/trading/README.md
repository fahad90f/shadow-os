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

   **Slow models.** A large model can take minutes, and a hosted service
   sometimes drops a request that would succeed a moment later. A model is
   given 180 seconds for an answer (*Model Panel → Time to answer*, 30 to
   600). One that does not answer in that time, or gets a "too many
   requests" or server error, is asked once more before it is left out, and
   its row says so while that happens. At most six questions are with the
   service at a time (`nvidia/parallel`); the others wait their turn, which
   does not count against their time. The analysis waits for every model, so
   one that needs both tries delays the signal by up to twice that time.
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

## The AI Desk

A conversation with an analyst that follows it, and the market, for as long
as the application runs, whatever page is open. Nothing has to be started and
there is no "Analyze" step: every message is taken in as it arrives.

**Layers, none of which waits for another.**

1. *Accepted.* `DeskEngine::post()` puts the message in the conversation and
   returns at once (well under a millisecond); the box is emptied only then.
   The box is never locked: a message can be written and sent while answers
   to earlier ones are still being written. A draft has a token, so the same
   draft sent twice (a double Enter) is one message. Messages keep their order.
2. *Read*, on the next turn of the event loop, by `core/desk.cpp` (no model):
   the market named (BTC, ETH, gold, EUR/USD, … resolved to a market on the
   list), price levels (only where the words around a number make it one:
   "approaching 95,000", "stop at 95.8k", not "RSI 72" or "the 200 EMA"; and
   only within half to twice the live price), indicator and chart
   observations (RSI divergence, overbought, MACD cross, patterns, structure,
   trend, news), which way the message leans, the trade being weighed, and
   whether it asks something. The chips under a message show what was taken
   from it. Levels are compared with the live price at once.
3. *Understood* in the background by a quick model (`desk/fast_model`,
   built-in Nemotron 3.5 Bolt), which gets only the new messages, the running
   summary and the market's current analysis, never the whole conversation
   again, and returns what to add (observations, levels, plan, which messages
   want an answer, a new summary). Messages that arrive while it works are
   read together in its next request.
4. *Answered* when a message asks something. A question that only asks for
   live facts (price, RSI, MACD, trend, the team's call, the open position,
   the levels named) is answered at once from the live data, without a model.
   Other questions are answered by a model, streamed word by word: the deeper
   one (`desk/deep_model`, built-in Kimi K3.1 Pro) for trade decisions and
   analysis, the quick one for short questions. The answer gets the
   accumulated analysis, the live readings of the market, the analysis team's
   last call and the recent conversation, and is told to keep apart what the
   trader said, what the live data shows and what the team concluded. Several
   answers can be under way at once; each stays below its question and can be
   stopped (what was written stays) or asked again.

**The analysis kept** (one per market, `core/desk.h`): signals by side,
indicator observations, levels with their distance from the live price, the
trade weighed (side, entry, stop, target, reward/risk or what is wrong with
it), the assessed bias, conclusions, a running summary, and every change with
its time. Each entry says where it came from: *you* (what the trader said,
also when a model read it from their words), a model by name, the live market,
or the analysis team. Only the changed fields are updated.

**The market side**, independent of the conversation: the live price against
every level named (a note when it comes close, half an ATR on the chart's
market, and when it crosses), the chart's indicator readings compared every
few seconds (RSI zone, MACD state, trend, structure events, listed as market
signal changes), and the analysis team. A trade question about the chart's
market starts the team in the background when its last call there is missing
or older than two candles; the answer does not wait for it, and the team's
call arrives as a note and becomes the market's bias. While the team works,
its call as it forms (the analysts that have answered so far) is shown on the
signal panel and in the desk's task list; it is never traded on.

**Live status** (right column): whether the desk runs, the conversation
monitored (messages read, the last one), the model reading (on, no key, an
error), the market feed (stream, polling, error), the last update and where
it came from, and each task under way with its model, stage and how long it
has run. Each value comes from a real event; nothing is shown as live that is
not.

**Without a key** the desk still reads every message, keeps the analysis,
watches the levels and answers live facts; for an opinion it says that a
model is needed. Nothing is sent anywhere.

The conversation and the analysis are saved in `desk.json` (mode 600) and come
back at the next start. An answer cut off by closing the application is
marked so and can be asked again; a message that was not yet read by the
model is read then.

## The window

The window is always dark with a violet accent, whatever the system theme
(`Shadow::Appearance::pin()`; the user's settings are neither read for these
two nor changed).

- **Live Trading**: the markets with 24-hour lines, chart, the signal (action,
  confidence, entry, stop, target, reasoning), models and skills, live trades,
  portfolio, risk.
  **Crypto or forex**: the switch at the top chooses which markets the desk
  shows. *Crypto*: coins against USDT, from Binance's public data. *Forex*:
  the US dollar against the euro, the pound, the Australian and Canadian
  dollar and the franc, and gold, from Kraken's public data; the account is
  then counted in dollars. Each has its own list of markets. The paper
  account is the same one, so the switch is refused while a position is open.
  **An open position stays on show.** While a position is open on the market
  on the chart, its entry, stop and target are the lines on the chart (the
  entry line carries its side and running profit) and the three boxes of the
  signal panel, and a strip under them shows its size and profit with a
  *Close position* button. The newest analysis may say hold, or something
  else, without the position disappearing from view; it leaves when it
  closes.
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
- **AI Desk**: the conversation on the left (each message with the chips of
  what was taken from it, answers written word by word with Stop and Ask
  again, notes from the market and the analysis team), the live analysis on
  the right (status, tasks under way, the market's bias and live readings,
  signals, levels, the trade weighed, conclusions and changes). See *The AI
  Desk* above. Live Trading has a button with the desk's state that opens it.
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
| `~/.local/share/shadow/trading/desk.json` | the AI Desk: the conversation (the newest 400 messages), the analysis of each market discussed, the changes (mode 600). A file of another version is moved to `desk.json.unreadable` |

```ini
[market]
; crypto (default) or forex. The window's switch writes this line.
; kind=crypto
; Forex has its own source, list and selection: forex_data_url
; (https://api.kraken.com), forex_stream_url (wss://ws.kraken.com),
; forex_watchlist (EURUSD,GBPUSD,AUDUSD,USDCAD,USDCHF,PAXGUSD), forex_symbol.
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
; Seconds a model is given for one answer (asked once more when it does not).
; timeout_seconds=180
; Questions with the service at a time; the others wait their turn.
; parallel=6
```

A value outside its allowed range is ignored and the default used; a model id
that is not of the form `publisher/model` is ignored.

## Data sources and runtime dependencies

- Forex market data: `GET <forex_data_url>/0/public/OHLC`, `/0/public/Ticker`
  and the stream `<forex_stream_url>/v2` (channels `trade`, `ohlc`,
  `ticker`, subscribed to once connected).
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
scripted `SHADOW_TRADING_SESSION_ACTIONS`, then reports), `desk` (runs the
desk through the steps of `SHADOW_TRADING_DESK_SCRIPT`, a JSON list of
`post`, `wait`, `idle`, `stop`, `retry`, `select`, `analyze`, `clear`, then
prints the conversation, the analysis, the status and a timed event log).

| Test | What it covers | Run |
|---|---|---|
| `tests/test_skills.cpp` | indicators and patterns against hand-worked values (3276 checks) | build command in the file's header; plain `g++`, sanitizers |
| `tests/test_core.cpp` | consensus, rule-based analyst, risk rules, paper account, markets priced in another currency (346 checks) | as above |
| `tests/test_desk.cpp` | the desk's reading of messages (markets, levels and the numbers that are not levels, observations, intent, Roman Urdu questions) and its analysis state (books per market, merging, plan and reward/risk, levels against the live price) (134 checks) | `tests/run-unit.sh desk` |
| `tests/test_app.py` | the real binary against `tests/fake_services.py` (62 tests): the watchlist and markets the data service does not list, adding and removing markets, the forex markets (the other service's requests and stream, what is offered, a dollar-base pair counted in dollars, switching kinds, recorded answers and messages of the real service), an open position staying on show after a hold, models that are slow or fail once (asked again, left out after two tries, only so many at a time), the live stream (prices, a stop hit between two polls, a stream that drops or is switched off, recorded messages of the real service), older candles back to the start of history, the panel and its skill briefs, the analysis team's seats and its review (a changed call, a failed review, review off, a team of one), signals, trade placement, stops and targets as the price moves, the candle-close loop, manual placing and closing, halts, key handling, failing models, bad files; and the AI Desk (9 tests): messages accepted at once and built on (the answer sees the earlier messages, the model reads each message once), messages taken while a slow answer is written (two answers under way, a double send is one message, the chart following the conversation), live facts and no key, level alerts from the live price, the team started for a trade question and its call forming, a model's reading adding what the quick reading missed, stopping an answer, the conversation surviving a restart, gold found where it is listed | `SHADOW_TRADING_BIN=… python3 -B tests/test_app.py` |

`tests/fake_services.py` stands in for the exchange and for NVIDIA's model
service on one local port, checks each request (the key, the model, the shape
of the body) and records it. `tests/with-fakes.sh shadow-trading` runs the
application against it by hand. Test hooks: `SHADOW_TRADING_DATA_URL`,
`SHADOW_TRADING_NVIDIA_URL`, `SHADOW_TRADING_STREAM_URL`,
`SHADOW_TRADING_ANALYZE_ON_START=1`.

## Limits

- **The desk's answers are only as good as the model.** The quick reading on
  this computer is cautious and English-centred (with a few Roman Urdu
  question words); what it misses, the model's reading adds. Neither the
  models' real answers nor real streaming from NVIDIA could be checked in the
  build environment (no key): the stand-in service streams the same way the
  OpenAI-compatible protocol does.
- **Indicators are measured for the chart's market.** For another market the
  desk has its price only; "Chart follows the conversation" (on by default)
  switches the chart to the market being discussed when it is on the list.

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
- **Forex prices are an exchange's, not a broker's.** They are the prices at
  which the currency pairs trade on Kraken: within a pip or so of the
  interbank rates for the pairs that trade often there, and trading goes on
  at weekends. Offered are the pairs with the US dollar, and gold (the PAX
  Gold and Tether Gold tokens against the dollar), that had at least 500
  trades in the last 24 hours. At the time of writing that is EUR/USD,
  GBP/USD, AUD/USD, USD/CAD, USD/CHF and the two gold tokens; **USD/JPY had
  about thirty trades a day there, with prices percent apart, and is not
  offered**. Crosses (EUR/GBP, EUR/JPY) are not offered: their profit would
  need a second exchange rate. That service keeps the newest 720 candles of
  an interval (a week of 15-minute candles, two years of daily ones) and
  the chart goes back no further. No leverage, swaps or lot sizes are
  simulated: an order is a plain amount of the base currency, paid in full.
  Stocks and indices are not available from either service.
- On a market whose base is the dollar (USD/CHF, USD/CAD) the quantity is
  dollars, and a profit in the other currency is turned into dollars at the
  price it is taken at (`Trading::notionalOf`, `moveValue` in `core/paper.h`).
- The risk checks' detail texts and the rule-based analyst's reasoning come
  from the Qt-free core in English and are not translated.
- **Checked against the real services:** the market data requests and the
  live streams of both data services (their answers and messages were
  recorded and are replayed in the tests, `tests/fixtures/real-stream.jsonl`
  and `tests/fixtures/real-kraken-*`), and NVIDIA's public model list, to
  confirm the panel's ids. The application itself was run against stand-ins
  shaped after those recordings, not against the services directly.
- **Not verified here** (no key in the build environment): real answers from
  NVIDIA's models. That every one of the fourteen answers in the JSON format,
  that the three vision models accept the chart picture, and how the team
  behaves in its review were exercised only against the stand-in; a model
  that does not cope is shown with its error on the Model Panel and left out.
  The TradingView chart was checked only up to its "could not be reached"
  fallback to the AI chart. The chart's mouse handling was driven in a
  virtual display (drag, wheel, pointer), not by hand on a real screen.
