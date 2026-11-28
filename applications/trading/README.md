# Shadow Trading (`shadow-trading`)

A forex trading desk for Shadow OS: gold (XAU/USD), silver (XAG/USD), the
euro (EUR/USD) and the other dollar pairs. Prices and a chart on the left; on the
right, the setup the trader's own strategy (**SBT Model 1**) has found, what
a panel of AI analysts (models hosted by NVIDIA) says about that setup, the
trade that follows from it, and every trade that was made. The models do
not trade on their own view: every trade comes from the strategy.

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
3. **SBT Model 1** (`core/sbt.cpp`, plain C++17, unit-tested in
   `tests/test_sbt.cpp`): the trader's strategy, and the only source of
   trades. One engine for both directions; bearish mirrors bullish:
   1. a proper **1-2-3** of the **external** structure: Level 1 the
      external high (the top of the last leg, confirmed by a valid pullback:
      a candle closing below the low of the candle that made it), Level 2
      the lowest low of the pullback after it. Swings inside a leg or inside
      the pullback are internal structure and are never Level 1, 2 or 3;
      a close above an internal lower high is no setup;
   2. a **break of structure** (BOS): a candle **closes** above Level 1;
   3. the **Level 2 zone**: at the valid swing low, the lowest **bearish**
      candle between Level 1 and the BOS, **from its opening price to its
      lowest wick** (bearish: at the swing high, the highest bullish candle,
      from its open to its highest wick); no such candle there, no setup.
      **That same candle must be both the order block and the demand zone**
      (bearish: the supply zone), or the setup is skipped with the reason:
      *order block*: the next candle goes the other way (bullish) and,
      within three candles, one closes above the zone candle's high, so it
      is the last bearish candle before the move; *demand*: the best close
      of those three candles is at least one ATR beyond its open
      (*Demand / supply: move away*) and no candle comes back into the zone
      before the BOS (fresh);
   4. **Level 3**: the top of the leg after the BOS, confirmed by a valid
      pullback (a close below the low of the candle that made it) or by a
      candle that comes back to the zone without a new high; it follows a
      new high until then, also through pauses inside the leg;
   5. the **retest**: a closed candle reaches the zone; if it (or any candle
      before it) closes **below** the zone, the setup is invalid;
   6. **entry at the next candle's open**, target **Level 3**, the stop past
      the zone and Level 2 by a quarter ATR (*Stop beyond the zone*); entered
      only when reward/risk meets the minimum of the risk rules.

   These follow the strategy's own 7 rules (a proper 1-2-3, a BOS, the
   lowest bearish candle at 2 marked, wait for the test, the testing candle
   must not close beyond the zone, enter at the next candle's open, target
   Level 3); `tests/test_sbt.cpp` replays the strategy's own bullish and
   bearish charts candle by candle with the default settings.

   **PD arrays** are an optional extra, off by default (*Also require a PD
   array*): fair value gaps, order blocks (the last opposite candle before a
   displacement) and demand zones (a small-bodied base before a
   displacement); for a bearish setup supply. Valid only if on the setup's
   side, at or below Level 2 (bearish: at or above), within 1.5 ATR,
   unbroken (no close through it), a gap not yet filled, and reached by the
   move into the zone; the candle that marks the zone does not confirm
   itself. They are listed with a setup when present; only with the filter
   on must one confirm the retest.

   **Gap filter.** A candle that opens beyond the previous candle's range
   and at least half an ATR from its close (*Gap: size*) is a gap up or
   down. From that candle on, nothing is analysed or traded: a BOS then is
   skipped (and the AI team is not asked), a retest then is skipped, and an
   entry candle that opens with a gap is skipped, until the gap is filled
   (price trades back to the close before it) or six more candles have
   closed (*Gap: pause*). Open trades keep their stop and target. The
   strategy card says when a pause is on.

   External structure, as the engine follows it: a bullish BOS makes Level 2
   the external (protected) low and starts the leg up; the leg's top becomes
   the external high at its valid pullback. The next close above that high
   is the next bullish setup; a close below the external low is a bearish
   one (Level 1 that low, Level 2 the highest point between). The structure
   starts from the first candle's range; a break of that start range only
   starts the structure and is never a setup.

   Only closed candles decide; the forming candle gives the entry's open
   and an "in the zone now" note that is shown, never acted on. Nothing is
   recomputed later: an entered setup keeps its levels. Each setup (one per
   Level 1 and market) enters at most once, also after the market is opened
   again or the application restarts (the handled setups are kept with the
   paper account), and is placed by hand at most once. Candles missing after
   a gap (the computer slept) are read as history, not traded. Work per candle is constant: the engine
   keeps its own state (swings, setups, PD arrays, a bounded window of
   candles) and never rescans the history. When a market or interval is
   opened, its history is read once as **history**: setups found there are
   shown but never traded, except an entry whose candle is the one forming
   now. A setup expires without a retest within 200 candles.
4. **The AI team reviews each setup** (it does not look for trades). As
   soon as a setup is armed (structure, BOS, zone known; waiting for the
   retest), every model on the panel gets the market snapshot (the last 40
   candles, indicators, structure, the higher timeframe) and the setup, with
   the strategy's rules in its instructions, and answers with one JSON
   object: the setup's direction to confirm it, hold to reject it, a
   confidence and its reasoning. The review happens while the setup waits,
   so an entry need not wait for it; an entry that comes first waits for the
   review and is then filled at the price of that moment. *Review setup now*
   asks again. With *AI team confirms each setup* on (the default) an entry
   is placed only when the team confirms: the setup's direction wins the
   consensus with at least the minimum confidence and agreement of the risk
   rules. Without a key, or with it off, the rules alone decide.

   Every model is one of **NVIDIA's hosted models**, reached through one
   endpoint (`https://integrate.api.nvidia.com/v1`, `/chat/completions`)
   with one key. The panel has four roles, each briefed with its skills
   (`prompts/skill-*.md`, compiled into the binary):

   | Role | Checks | Skill briefs | Built-in models |
   |---|---|---|---|
   | Analysis (the team) | six seats, one per model, see below | TA Indicators, SMC Patterns, Risk Engine | Mistral Large 4, GLM 5.3 Flash, Gemma 3 12B, Nemotron 3.5 Lightning, Laguna XS 2.1, Gemma 3 4B |
   | Reasoning | argues against the setup first | SMC Patterns, TA Indicators, Risk Engine | Kimi K3.1 Pro, Nemotron 3 Ultra, Step 5 Preview |
   | Vision | also gets a picture of the chart and finds the 1-2-3 in it | Chart Reading, SMC Patterns, Risk Engine | Nemotron 3.5 Super VL, Nemotron 3 Nano Omni, Cosmos Reason 2 |
   | Compute | distances to stop and target in ATRs, reward/risk | TA Indicators, Risk Engine | MiniMax M3.1 Flash, Nemotron 3.5 Bolt |

   All fourteen ids were in NVIDIA's public model list in late November 2026.
   The Model Panel switches models on and off, tests each one, adds any other
   chat model from NVIDIA's list and restores the built-in panel.

   **The analysis team.** Each member checks the setup from one seat: the
   trend it trades with or against, the quality of the break (momentum), the
   stop and target against volatility, the 1-2-3 and the PD arrays
   (structure), the candles of the zone and the retest, the higher
   timeframe. Then, in the *team review*, every member sees its teammates'
   calls and makes its final call (told that agreement is not evidence); it
   can be switched off (*Model Panel → Analysis → Team review*).

   A model that fails, refuses or answers with something other than the JSON
   object is left out of that review and shown with the reason; its vote is
   never guessed. A model gets 180 seconds (*Time to answer*, 30 to 600) and
   is asked once more after a timeout, "too many requests" or a server
   error. At most six questions are with the service at a time.

   The rule-based analyst and the models' own buy/sell analysis of earlier
   versions no longer take part in trading.
5. **Risk Engine** (`core/risk.cpp`). Fixed arithmetic that no model takes
   part in, applied to the strategy's entry: minimum confidence and agreement
   (of the review; the rules alone count as one voter at 100), a stop on the right side and
   within range, minimum reward to risk, one position per market and a
   maximum in total, a daily loss limit, a drawdown limit that halts trading
   until you resume it, a cooldown after a loss, and the order size: a set
   share of the account lost if the stop is hit, never more than the balance
   covers, and never more than one position's share of the account (a third
   with three positions allowed), so a tight stop cannot put everything into
   one trade.
6. **Execution Engine** (`core/paper.cpp`). With *Auto trade* on, an SBT
   entry that passes the rules is placed on the paper account: a market order
   with fee and slippage, long or short, its stop and Level 3 as its levels. The stop and target are checked on every
   price update; a stop fills at the stop or, if price has gone past it, at
   the worse price; a target fills at its price; when one update covers both,
   the stop is assumed first. An SBT entry the other way closes the open
   position first. With *Auto trade* off (the default) the signal is shown and
   *Place paper trade* places it by hand.

Everything on screen is real: prices from the exchange, setups from the
strategy's rules, verdicts from the models that answered, statistics from the
trades the account made.

**On the screen.** The signal panel has an *SBT Model 1* card: the
direction (green for bullish, red for bearish), the status (armed and waiting
for the retest, price in the zone now, retest confirmed, entered, Level 3
reached, stopped, invalidated with the reason), Levels 1, 2 and 3, the zone,
any PD arrays found with it and the team's review. The AI chart draws the
current setup only: the Level 2 zone, the lines of Levels 1, 2 and 3 (3
dashed while it forms), the BOS and retest marks and the entry; past setups
are not drawn. The PD arrays in play (FVG blue, order blocks violet, demand
green, supply red, dashed) are drawn only with the PD filter on. The strategy's settings are under
*Settings → Strategy: SBT Model 1* (`[sbt]` in the file: `ai_confirm`,
`require_pd_array`, `zone_move_atr`, `gap_atr`, `gap_pause_candles`, `stop_buffer_atr`,
`pd_distance_atr`, `max_wait_candles`).

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
  **Forex only.** The markets are gold (XAUUSD), silver (XAGUSD) and EUR/USD
  to start with, and the other pairs with the US dollar can be added; the
  account is counted in dollars. Their candles come from **OANDA** (its v20
  REST API, with the trader's own token: *Settings → Prices: OANDA*; a free
  practice account is enough). FOREX.com, whose charts TradingView shows,
  has no public price service; OANDA's prices of the same markets differ
  from FOREX.com's by a few cents (gold) or a fraction of a pip. Without a
  token there are no prices, and the chart says where to add one. There is
  no crypto any more: a `kind=crypto` left in the settings file by an
  earlier version's switch is not followed (only the test suite, through
  `SHADOW_TRADING_MARKET_KIND`, still drives the crypto service its stand-in
  speaks), and OANDA's markets have a list of their own (`fx_watchlist`),
  so a list saved for earlier services starts afresh as gold, silver and
  the euro.
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
  The chart has two views, both TradingView's:
  - **TradingView · FOREX.com** (the default): TradingView's own chart widget
    with FOREX.com's prices (`FOREXCOM:XAUUSD`, `FOREXCOM:XAGUSD`,
    `FOREXCOM:EURUSD`), loaded from tradingview.com. The desk's markets are
    the widget's own watchlist (one click charts another); its symbol
    search adds any other chart TradingView has; its drawing tools,
    indicators and date ranges are TradingView's. Choosing a market on the
    desk, or another interval, charts it here too. It shows the broker's
    chart; the strategy's marks are on the strategy chart.
  - **Strategy chart**: TradingView's open-source chart library
    (Lightweight Charts 5.2.1, Apache-2.0, compiled into the application:
    nothing is fetched) drawing the candles SBT Model 1 reads, the setup in
    play (1, 2 with its zone, BOS, 3 and the entry) and the **past trades**:
    BUY or SELL where each entered, its take profit (green) and stop loss
    (red) as a short line from the entry, and TP or SL where one was
    reached. The past trades are the strategy's entries over every candle
    loaded, worked out in order as they would have been seen, and the paper
    account's own trades on the market (with the stop and target they had).
    An open position adds its Entry, SL and TP lines. Moving the chart to
    its left edge loads older candles. The chart carries TradingView's
    attribution link, as the library's licence asks.

  Both need the web view (`qml6-module-qtwebengine`). Without it the
  built-in chart shows the same candles, the setup and the past trades
  (price only: no EMA, RSI or Bollinger lines; the TradingView widget also
  starts without indicators). The built-in chart moves like any trading
  chart:

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
| `~/.config/shadow/trading/oanda.token` | the OANDA token, for the forex prices (mode 600; `SHADOW_TRADING_OANDA_TOKEN` takes its place) |
| `~/.local/share/shadow/trading/paper.json` | the paper account: balance, open positions, trade history, equity curve (mode 600). A file that cannot be read is moved to `paper.json.unreadable`, never overwritten |
| `~/.local/share/shadow/trading/desk.json` | the AI Desk: the conversation (the newest 400 messages), the analysis of each market discussed, the changes (mode 600). A file of another version is moved to `desk.json.unreadable` |

```ini
[market]
; forex (default). crypto only for the test suite's stand-in exchange.
; kind=forex
; Where the forex prices come from: oanda (default; needs the token) or
; kraken (no key; no silver, gold only as a token).
; forex_source=oanda
; The OANDA account the token belongs to: practice (api-fxpractice.oanda.com)
; or live (api-fxtrade.oanda.com). Only prices are read.
; oanda_account=practice
; OANDA's markets have their own list and selection: fx_watchlist
; (XAUUSD,XAGUSD,EURUSD), fx_symbol; fx_data_url replaces its address.
; (Kraken's: forex_watchlist, forex_symbol, forex_data_url.)
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

- Forex market data (OANDA): `GET <fx_data_url>/v3/instruments/<XAU_USD>/candles`
  with `granularity` (M1 … D), `price=M` (mid prices), `count` and, for older
  candles, `to`; the header `Authorization: Bearer <token>` and
  `Accept-Datetime-Format: UNIX`. The watchlist's prices are each market's
  two newest daily candles (the day starting at 17:00 New York). Asked for
  every 5 seconds; no stream.
- Forex market data (Kraken, `forex_source=kraken`): `GET <forex_data_url>/0/public/OHLC`,
  `/0/public/Ticker` and the stream `<forex_stream_url>/v2` (channels `trade`,
  `ohlc`, `ticker`, subscribed to once connected).
- Market data: `GET <data_url>/api/v3/klines` (with `endTime` for older
  candles), `/api/v3/ticker/24hr` and, for *Add a market*,
  `/api/v3/ticker/price` (QtNetwork); the live stream
  `<stream_url>/stream?streams=<market>@aggTrade/<market>@kline_<interval>/<each market>@miniTicker`
  (QtWebSockets). Gold and the euro are shown as they trade on that exchange:
  the PAX Gold token (`PAXGUSDT`) and `EURUSDT`, not spot XAU/USD or EUR/USD.
- Models: HTTPS requests to NVIDIA's endpoint (QtNetwork), one connection pool
  per request in flight so all models are asked at once.
- Packages: `shadow-ui`, `libqt6websockets6`; for the TradingView charts `qml6-module-qtwebengine`
  (recommended; without it the built-in chart is shown and the application
  says so).
- TradingView Lightweight Charts™ 5.2.1 (`web/lightweight-charts.standalone.production.js`,
  unmodified, Apache License 2.0, `web/LICENSE.lightweight-charts` and
  `web/NOTICE.lightweight-charts`): © TradingView, Inc., https://www.tradingview.com/.
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
| `tests/test_sbt.cpp` | SBT Model 1: a hand-drawn bullish chart and the same chart upside down for bearish; Level 1/2/3, the BOS, the zone, the retest, a close through the zone, a wick through it, no retest before Level 3, Level 3 following a new high, no BOS without a close, the strategy's own bullish and bearish charts with the default settings (and a testing candle that closes through the zone), external structure only (an internal lower high and a pause inside the leg are never Level 1 or 3), reward/risk and the optional PD filter, history not traded, expiry, no bearish candle at 2, FVG/order block/supply detection, old candles ignored, a 6000-candle random run (bounded memory, no entry twice) (13160 checks) | `tests/run-unit.sh sbt` |
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
- **The strategy's prices are OANDA's, the FOREX.com tab's are FOREX.com's.**
  The two brokers quote the same markets a few cents (gold, silver) or a
  fraction of a pip (pairs) apart, so a level can sit a hair differently on
  the two charts. The forex market is closed at weekends: no candles then.
  OANDA's prices are asked for every 5 seconds (no stream); a stop or
  target that is touched and left inside those seconds can be missed.
  **Not checked against OANDA's real service here** (the build environment
  cannot reach it and has no token): the requests and answers follow OANDA's
  v20 documentation and were exercised against a stand-in
  (`tests/fake_services.py`). If OANDA answers differently, the chart shows
  its error message.
- **Forex prices from Kraken (`forex_source=kraken`) are an exchange's, not a
  broker's.** They are the prices at which the currency pairs trade on Kraken: within a pip or so of the
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
  The TradingView · FOREX.com widget (tradingview.com) was checked only up
  to its settings (symbol, watchlist, symbol search, drawing tools) and its
  "could not be reached" fallback to the strategy chart: the build
  environment cannot reach tradingview.com; the strategy chart
  (compiled in) was rendered in a virtual display. The chart's mouse handling was driven in a
  virtual display (drag, wheel, pointer), not by hand on a real screen.
