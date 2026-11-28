// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.trading
import "../components/Format.js" as Format

// The trading desk: prices on top, the chart on the left, what the analysts
// say on the right, and the account underneath.
Item {
    id: page

    signal openPage(string key)

    readonly property var account: trading.account
    readonly property var roles: trading.analysts.filter(a => a.kind === "role")

    Labels { id: words }
    MarketsDialog { id: marketsDialog }

    // The desk fills the window; in a window too small for all of it, it
    // keeps its minimum size and scrolls.
    Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: width
        contentHeight: Math.max(height, 1010)
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        ScrollBar.vertical: ScrollBar {}

        ColumnLayout {
            x: Theme.spaceMd
            y: Theme.spaceMd
            width: flick.contentWidth - Theme.spaceMd * 2
            height: flick.contentHeight - Theme.spaceMd * 2
            spacing: Theme.spaceSm + 2

            // ---- mode, tagline, models ---------------------------------------
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceSm

                ModePill {}
                // The desk runs behind every page; this says what it is doing and opens it.
                NxButton {
                    kind: "ghost"
                    iconName: "zap"
                    // From the desk's real state: running, the market data, the tasks under way.
                    text: !desk.status.running ? qsTr("AI Desk  •  stopped")
                        : desk.status.feed === "error" ? qsTr("AI Desk  •  no market data")
                        : desk.tasks.length > 0 ? qsTr("AI Desk  •  %1 working").arg(desk.tasks.length)
                        : desk.status.feed === "waiting" ? qsTr("AI Desk  •  connecting")
                        : qsTr("AI Desk  •  live")
                    onClicked: page.openPage("desk")
                    Accessible.name: qsTr("Open the AI desk")
                }
                Label {
                    Layout.fillWidth: true
                    Layout.leftMargin: Theme.spaceLg
                    text: qsTr("MULTI-MODEL   •   REAL-TIME ANALYSIS   •   %1")
                        .arg(trading.autoTrade ? qsTr("AUTONOMOUS TRADING") : qsTr("AUTO TRADE OFF"))
                    color: Theme.accent
                    font.pixelSize: Theme.fontSizeSmall
                    font.italic: true
                    font.letterSpacing: 1.2
                    elide: Text.ElideRight
                }
                Repeater {
                    model: page.roles
                    ModelChip {
                        required property var modelData
                        analyst: modelData
                        labels: words
                        onClicked: page.openPage("models")
                    }
                }
                NxIconButton {
                    iconName: ApplicationWindow.window.visibility === Window.FullScreen ? "minimize" : "maximize"
                    tooltip: ApplicationWindow.window.visibility === Window.FullScreen ? qsTr("Leave full screen") : qsTr("Full screen")
                    onClicked: {
                        var w = ApplicationWindow.window
                        if (w.visibility === Window.FullScreen) w.showNormal(); else w.showFullScreen()
                    }
                }
            }

            // ---- watchlist and total -------------------------------------------
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceSm
                // The markets: side by side while they fit, scrolling sideways
                // when there are more.
                ListView {
                    id: markets
                    Layout.fillWidth: true
                    Layout.preferredHeight: 64
                    orientation: ListView.Horizontal
                    spacing: Theme.spaceSm
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    model: trading.watchlist
                    readonly property real cardWidth: Math.max(164, (width - spacing * (count - 1)) / Math.max(1, count))
                    delegate: TickerCard {
                        required property var modelData
                        width: markets.cardWidth
                        height: markets.height
                        market: modelData
                        removable: markets.count > 1
                        onChosen: (symbol) => trading.symbol = symbol
                        onRemoved: (symbol) => {
                            var error = trading.removeMarket(symbol)
                            if (error !== "")
                                ApplicationWindow.window.toaster.show(error, "warning")
                        }
                    }
                    // The wheel moves the row sideways.
                    WheelHandler {
                        enabled: markets.contentWidth > markets.width
                        onWheel: (event) => {
                            var delta = event.angleDelta.y !== 0 ? event.angleDelta.y : event.angleDelta.x
                            markets.contentX = Math.max(0, Math.min(markets.contentWidth - markets.width, markets.contentX - delta))
                        }
                    }
                    ScrollBar.horizontal: ScrollBar { policy: markets.contentWidth > markets.width ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff }
                }
                NxIconButton {
                    Layout.preferredWidth: 40
                    Layout.preferredHeight: 64
                    iconName: "plus"
                    tooltip: qsTr("Add a market")
                    onClicked: marketsDialog.open()
                }
                PnlCard {}
            }

            NxBanner {
                Layout.fillWidth: true
                visible: trading.dataState === "error"
                kind: "warning"
                title: qsTr("No market data")
                message: trading.dataError
            }

            // ---- chart and analysts ------------------------------------------------
            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: Theme.spaceSm + 2

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: Theme.spaceSm + 2

                    ChartPane {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.minimumHeight: 260
                        glow: true
                        labels: words
                    }

                    // ---- trades and portfolio, under the chart ----------------------
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 200
                        Layout.maximumHeight: 200
                        spacing: Theme.spaceSm + 2

                        Panel {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.preferredWidth: 5
                            title: qsTr("Live Trades")
                            icon: "activity"
                            trailing: [
                                StatusDot {
                                    text: trading.dataState === "live" ? qsTr("Live") : trading.dataState === "error" ? qsTr("Offline") : qsTr("Loading")
                                    tone: trading.dataState === "live" ? Theme.success : trading.dataState === "error" ? Theme.danger : Theme.textMuted
                                },
                                LinkButton { text: qsTr("View All"); onClicked: page.openPage("history") }
                            ]
                            TradesTable {
                                anchors.fill: parent
                                labels: words
                                maxRows: 12
                            }
                        }

                        Panel {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.preferredWidth: 5
                            title: qsTr("Portfolio Overview")
                            icon: "pie"
                            trailing: LinkButton { text: qsTr("View Portfolio"); onClicked: page.openPage("portfolio") }

                            RowLayout {
                                anchors.fill: parent
                                spacing: Theme.spaceMd
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    spacing: 2
                                    Label {
                                        text: Format.money(page.account.equity)
                                        color: Theme.textPrimary
                                        font.pixelSize: Theme.fontSizeTitle + 2
                                        font.weight: Theme.weightSemibold
                                    }
                                    Label {
                                        text: (page.account.todayPnl >= 0 ? "▲ " : "▼ ")
                                            + qsTr("%1 (%2 %3) today").arg(Format.signedPct(page.account.todayPnlPct)).arg(Format.signedMoney(page.account.todayPnl)).arg(trading.accountCurrency)
                                        color: page.account.todayPnl > 0 ? Theme.success : page.account.todayPnl < 0 ? Theme.danger : Theme.textMuted
                                        font.pixelSize: Theme.fontSizeSmall
                                    }
                                    NxSparkline {
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        Layout.minimumHeight: 36
                                        // Drawn relative to its lowest point so small moves show.
                                        readonly property var series: trading.equityCurve
                                        readonly property real floor: series.length > 0 ? Math.min.apply(null, series.map(p => p.v)) : 0
                                        values: series.map(p => p.v - floor)
                                        color: page.account.totalPnl >= 0 ? Theme.success : Theme.danger
                                        gridLines: 0
                                        Accessible.name: qsTr("Account value over time")
                                    }
                                }
                                ColumnLayout {
                                    Layout.fillHeight: true
                                    Layout.preferredWidth: 104
                                    spacing: Theme.spaceXs
                                    Repeater {
                                        model: [
                                            { label: qsTr("Win Rate"), value: page.account.trades > 0 ? Format.pct(page.account.winRatePct) : "—",
                                              tone: page.account.trades > 0 ? Theme.success : Theme.textPrimary },
                                            { label: qsTr("Total Trades"), value: String(page.account.trades), tone: Theme.textPrimary },
                                            { label: qsTr("Total P&L"), value: Format.signedMoney(page.account.totalPnl),
                                              tone: page.account.totalPnl > 0 ? Theme.success : page.account.totalPnl < 0 ? Theme.danger : Theme.textPrimary },
                                        ]
                                        ColumnLayout {
                                            required property var modelData
                                            spacing: 0
                                            Label { text: modelData.label; color: Theme.textMuted; font.pixelSize: Theme.fontSizeCaption }
                                            Label {
                                                text: modelData.value
                                                color: modelData.tone
                                                font.pixelSize: Theme.fontSizeBodyLarge
                                                font.weight: Theme.weightSemibold
                                            }
                                        }
                                    }
                                    Item { Layout.fillHeight: true }
                                }
                            }
                        }
                    }
                }

                // ---- the analysts, and risk under them ---------------------------------
                ColumnLayout {
                    Layout.preferredWidth: Math.max(430, Math.min(540, page.width * 0.345))
                    Layout.minimumWidth: 430
                    Layout.maximumWidth: 540
                    Layout.fillHeight: true
                    spacing: Theme.spaceSm + 2

                    SignalPanel {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        labels: words
                        onOpenPage: (key) => page.openPage(key)
                    }
                    RiskCard {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 200
                        Layout.maximumHeight: 200
                        onOpenPage: (key) => page.openPage(key)
                    }
                }
            }
        }
    }
}
