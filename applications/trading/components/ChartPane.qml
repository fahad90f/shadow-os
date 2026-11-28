// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.trading
import "Format.js" as Format

// The left side of the live page: the chart of the selected market. Two
// views, both TradingView's (where the web view is installed):
//  - the strategy chart: TradingView's Lightweight Charts library drawing the
//    candles the strategy reads, the SBT setup in play and the past trades
//    (entry, take profit, stop loss);
//  - FOREX.com's own chart of the market on TradingView (FOREXCOM:XAUUSD …).
// Without the web view, the built-in chart draws the same candles, setup and
// past trades.
GlowCard {
    id: root

    // The shapes the chart draws for SBT Model 1. Bullish in the "up" colour,
    // bearish in the "down" colour; PD arrays dashed, in their own colours.
    // The setup being watched or traded now, drawn as in the strategy's own
    // chart: 1, 2 with its zone, the BOS, 3 and the entry. Past setups are
    // not drawn. PD arrays only when the extra PD filter is switched on (they
    // are not one of the 7 rules).
    // The past trades: where each entered (BUY or SELL), its take profit and
    // stop loss (a short line from the entry), and which of the two was reached.
    function historyShapes(history) {
        var shapes = []
        var span = 12 * trading.intervalMs
        var stubEnd = function (start, end) { return end > 0 ? Math.min(end, start + span) : start + span }
        var list = (history.setups || []).slice(-60)
        var entries = {}
        for (var i = 0; i < list.length; i++) {
            var h = list[i]
            var bull = h.direction === "bullish"
            var end = stubEnd(h.entryTime, h.result === "open" ? 0 : h.exitTime)
            entries[h.entryTime] = true
            shapes.push({ kind: "mark", price: h.entry, from: h.entryTime, color: bull ? Theme.success : Theme.danger,
                          label: bull ? qsTr("BUY") : qsTr("SELL"), below: !bull })
            shapes.push({ kind: "line", price: h.target, from: h.entryTime, to: end, color: Theme.alpha(Theme.success, 0.75), dashed: true, label: "" })
            shapes.push({ kind: "line", price: h.stop, from: h.entryTime, to: end, color: Theme.alpha(Theme.danger, 0.75), dashed: true, label: "" })
            if (h.result === "target")
                shapes.push({ kind: "mark", price: h.target, from: h.exitTime, color: Theme.success, label: qsTr("TP") })
            else if (h.result === "stop")
                shapes.push({ kind: "mark", price: h.stop, from: h.exitTime, color: Theme.danger, label: qsTr("SL"), below: true })
        }
        // The paper account's own trades the strategy's list does not have.
        var step = trading.intervalMs
        var trades = (history.trades || []).slice(-60)
        for (var j = 0; j < trades.length; j++) {
            var t = trades[j]
            var at = t.entryTime - (t.entryTime % step)
            if (entries[at] || entries[at - step] || entries[at + step])
                continue
            var buy = t.side === "buy"
            var stop = stubEnd(t.entryTime, t.open ? 0 : t.exitTime)
            shapes.push({ kind: "mark", price: t.entry, from: t.entryTime, color: Theme.accent,
                          label: buy ? qsTr("BUY") : qsTr("SELL"), below: !buy })
            if (t.target > 0)
                shapes.push({ kind: "line", price: t.target, from: t.entryTime, to: stop, color: Theme.alpha(Theme.success, 0.75), dashed: true, label: "" })
            if (t.stop > 0)
                shapes.push({ kind: "line", price: t.stop, from: t.entryTime, to: stop, color: Theme.alpha(Theme.danger, 0.75), dashed: true, label: "" })
            if (!t.open)
                shapes.push({ kind: "mark", price: t.exit, from: t.exitTime,
                              color: t.reason === "take_profit" ? Theme.success : t.reason === "stop_loss" ? Theme.danger : Theme.textSecondary,
                              label: t.reason === "take_profit" ? qsTr("TP") : t.reason === "stop_loss" ? qsTr("SL") : qsTr("Exit"),
                              below: t.reason === "stop_loss" })
        }
        return shapes
    }

    function sbtOverlay(sbt, showArrays) {
        var shapes = []
        var arrays = showArrays ? (sbt.pdArrays || []) : []
        for (var i = Math.max(0, arrays.length - 10); i < arrays.length; i++) {
            var a = arrays[i]
            var color = a.kind === "fvg" ? Theme.accentBlue : a.kind === "order_block" ? Theme.accentViolet
                      : a.kind === "demand" ? Theme.success : Theme.danger
            var name = a.kind === "fvg" ? qsTr("FVG") : a.kind === "order_block" ? qsTr("OB")
                     : a.kind === "demand" ? qsTr("Demand") : qsTr("Supply")
            shapes.push({ kind: "zone", top: a.top, bottom: a.bottom, from: a.time, to: 0, color: Theme.alpha(color, 0.7),
                          label: name, dashed: true })
        }
        var s = sbt.current
        if (!s || s.id === undefined || s.status === "expired")
            return shapes
        var tone = s.direction === "bullish" ? Theme.success : Theme.danger
        var end = s.entryTime > 0 ? s.entryTime : (s.retest && s.retest.time ? s.retest.time : 0)
        shapes.push({ kind: "zone", top: s.zone.top, bottom: s.zone.bottom, from: s.zone.time, to: end, color: tone,
                      label: qsTr("Level 2 zone") })
        shapes.push({ kind: "line", price: s.level1.price, from: s.level1.time, to: s.bos.time, color: Theme.textSecondary,
                      label: "1", dashed: true })
        shapes.push({ kind: "line", price: s.level2.price, from: s.level2.time, to: end, color: Theme.textSecondary,
                      label: "2", dashed: true })
        if (s.level3 && s.level3.price)
            shapes.push({ kind: "line", price: s.target, from: s.level3.time, to: 0, color: tone,
                          label: s.level3Confirmed ? qsTr("3 · target") : qsTr("3 (forming)"), dashed: !s.level3Confirmed })
        shapes.push({ kind: "mark", price: s.level1.price, from: s.bos.time, color: Theme.accent, label: qsTr("BOS") })
        // The retest candle's close; the entry is the position's own line once it is open.
        if (s.retest && s.retest.time)
            shapes.push({ kind: "mark", price: s.retest.price, from: s.retest.time, color: tone, label: "" })
        if (s.entryTime > 0 && !root.holding)
            shapes.push({ kind: "mark", price: s.entry, from: s.entryTime, color: Theme.textPrimary,
                          label: s.direction === "bullish" ? qsTr("BUY") : qsTr("SELL") })
        return shapes
    }

    property Labels labels
    // 0 = FOREX.com's chart on TradingView (the TradingView widget, the default),
    // 1 = the strategy chart. Without the web view there is only the built-in chart.
    property int view: 0
    readonly property bool widgetShown: trading.webViewAvailable && view === 0
    readonly property bool strategyShown: !widgetShown
    // TradingView's site was tried and could not be reached; the strategy chart took its place.
    property bool tradingViewDown: false
    // The built-in chart: without the web view.
    readonly property bool builtIn: !trading.webViewAvailable
    readonly property var quote: trading.quote
    readonly property var signalData: trading.signal
    readonly property bool showLevels: (signalData.state || "") === "ready" && signalData.action !== "hold"
        && signalData.symbol === trading.symbol
    // An open position on this market: its lines stay on the chart for as long
    // as it is open, whatever the newest analysis says.
    readonly property var position: trading.position
    readonly property bool holding: position.id !== undefined

    padding: Theme.spaceSm + 2
    implicitWidth: 480
    implicitHeight: 320

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.spaceSm

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceSm

            NxSegmented {
                Accessible.name: qsTr("Candle interval")
                model: trading.intervals
                currentIndex: Math.max(0, trading.intervals.indexOf(trading.interval))
                onActivated: (index) => trading.interval = trading.intervals[index]
            }
            NxSegmented {
                visible: trading.webViewAvailable
                Accessible.name: qsTr("Chart view")
                model: [trading.tradingViewSymbol.indexOf("FOREXCOM:") === 0 ? qsTr("TradingView · FOREX.com") : qsTr("TradingView"),
                        qsTr("Strategy chart")]
                currentIndex: root.view
                onActivated: (index) => {
                    if (index === 0)
                        root.tradingViewDown = false   // try again
                    root.view = index
                }
            }
            NxBadge {
                // How fresh the prices are: pushed by the data service as trades
                // happen, or asked for every few seconds.
                visible: root.quote.close !== undefined
                kind: trading.streaming ? "success" : "neutral"
                text: trading.streaming ? qsTr("Real time") : qsTr("Every %1 s").arg(trading.pollSeconds)
                Accessible.name: trading.streaming ? qsTr("Prices arrive in real time")
                    : qsTr("Prices are fetched every %1 seconds").arg(trading.pollSeconds)
            }
            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignRight
                // The candle under the pointer; without one, the newest candle.
                readonly property bool pointed: chart.visible && chart.hovered.close !== undefined
                readonly property var shown: pointed ? chart.hovered : root.quote
                // All four prices where there is room, else the close and the change.
                readonly property bool roomy: root.width >= 900
                text: shown.close === undefined ? ""
                    : (pointed ? Qt.formatDateTime(new Date(shown.time), "d MMM HH:mm") + "   " : "")
                    + (roomy ? qsTr("O %1  H %2  L %3  C %4  %5")
                        .arg(Format.price(shown.open)).arg(Format.price(shown.high))
                        .arg(Format.price(shown.low)).arg(Format.price(shown.close))
                        .arg(Format.signedPct(shown.changePct))
                    : qsTr("%1  %2").arg(Format.price(shown.close)).arg(Format.signedPct(shown.changePct)))
                color: shown.change >= 0 ? Theme.success : Theme.danger
                font.pixelSize: Theme.fontSizeSmall
                elide: Text.ElideLeft
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            // ---- the strategy chart (TradingView's library) -------------------
            Loader {
                id: strategyLoader
                anchors.fill: parent
                active: trading.webViewAvailable && root.view === 1
                visible: active && root.quote.close !== undefined
                source: active ? "StrategyChartPane.qml" : ""
                onLoaded: item.colours = {
                    background: Theme.bgSurface, text: Theme.textMuted, grid: Theme.borderSubtle,
                    up: Theme.success, down: Theme.danger, accent: Theme.accent, muted: Theme.textSecondary,
                    font: Theme.fontUi
                }
            }

            // ---- FOREX.com on TradingView ------------------------------------
            Loader {
                id: webLoader
                anchors.fill: parent
                active: root.widgetShown
                visible: active
                source: active ? "TradingViewPane.qml" : ""
                readonly property string offlineText: qsTr("TradingView could not be loaded. Check the internet connection, or use the strategy chart.")
                onLoaded: item.page = trading.tradingViewPage(Theme.dark, offlineText)
                // Rebuild the page when the market, the interval or the list of
                // markets (the widget's own watchlist) changes; not for prices.
                function refresh() {
                    if (!webLoader.item)
                        return
                    var page = trading.tradingViewPage(Theme.dark, webLoader.offlineText)
                    if (page !== webLoader.item.page)
                        webLoader.item.page = page
                }
                Connections {
                    target: trading
                    function onSelectionChanged() { webLoader.refresh() }
                    function onMarketChanged() { webLoader.refresh() }
                }
                // No route to TradingView: show the strategy chart instead of an empty pane.
                Connections {
                    target: webLoader.item
                    ignoreUnknownSignals: true
                    function onUnreachableChanged() {
                        if (webLoader.item && webLoader.item.unreachable) {
                            root.tradingViewDown = true
                            root.view = 1
                        }
                    }
                }
            }

            // ---- the built-in chart (no web view) -----------------------------
            ChartItem {
                id: chart
                anchors.fill: parent
                visible: root.builtIn && root.quote.close !== undefined
                source: trading
                entry: root.holding ? root.position.entry : root.showLevels ? root.signalData.entry : 0
                stop: root.holding ? root.position.stop : root.showLevels ? root.signalData.stop : 0
                target: root.holding ? root.position.takeProfit : root.showLevels ? root.signalData.takeProfit : 0
                entryLabel: root.holding
                    ? qsTr("%1  %2").arg(root.labels.action(root.position.side)).arg(Format.signedMoney(root.position.pnl))
                    : ""
                backgroundColor: Theme.bgSurface
                gridColor: Theme.borderSubtle
                textColor: Theme.textMuted
                upColor: Theme.success
                downColor: Theme.danger
                // Price only: no EMA or Bollinger lines on the window's chart.
                fastColor: "transparent"
                slowColor: "transparent"
                bandColor: "transparent"
                entryColor: Theme.textPrimary
                // SBT Model 1: the setup's zone, levels and marks, and the PD arrays in play.
                overlay: root.historyShapes(trading.sbtHistory).concat(root.sbtOverlay(trading.sbt, trading.sbtSettings.require_pd_array === true))
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeCaption
                Accessible.role: Accessible.Graphic
                Accessible.name: qsTr("Candlestick chart of %1, %2 candles").arg(trading.symbolName).arg(trading.interval)
                Accessible.description: qsTr("Drag or use the arrow keys to move through time, scroll or press plus and minus to zoom, Home for the newest candles.")
            }
            Rectangle {
                anchors.fill: chart
                visible: chart.visible && chart.activeFocus
                color: "transparent"
                radius: Theme.radiusXs
                border.width: 1
                border.color: Theme.accent
            }
            // Where the view is in time, when it is not at the newest candles.
            Rectangle {
                anchors.fill: pastHint
                anchors.margins: -Theme.spaceXs
                visible: pastHint.visible
                radius: Theme.radiusXs
                color: Theme.alpha(Theme.bgSurface, 0.9)
            }
            Label {
                id: pastHint
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                anchors.topMargin: Theme.spaceSm + Theme.spaceXs
                visible: chart.visible && (trading.loadingOlder || (!chart.live && !trading.hasOlder && chart.atStart))
                text: trading.loadingOlder ? qsTr("Loading older candles…") : qsTr("This is where %1 begins").arg(trading.symbolName)
                color: Theme.textMuted
                font.pixelSize: Theme.fontSizeCaption
            }
            // Zoom, and back to the newest candles.
            RowLayout {
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.rightMargin: 84
                anchors.bottomMargin: 34
                visible: chart.visible
                spacing: Theme.spaceXs
                NxButton {
                    visible: !chart.live
                    kind: "primary"
                    iconName: "chevron-right"
                    text: qsTr("Live")
                    Accessible.name: qsTr("Back to the newest candles")
                    onClicked: chart.goLive()
                }
                Rectangle {
                    Layout.preferredWidth: zoomRow.implicitWidth + 4
                    Layout.preferredHeight: zoomRow.implicitHeight + 4
                    radius: Theme.radiusSm
                    color: Theme.alpha(Theme.bgRaised, 0.92)
                    border.width: 1
                    border.color: Theme.borderSubtle
                    RowLayout {
                        id: zoomRow
                        anchors.centerIn: parent
                        spacing: 0
                        NxIconButton {
                            iconName: "zoom-out"
                            iconSize: Theme.iconSmall
                            tooltip: qsTr("Show more candles")
                            onClicked: chart.zoom(1.25)
                        }
                        NxIconButton {
                            iconName: "zoom-in"
                            iconSize: Theme.iconSmall
                            tooltip: qsTr("Show fewer candles")
                            onClicked: chart.zoom(0.8)
                        }
                    }
                }
            }

            NxEmptyState {
                anchors.centerIn: parent
                width: Math.min(parent.width - Theme.spaceXl * 2, 460)
                visible: root.strategyShown && root.quote.close === undefined
                icon: trading.dataState === "error" ? "alert-triangle" : "activity"
                tone: trading.dataState === "error" ? "warning" : "neutral"
                title: trading.dataState === "error" ? qsTr("No market data") : qsTr("Loading %1…").arg(trading.symbolName)
                message: trading.dataState === "error" ? trading.dataError
                    : qsTr("Fetching candles from %1.").arg(trading.dataSource)
            }
        }

        Label {
            Layout.fillWidth: true
            visible: true
            text: !trading.webViewAvailable
                ? qsTr("The TradingView charts need the package qml6-module-qtwebengine. Showing the built-in chart of the same candles the strategy reads, with its setup and past trades.")
                : root.tradingViewDown
                ? qsTr("TradingView's site could not be reached (no internet connection, or the site is blocked). Showing the strategy chart. Choose TradingView above to try again.")
                : root.widgetShown
                ? qsTr("TradingView's chart of %1 with FOREX.com's prices. Its watchlist (right) holds your markets; its symbol search (top left) adds any other chart, and its drawing tools are on the left. SBT Model 1's setups and past trades are on the Strategy chart.").arg(trading.tradingViewSymbol)
                : qsTr("Strategy chart: the candles SBT Model 1 reads (%1), its setup in play and its past trades: BUY or SELL where each entered, its take profit (green) and stop loss (red), TP or SL where one was reached. Charting by TradingView.").arg(trading.dataSource)
            color: Theme.textMuted
            font.pixelSize: Theme.fontSizeCaption
            wrapMode: Text.WordWrap
        }
    }
}
