#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Print a paper-account file with a few made-up closed trades, for
screenshots (tests/with-fakes.sh FAKE_HISTORY=1). The numbers are consistent:
the balance is the starting balance plus the trades' net results."""
import json
import time

now = int(time.time() * 1000)
hour = 3_600_000
start = 10_000.0
fee = 0.001
plan = [  # symbol, long, quantity, entry, exit, hours ago opened, hours held, reason, lead
    ("BTCUSDT", True, 0.052, 91250.0, 92110.0, 30, 3.0, "take_profit", "anthropic"),
    ("ETHUSDT", False, 1.4, 3262.0, 3291.5, 26, 1.5, "stop_loss", "openai"),
    ("SOLUSDT", True, 31.0, 138.20, 141.65, 21, 4.0, "take_profit", "gemini"),
    ("BTCUSDT", True, 0.048, 91880.0, 91420.0, 15, 2.0, "stop_loss", "rules"),
    ("ETHUSDT", True, 1.6, 3188.0, 3247.8, 9, 5.0, "take_profit", "nvidia"),
    ("PAXGUSDT", True, 1.1, 4041.0, 4076.4, 4, 2.5, "take_profit", "anthropic"),
]
balance = start
trades = []
curve = [[now - 32 * hour, start]]
for number, (symbol, is_long, quantity, entry, exit_, opened, held, reason, lead) in enumerate(plan, 1):
    fees = (entry + exit_) * quantity * fee
    gross = (exit_ - entry if is_long else entry - exit_) * quantity
    pnl = gross - fees
    balance += pnl
    closed_at = now - int((opened - held) * hour)
    trades.append({
        "id": number, "symbol": symbol, "long": is_long, "quantity": quantity, "entry": entry, "exit": exit_,
        "opened_at": now - opened * hour, "closed_at": closed_at, "fees": round(fees, 6), "pnl": round(pnl, 6),
        "pnl_pct": round(pnl / (entry * quantity) * 100, 6), "reason": reason, "confidence": 68.0, "lead": lead,
        "reasoning": "Made-up trade for a screenshot.",
    })
    curve.append([closed_at, round(balance, 6)])
print(json.dumps({
    "version": 1, "settings": {"starting_balance": start}, "balance": round(balance, 6),
    "peak_equity": round(max(point[1] for point in curve), 6), "day_start_equity": round(balance, 6),
    "day": now // 86_400_000, "max_drawdown_pct": 0.4, "next_id": len(plan) + 1, "halted": False,
    "positions": [], "trades": trades, "equity_curve": curve,
}, indent=1))
