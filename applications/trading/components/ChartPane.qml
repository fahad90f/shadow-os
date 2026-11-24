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

    property Labels labels
    // 0 = TradingView, 1 = AI chart. Without the web view there is only the AI chart.
    property int view: trading.webViewAvailable ? 0 : 1
    // TradingView was tried and could not be reached; the AI chart took its place.
    property bool tradingViewDown: false
    readonly property var quote: trading.quote
    readonly property var signalData: trading.signal
    readonly property bool showLevels: (signalData.state || "") === "ready" && signalData.action !== "hold"
        && signalData.symbol === trading.symbol

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
            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignRight
                // All four prices where there is room, else the close and the change.
                readonly property bool roomy: root.width >= 900
                text: root.quote.close === undefined ? ""
                    : roomy ? qsTr("O %1  H %2  L %3  C %4  %5")
                        .arg(Format.price(root.quote.open)).arg(Format.price(root.quote.high))
                        .arg(Format.price(root.quote.low)).arg(Format.price(root.quote.close))
                        .arg(Format.signedPct(root.quote.changePct))
                    : qsTr("%1  %2").arg(Format.price(root.quote.close)).arg(Format.signedPct(root.quote.changePct))
                color: root.quote.change >= 0 ? Theme.success : Theme.danger
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
                entry: root.showLevels ? root.signalData.entry : 0
                stop: root.showLevels ? root.signalData.stop : 0
                target: root.showLevels ? root.signalData.takeProfit : 0
                backgroundColor: Theme.bgSurface
                gridColor: Theme.borderSubtle
                textColor: Theme.textMuted
                upColor: Theme.success
                downColor: Theme.danger
                fastColor: Theme.accentAmber
                slowColor: Theme.accentViolet
                bandColor: Theme.alpha(Theme.accentBlue, 0.7)
                entryColor: Theme.textPrimary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeCaption
                Accessible.role: Accessible.Graphic
                Accessible.name: qsTr("Candlestick chart of %1, %2 candles").arg(trading.symbolName).arg(trading.interval)

                // Scroll to zoom.
                WheelHandler {
                    onWheel: (event) => chart.visibleCandles = chart.visibleCandles + (event.angleDelta.y > 0 ? -15 : 15)
                }
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
