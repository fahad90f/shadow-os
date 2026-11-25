// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.trading
import "../components/Format.js" as Format

// Every trade the paper account made.
Item {
    id: page

    readonly property var account: trading.account
    Labels { id: words }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spaceXl
        spacing: Theme.spaceMd

        Label {
            text: qsTr("Trade History")
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSizeTitle
            font.weight: Theme.weightSemibold
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("Open positions and closed trades of the paper account. Hover a row to read the reasoning it was opened on.")
            color: Theme.textMuted
            font.pixelSize: Theme.fontSizeSmall
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceSm
            NxStatTile {
                Layout.fillWidth: true
                label: qsTr("Closed trades")
                value: String(page.account.trades)
                detail: qsTr("%1 won · %2 lost").arg(page.account.wins).arg(page.account.losses)
                icon: "list"
            }
            NxStatTile {
                Layout.fillWidth: true
                label: qsTr("Win rate")
                value: page.account.trades > 0 ? Format.pct(page.account.winRatePct) : "—"
                detail: page.account.trades > 0 ? qsTr("of %1 closed trades").arg(page.account.trades) : qsTr("No closed trades yet")
                icon: "target"
            }
            NxStatTile {
                Layout.fillWidth: true
                label: qsTr("Realized profit")
                value: Format.signedMoney(page.account.realizedPnl)
                detail: qsTr("%1, after fees").arg(trading.accountCurrency)
                tone: page.account.realizedPnl < 0 ? "danger" : page.account.realizedPnl > 0 ? "success" : "neutral"
                icon: "activity"
            }
            NxStatTile {
                Layout.fillWidth: true
                label: qsTr("Profit factor")
                value: page.account.profitFactor > 0 ? Format.number(page.account.profitFactor, 2) : "—"
                detail: qsTr("Gross profit / gross loss")
                icon: "pie"
            }
        }

        NxCard {
            Layout.fillWidth: true
            Layout.fillHeight: true
            padding: Theme.spaceMd
            implicitWidth: 480
            implicitHeight: 320
            TradesTable {
                anchors.fill: parent
                labels: words
                showReason: true
                showClose: true
            }
        }
    }
}
