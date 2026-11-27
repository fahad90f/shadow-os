// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.trading
import "Format.js" as Format

// The left side of the live page: the chart of the selected market. Two
// views: TradingView's own chart (where the web view is installed) and the
// AI chart, drawn from the candles the analysts read, with the trade levels.
GlowCard {
    id: root

    // The shapes the chart draws for SBT Model 1. Bullish in the "up" colour,
    // bearish in the "down" colour; PD arrays dashed, in their own colours.
    // Show the strategy's past entries and the paper trades on the chart.
    property bool showHistory: true

    // The strategy over the loaded history: each entry (BUY/SELL where it
    // entered), how it ended (Level 3 or the stop, in R), the path between;
    // and the paper account's own trades on this market.
    function historyShapes(history) {
        var shapes = []
        var list = history.setups || []
        for (var i = 0; i < list.length; i++) {
            var h = list[i]
            var bull = h.direction === "bullish"
            var tone = bull ? Theme.success : Theme.danger
            shapes.push({ kind: "mark", price: h.entry, from: h.entryTime, color: tone,
                          label: bull ? qsTr("BUY") : qsTr("SELL") })
            if (h.result === "open") {
                shapes.push({ kind: "line", price: h.target, from: h.entryTime, to: 0, color: Theme.alpha(Theme.success, 0.6), dashed: true, label: "" })
                shapes.push({ kind: "line", price: h.stop, from: h.entryTime, to: 0, color: Theme.alpha(Theme.danger, 0.6), dashed: true, label: "" })
                continue
            }
            var won = h.result === "target"
            shapes.push({ kind: "line", price: h.entry, from: h.entryTime, to: h.exitTime,
                          color: Theme.alpha(Theme.textSecondary, 0.7), dashed: true, label: "" })
            shapes.push({ kind: "mark", price: h.exit, from: h.exitTime, color: won ? Theme.success : Theme.danger,
                          label: won ? qsTr("TP +%1R").arg(Number(h.r).toFixed(1)) : qsTr("SL −1R"), below: !won })
        }
        var trades = history.trades || []
        for (var j = 0; j < trades.length; j++) {
            var t = trades[j]
            // The paper trades' labels go under their marks, clear of the strategy's.
            shapes.push({ kind: "mark", price: t.entry, from: t.entryTime, color: Theme.accent, below: true,
                          label: qsTr("Paper %1").arg(t.side === "buy" ? qsTr("BUY") : qsTr("SELL")) })
            if (!t.open)
                shapes.push({ kind: "mark", price: t.exit, from: t.exitTime, color: Theme.accent, below: true,
                              label: qsTr("exit %1").arg(Format.signedMoney(t.pnl)) })
        }
        return shapes
    }

    function sbtOverlay(sbt, history, showHistory) {
        var shapes = []
        if (showHistory)
            shapes = shapes.concat(historyShapes(history))
        var arrays = sbt.pdArrays || []
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
    // 0 = TradingView, 1 = AI chart. Without the web view there is only the AI chart.
    property int view: trading.webViewAvailable ? 0 : 1
    // TradingView was tried and could not be reached; the AI chart took its place.
    property bool tradingViewDown: false
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
                model: [qsTr("TradingView"), qsTr("AI chart")]
                currentIndex: root.view
                onActivated: (index) => {
                    if (index === 0)
                        root.tradingViewDown = false   // try again
                    root.view = index
                }
            }
            // Where SBT Model 1 entered in the past and how it ended, and the paper trades.
            NxButton {
                visible: root.view === 1
                kind: root.showHistory ? "secondary" : "ghost"
                iconName: "clock"
                text: qsTr("SBT history")
                onClicked: root.showHistory = !root.showHistory
                Accessible.name: root.showHistory ? qsTr("Hide the SBT history on the chart") : qsTr("Show the SBT history on the chart")
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
                readonly property bool pointed: root.view === 1 && chart.hovered.close !== undefined
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

            // ---- TradingView -------------------------------------------------
            Loader {
                id: webLoader
                anchors.fill: parent
                active: trading.webViewAvailable && root.view === 0
                visible: active
                source: active ? "TradingViewPane.qml" : ""
                readonly property string offlineText: qsTr("TradingView could not be loaded. Check the internet connection, or use the AI chart.")
                onLoaded: item.page = trading.tradingViewPage(Theme.dark, offlineText)
                // Rebuild the page when the market or the interval changes.
                Connections {
                    target: trading
                    function onSelectionChanged() {
                        if (webLoader.item)
                            webLoader.item.page = trading.tradingViewPage(Theme.dark, webLoader.offlineText)
                    }
                }
                // No route to TradingView: show the AI chart instead of an empty pane.
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

            // ---- AI chart ----------------------------------------------------
            ChartItem {
                id: chart
                anchors.fill: parent
                visible: root.view === 1 && root.quote.close !== undefined
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
                fastColor: Theme.accentAmber
                slowColor: Theme.accentViolet
                bandColor: Theme.alpha(Theme.accentBlue, 0.7)
                entryColor: Theme.textPrimary
                // SBT Model 1: the setup's zone, levels and marks, and the PD arrays in play.
                overlay: root.sbtOverlay(trading.sbt, trading.sbtHistory, root.showHistory)
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeCaption
                Accessible.role: Accessible.Graphic
                Accessible.name: qsTr("Candlestick chart of %1, %2 candles").arg(trading.symbolName).arg(trading.interval)
                Accessible.description: qsTr("Drag or use the arrow keys to move through time, scroll or press plus and minus to zoom, Home for the newest candles.")
            }
            // What the strategy's history adds up to, on the candles loaded.
            Label {
                readonly property var stats: trading.sbtHistory.stats || ({})
                anchors.left: chart.left
                anchors.top: chart.top
                anchors.leftMargin: 10
                anchors.topMargin: 26
                visible: chart.visible && root.showHistory && stats.entries !== undefined
                text: stats.entries === 0
                    ? qsTr("SBT history: no entry on the %1 candles loaded").arg(trading.sbtHistory.candles)
                    : qsTr("SBT history on %1 candles: %2 · %3 target · %4 stop%5 · win %6 % · %7R  (by the rules, not traded)")
                          .arg(trading.sbtHistory.candles)
                          .arg(stats.entries === 1 ? qsTr("1 entry") : qsTr("%1 entries").arg(stats.entries))
                          .arg(stats.targets).arg(stats.stops)
                          .arg(stats.open > 0 ? qsTr(" · %1 open").arg(stats.open) : "")
                          .arg(Number(stats.winRate).toFixed(0))
                          .arg((stats.totalR >= 0 ? "+" : "") + Number(stats.totalR).toFixed(1))
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSizeCaption
                background: Rectangle { color: Theme.alpha(Theme.bgSurface, 0.8); radius: 3 }
                padding: 3
            }
            Rectangle {
                anchors.fill: chart
                visible: chart.visible && chart.activeFocus
                color: "transparent"
                radius: Theme.radiusXs
                border.width: 1
                border.color: Theme.accent
            }
            // The legend sits on a plate so a level line behind it stays readable.
            Rectangle {
                anchors.fill: legend
                anchors.margins: -Theme.spaceXs
                visible: legend.visible
                radius: Theme.radiusXs
                color: Theme.alpha(Theme.bgSurface, 0.9)
            }
            RowLayout {
                id: legend
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.margins: Theme.spaceSm + Theme.spaceXs
                visible: chart.visible
                spacing: Theme.spaceMd
                Repeater {
                    model: [
                        { text: qsTr("EMA 20"), tone: Theme.accentAmber },
                        { text: qsTr("EMA 50"), tone: Theme.accentViolet },
                        { text: qsTr("Bollinger 20, 2"), tone: Theme.accentBlue },
                    ]
                    RowLayout {
                        required property var modelData
                        spacing: Theme.spaceXs
                        Rectangle { width: 10; height: 2; color: modelData.tone }
                        Label {
                            text: modelData.text
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSizeCaption
                        }
                    }
                }
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
                visible: root.view === 1 && root.quote.close === undefined
                icon: trading.dataState === "error" ? "alert-triangle" : "activity"
                tone: trading.dataState === "error" ? "warning" : "neutral"
                title: trading.dataState === "error" ? qsTr("No market data") : qsTr("Loading %1…").arg(trading.symbolName)
                message: trading.dataState === "error" ? trading.dataError
                    : qsTr("Fetching candles from %1.").arg(trading.dataSource)
            }
        }

        Label {
            Layout.fillWidth: true
            visible: !trading.webViewAvailable || root.tradingViewDown
            text: root.tradingViewDown
                ? qsTr("TradingView could not be reached (no internet connection, or the site is blocked). Showing the AI chart, drawn from the same candles the analysts read. Choose TradingView above to try again.")
                : qsTr("The TradingView view needs the package qml6-module-qtwebengine. Showing the AI chart, drawn from the same candles the analysts read.")
            color: Theme.textMuted
            font.pixelSize: Theme.fontSizeCaption
            wrapMode: Text.WordWrap
        }
    }
}
