# Skill: TA Indicators

How to read the `indicators` block of the snapshot.

- **Trend.** `trend` is "bullish" when price > EMA20 > EMA50 and "bearish" when price < EMA20 < EMA50; "mixed" means no trend to lean on. EMA200 (when present) is the long-term side: trades against it need more evidence.
- **Momentum.** RSI14 above 70 is stretched up, below 30 stretched down. In a strong trend RSI stays stretched; on its own it is a reason for caution, not for a trade against the trend.
- **MACD.** A histogram that is positive and rising supports longs; negative and falling supports shorts. A histogram shrinking toward zero says the move is losing force.
- **Volatility.** `atr14` is the size of a normal candle's range. Distances (to the EMAs, to a stop) mean little as prices: think of them in ATRs. `bollinger_position` near 1 is the upper band, near 0 the lower; a narrow `bollinger_width_pct` after a wide one is compression before a move, direction unknown.
- **Volume.** `volume_ratio` above 1.5 confirms the last candle's direction; a breakout on low volume is suspect.

These are measurements of past candles. One reading is never a trade; look for several that agree.
