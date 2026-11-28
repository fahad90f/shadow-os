#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""fetch_klines.py SYMBOL INTERVAL PAGES OUT.csv

Closed candles from Binance's public market data (no account, no key), newest
PAGES*1000 of them, oldest first, as sbt_backtest reads them:
time_ms,open,high,low,close,volume. For a look at the strategy's rules on real
candles when no forex feed is at hand: PAXGUSDT (a gold token) and EURUSDT
trade round the clock on an exchange and are not FOREX.com's XAUUSD or EURUSD.
"""
import json
import sys
import time
import urllib.request

symbol, interval, pages, out = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
rows, end = {}, None
for _ in range(pages):
    url = f"https://data-api.binance.vision/api/v3/klines?symbol={symbol}&interval={interval}&limit=1000"
    if end is not None:
        url += f"&endTime={end}"
    for attempt in range(4):
        try:
            with urllib.request.urlopen(url, timeout=30) as reply:
                data = json.load(reply)
            break
        except OSError:
            time.sleep(1 + attempt * 2)
    else:
        sys.exit(f"could not fetch {url}")
    if not data:
        break
    for k in data:
        rows[k[0]] = k
    end = data[0][0] - 1
    if len(data) < 1000:
        break
    time.sleep(0.15)
now = int(time.time() * 1000)
closed = [k for _, k in sorted(rows.items()) if k[6] < now]
with open(out, "w", encoding="utf-8") as f:
    for k in closed:
        f.write(",".join(str(x) for x in k[:6]) + "\n")
print(f"{symbol} {interval}: {len(closed)} candles")
