// SPDX-License-Identifier: GPL-3.0-or-later
//
// The strategy chart: TradingView's Lightweight Charts library (shipped with
// the application, web/lightweight-charts.standalone.production.js) drawing
// the candles the strategy reads, the SBT Model 1 setup in play, and the past
// trades: where each one entered, its take profit and stop loss (a short line
// from the entry) and which of the two was reached. Nothing here fetches anything;
// the application hands the page its data:
//
//   shadow.apply({full, key, candles: [[time s, open, high, low, close], ...]})
//   shadow.overlay({intervalMs, sbt, history, position})
//   shadow.state() -> {key, first, last}   what the page has (times in s)
//
// The page asks for older candles by printing "shadow:older" to the console
// when the chart is moved to its left edge.
(function () {
    'use strict';
    var LC = window.LightweightCharts;
    var theme = window.SHADOW_THEME || {};
    var state = { key: '', first: 0, last: 0 };
    var chart = null, candles = null, markers = null;
    var drawn = [];       // the overlay's line series
    var lines = [];       // the open position's price lines
    var precision = 5;
    var askedOlder = false;

    function priceFormat() {
        return { type: 'price', precision: precision, minMove: Math.pow(10, -precision) };
    }

    function init() {
        chart = LC.createChart(document.getElementById('chart'), {
            autoSize: true,
            layout: {
                background: { type: 'solid', color: theme.background || '#0f1117' },
                textColor: theme.text || '#a9b1c2',
                fontFamily: theme.font || 'sans-serif',
                attributionLogo: true // TradingView's link, as the library's licence asks
            },
            grid: {
                vertLines: { color: theme.grid || '#1d2230' },
                horzLines: { color: theme.grid || '#1d2230' }
            },
            rightPriceScale: { borderColor: theme.grid || '#1d2230' },
            timeScale: { borderColor: theme.grid || '#1d2230', timeVisible: true, secondsVisible: false,
                         rightOffset: 8 },
            crosshair: { mode: LC.CrosshairMode.Normal }
        });
        candles = chart.addSeries(LC.CandlestickSeries, {
            upColor: theme.up || '#22c55e', downColor: theme.down || '#ef4444',
            wickUpColor: theme.up || '#22c55e', wickDownColor: theme.down || '#ef4444',
            borderVisible: false
        });
        markers = LC.createSeriesMarkers(candles, []);
        chart.timeScale().subscribeVisibleLogicalRangeChange(function (range) {
            if (!range || askedOlder || state.first === 0)
                return;
            if (range.from < 5) {
                askedOlder = true;
                console.log('shadow:older');
            }
        });
    }

    function seconds(ms) { return Math.floor(ms / 1000); }

    // A horizontal piece of line from t0 to t1 (seconds), on the candles' times.
    function segment(t0, t1, price, color, dashed) {
        if (!(price > 0) || !(t1 > t0))
            return;
        var s = chart.addSeries(LC.LineSeries, {
            color: color, lineWidth: 1,
            lineStyle: dashed ? LC.LineStyle.Dashed : LC.LineStyle.Solid,
            priceLineVisible: false, lastValueVisible: false, crosshairMarkerVisible: false,
            priceFormat: priceFormat()
        });
        s.setData([{ time: t0, value: price }, { time: t1, value: price }]);
        drawn.push(s);
    }

    window.shadow = {
        state: function () { return state; },

        apply: function (data) {
            if (!data || !data.candles)
                return;
            var rows = data.candles.map(function (r) {
                return { time: r[0], open: r[1], high: r[2], low: r[3], close: r[4] };
            });
            if (data.full) {
                if (rows.length > 0) {
                    var p = rows[rows.length - 1].close;
                    precision = p >= 1000 ? 2 : p >= 10 ? 3 : 5;
                    candles.applyOptions({ priceFormat: priceFormat() });
                }
                var newMarket = state.key !== data.key;
                var before = chart.timeScale().getVisibleLogicalRange();
                var added = state.first > 0 && rows.length > 0 && rows[0].time < state.first
                    ? rows.filter(function (r) { return r.time < state.first; }).length : 0;
                candles.setData(rows);
                if (newMarket && rows.length > 0) {
                    chart.timeScale().setVisibleLogicalRange({ from: Math.max(0, rows.length - 160),
                                                               to: rows.length + 6 });
                } else if (added > 0 && before) {
                    // Older candles came in front: keep the view where it was.
                    chart.timeScale().setVisibleLogicalRange({ from: before.from + added, to: before.to + added });
                }
                state.key = data.key;
                state.first = rows.length > 0 ? rows[0].time : 0;
                askedOlder = false;
            } else {
                rows.forEach(function (r) { candles.update(r); });
            }
            if (rows.length > 0)
                state.last = Math.max(state.last, rows[rows.length - 1].time);
        },

        overlay: function (o) {
            drawn.forEach(function (s) { chart.removeSeries(s); });
            drawn = [];
            lines.forEach(function (l) { candles.removePriceLine(l); });
            lines = [];
            if (!o || state.first === 0) {
                markers.setMarkers([]);
                return;
            }
            var step = Math.max(60, seconds(o.intervalMs || 900000));
            var first = state.first, last = state.last;
            var marks = [];
            var up = theme.up || '#22c55e', down = theme.down || '#ef4444';
            var bar = function (ms) { var t = seconds(ms); return t - (t % step); };
            // A past trade's take profit and stop loss: a short piece of line
            // from its entry (a dozen candles, or up to where it ended).
            var stub = function (t0, t1) { return Math.min(Math.max(t1, t0 + step), t0 + 12 * step, last); };
            var onChart = function (t) { return t >= first && t <= last; };
            var seen = {};
            var mark = function (m) {
                var k = m.time + '|' + m.text;
                if (!onChart(m.time) || seen[k])
                    return;
                seen[k] = true;
                marks.push(m);
            };

            // ---- the past trades: entry, take profit, stop loss -------------
            var history = o.history || {};
            var entries = (history.setups || []).slice(-60);
            var entryTimes = {};
            entries.forEach(function (h) {
                var bull = h.direction === 'bullish';
                var t0 = bar(h.entryTime);
                var t1 = h.result === 'open' ? last : bar(h.exitTime);
                entryTimes[t0] = true;
                mark({ time: t0, position: bull ? 'belowBar' : 'aboveBar', color: bull ? up : down,
                       shape: bull ? 'arrowUp' : 'arrowDown', text: bull ? 'BUY' : 'SELL' });
                segment(Math.max(t0, first), stub(t0, t1), h.target, up, true);
                segment(Math.max(t0, first), stub(t0, t1), h.stop, down, true);
                if (h.result === 'target')
                    mark({ time: t1, position: 'atPriceMiddle', price: h.target, color: up, shape: 'circle', text: 'TP' });
                else if (h.result === 'stop')
                    mark({ time: t1, position: 'atPriceMiddle', price: h.stop, color: down, shape: 'circle', text: 'SL' });
            });
            // The paper account's own trades, where the strategy's list has no
            // entry on the same candle.
            (history.trades || []).slice(-60).forEach(function (t) {
                var buy = t.side === 'buy';
                var t0 = bar(t.entryTime);
                if (entryTimes[t0] || entryTimes[t0 - step] || entryTimes[t0 + step])
                    return;
                var t1 = t.open ? last : bar(t.exitTime);
                mark({ time: t0, position: buy ? 'belowBar' : 'aboveBar', color: theme.accent || '#8b5cf6',
                       shape: buy ? 'arrowUp' : 'arrowDown', text: buy ? 'BUY' : 'SELL' });
                segment(Math.max(t0, first), stub(t0, t1), t.target, up, true);
                segment(Math.max(t0, first), stub(t0, t1), t.stop, down, true);
                if (!t.open) {
                    var tp = t.reason === 'take_profit', sl = t.reason === 'stop_loss';
                    mark({ time: t1, position: 'atPriceMiddle', price: t.exit, color: tp ? up : sl ? down : (theme.text || '#a9b1c2'),
                           shape: 'circle', text: tp ? 'TP' : sl ? 'SL' : 'Exit' });
                }
            });

            // ---- the setup in play ------------------------------------------
            var s = o.sbt && o.sbt.current;
            if (s && s.id !== undefined && s.status !== 'expired' && s.level1) {
                var bullish = s.direction === 'bullish';
                var tone = bullish ? up : down;
                var muted = theme.muted || '#6b7385';
                var l1 = bar(s.level1.time), l2 = bar(s.level2.time), bos = bar(s.bos.time);
                var end = s.entryTime > 0 ? bar(s.entryTime) : last;
                mark({ time: l1, position: bullish ? 'aboveBar' : 'belowBar', color: muted, shape: 'circle', text: '1' });
                mark({ time: l2, position: bullish ? 'belowBar' : 'aboveBar', color: muted, shape: 'circle', text: '2' });
                mark({ time: bos, position: bullish ? 'aboveBar' : 'belowBar', color: theme.accent || '#8b5cf6',
                       shape: 'square', text: 'BOS' });
                segment(Math.max(l1, first), Math.min(bos, last), s.level1.price, muted, true);
                if (s.zone) {
                    var z0 = Math.max(bar(s.zone.time), first);
                    segment(z0, Math.min(Math.max(end, z0 + step), last), s.zone.top, tone, false);
                    segment(z0, Math.min(Math.max(end, z0 + step), last), s.zone.bottom, tone, false);
                }
                if (s.level3 && s.level3.price) {
                    var l3 = bar(s.level3.time);
                    mark({ time: l3, position: bullish ? 'aboveBar' : 'belowBar', color: tone, shape: 'circle', text: '3' });
                    segment(Math.max(l3, first), last, s.target || s.level3.price, tone, !s.level3Confirmed);
                }
                if (s.entryTime > 0)
                    mark({ time: bar(s.entryTime), position: bullish ? 'belowBar' : 'aboveBar', color: tone,
                           shape: bullish ? 'arrowUp' : 'arrowDown', text: bullish ? 'BUY' : 'SELL' });
            }

            // ---- the open position: its own lines ---------------------------
            var p = o.position;
            if (p && p.id !== undefined) {
                lines.push(candles.createPriceLine({ price: p.entry, color: theme.text || '#e5e7eb', lineWidth: 1,
                    lineStyle: LC.LineStyle.Solid, axisLabelVisible: true, title: 'Entry' }));
                if (p.stop > 0)
                    lines.push(candles.createPriceLine({ price: p.stop, color: down, lineWidth: 1,
                        lineStyle: LC.LineStyle.Dashed, axisLabelVisible: true, title: 'SL' }));
                if (p.takeProfit > 0)
                    lines.push(candles.createPriceLine({ price: p.takeProfit, color: up, lineWidth: 1,
                        lineStyle: LC.LineStyle.Dashed, axisLabelVisible: true, title: 'TP' }));
            }

            marks.sort(function (a, b) { return a.time - b.time; });
            markers.setMarkers(marks);
        }
    };

    init();
})();
