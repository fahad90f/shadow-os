// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.trading
import "../components/Format.js" as Format

// The paper account: what it is worth, how it got there, what is open.
NxPage {
    id: page

    readonly property var account: trading.account

    title: qsTr("Portfolio")
    subtitle: qsTr("Paper account · started with %1 %2").arg(Format.money(account.startingBalance)).arg(trading.accountCurrency)
    maxContentWidth: 1100
    actions: NxButton {
        kind: "danger"
        text: qsTr("Reset paper account")
        onClicked: resetDialog.open()
    }

    Labels { id: words }

    NxConfirmDialog {
        id: resetDialog
        title: qsTr("Reset the paper account?")
        message: qsTr("The balance goes back to %1 %2. Open positions and the whole trade history are discarded. This cannot be undone.")
            .arg(Format.money(trading.paperSettings.starting_balance)).arg(trading.accountCurrency)
        confirmText: qsTr("Reset")
        destructive: true
        onConfirmed: trading.resetPaperAccount()
    }

    GridLayout {
        Layout.fillWidth: true
        columns: width >= 880 ? 4 : 2
        columnSpacing: Theme.spaceSm
        rowSpacing: Theme.spaceSm
        NxStatTile {
            Layout.fillWidth: true
            label: qsTr("Account value")
            value: Format.money(page.account.equity)
            detail: qsTr("%1 since the start").arg(Format.signedPct(page.account.totalPnlPct))
            tone: page.account.totalPnl < 0 ? "danger" : page.account.totalPnl > 0 ? "success" : "neutral"
            icon: "pie"
        }
        NxStatTile {
            Layout.fillWidth: true
            label: qsTr("Today")
            value: Format.signedMoney(page.account.todayPnl)
            detail: Format.signedPct(page.account.todayPnlPct)
            tone: page.account.todayPnl < 0 ? "danger" : page.account.todayPnl > 0 ? "success" : "neutral"
            icon: "clock"
        }
        NxStatTile {
            Layout.fillWidth: true
            label: qsTr("Available")
            value: Format.money(page.account.available)
            detail: qsTr("%1 in open positions").arg(Format.money(page.account.balance - page.account.available))
            icon: "database"
        }
        NxStatTile {
            Layout.fillWidth: true
            label: qsTr("Deepest drawdown")
            value: Format.pct(page.account.maxDrawdownPct, 2)
            detail: qsTr("Now %1 below the peak").arg(Format.pct(page.account.drawdownPct, 2))
            tone: page.account.halted ? "danger" : "neutral"
            icon: "shield"
        }
    }

    NxCard {
        Layout.fillWidth: true
        implicitWidth: 480
        implicitHeight: 220
        padding: Theme.spaceMd
        ColumnLayout {
            anchors.fill: parent
            spacing: Theme.spaceSm
            Label {
                text: qsTr("Account value over time")
                color: Theme.textPrimary
                font.weight: Theme.weightSemibold
            }
            NxSparkline {
                Layout.fillWidth: true
                Layout.fillHeight: true
                readonly property var series: trading.equityCurve
                readonly property real floor: {
                    var low = Infinity
                    for (var i = 0; i < series.length; i++)
                        low = Math.min(low, series[i].v)
                    return isFinite(low) ? low : 0
                }
                values: series.map(p => p.v - floor)
                color: page.account.totalPnl >= 0 ? Theme.success : Theme.danger
                Accessible.name: qsTr("Account value over time")
            }
            Label {
                text: trading.equityCurve.length > 1
                    ? qsTr("From %1 to now · lowest %2 · peak %3").arg(Format.stamp(trading.equityCurve[0].t))
                          .arg(Format.money(Math.min.apply(null, trading.equityCurve.map(p => p.v))))
                          .arg(Format.money(page.account.peakEquity))
                    : qsTr("A point is added at every closed trade and every five minutes.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSizeCaption
            }
        }
    }

    NxSection {
        title: qsTr("Open positions")
        NxRow {
            visible: openModel.count === 0
            icon: "list"
            title: qsTr("No open positions")
        }
        Repeater {
            // Rows that stay while prices change, so Close can be clicked.
            model: TradesModel { id: openModel; source: trading; openOnly: true }
            NxRow {
                required property var trade
                readonly property var modelData: trade
                icon: modelData.side === "buy" ? "arrow-up" : "arrow-down"
                iconColor: words.actionColor(modelData.side)
                title: qsTr("%1 %2 · %3 %4").arg(words.action(modelData.side)).arg(modelData.name)
                    .arg(Format.quantity(modelData.quantity)).arg(Format.base(modelData.name))
                subtitle: qsTr("Entry %1 · now %2 · stop %3 · target %4 · opened %5").arg(Format.price(modelData.entry))
                    .arg(Format.price(modelData.price)).arg(Format.price(modelData.stop))
                    .arg(Format.price(modelData.takeProfit)).arg(Format.stamp(modelData.openedAt))
                value: qsTr("%1 %2").arg(Format.signedMoney(modelData.pnl)).arg(trading.accountCurrency)
                monoValue: true
                NxButton {
                    text: qsTr("Close")
                    onClicked: trading.closePosition(modelData.id)
                    Accessible.name: qsTr("Close the %1 position").arg(modelData.name)
                }
            }
        }
    }

    NxSection {
        title: qsTr("Results")
        NxRow { icon: "check-circle"; title: qsTr("Average win"); value: page.account.wins > 0 ? Format.money(page.account.averageWin) : "—"; monoValue: true }
        NxRow { icon: "x-circle"; title: qsTr("Average loss"); value: page.account.losses > 0 ? Format.money(page.account.averageLoss) : "—"; monoValue: true }
        NxRow { icon: "arrow-up"; title: qsTr("Best trade"); value: page.account.trades > 0 ? Format.signedMoney(page.account.bestTrade) : "—"; monoValue: true }
        NxRow { icon: "arrow-down"; title: qsTr("Worst trade"); value: page.account.trades > 0 ? Format.signedMoney(page.account.worstTrade) : "—"; monoValue: true }
    }
}
