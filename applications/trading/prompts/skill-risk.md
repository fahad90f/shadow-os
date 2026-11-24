# Skill: Risk Engine

How to set the levels of a trade. An order is placed at the snapshot's `price`.

- **Stop first.** Put `stop_loss` where the idea is proven wrong: beyond the swing, order block or range edge the trade leans on, usually 1 to 2 ATR from the price. A stop inside normal noise (under half an ATR) gets hit by chance.
- **Then the target.** `take_profit` must be at least `min_reward_risk` times as far from the price as the stop, and it must be reachable: not beyond the range edge, a strong opposing block or an unfilled gap in the way. If no reachable target gives that ratio, the answer is hold.
- **Sides.** For a buy: stop below the price, target above. For a sell: stop above, target below.
- **An open position** in the snapshot means the account is already in this market: say hold unless the evidence has clearly turned against it.

The application sizes the order and enforces its own limits afterwards; a signal that breaks these rules is simply not traded.
