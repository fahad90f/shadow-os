// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import "Format.js" as Format

// The risk rules at a glance: how much of the account is at risk in open
// positions, and where the account stands against each limit.
Panel {
    id: root

    signal openPage(string key)
    // The link to the Risk Manager page (not shown on that page itself).
    property bool showLink: true

    readonly property var account: trading.account
    readonly property var limits: trading.riskSettings
    // The most that can be at risk at once: every position slot at full size.
    readonly property real riskCeiling: limits.risk_per_trade_pct * limits.max_open_positions
    readonly property real dayLossPct: Math.max(0, -account.todayPnlPct)

    component Check: RowLayout {
        id: check
        property bool ok: true
        property string text: ""
        Layout.fillWidth: true
        spacing: Theme.spaceXs
        NxIcon {
            name: check.ok ? "check-circle" : "alert-triangle"
            size: Theme.iconSmall
            color: check.ok ? Theme.success : Theme.warning
        }
        Label {
            Layout.fillWidth: true
            text: check.text
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSizeSmall
            elide: Text.ElideRight
        }
    }

    title: qsTr("Risk Manager")
    icon: "shield"
    trailing: [
        StatusDot {
            text: root.account.halted ? qsTr("Halted") : root.account.dailyLossReached ? qsTr("Paused today") : qsTr("Active")
            tone: root.account.halted ? Theme.danger : root.account.dailyLossReached ? Theme.warning : Theme.success
        },
        LinkButton { visible: root.showLink; text: qsTr("Rules"); onClicked: root.openPage("risk") }
    ]

    RowLayout {
        anchors.fill: parent
        spacing: Theme.spaceMd

        NxGauge {
            Layout.preferredWidth: 104
            Layout.preferredHeight: 104
            Layout.alignment: Qt.AlignVCenter
            value: root.riskCeiling > 0 ? root.account.openRiskPct / root.riskCeiling : 0
            text: Format.pct(root.account.openRiskPct, 1)
            label: qsTr("Risk")
            color: root.account.halted ? Theme.danger : Theme.success
            HoverHandler { id: gaugeHover }
            ToolTip.visible: gaugeHover.hovered
            ToolTip.text: qsTr("Share of the account lost if every open position hits its stop. The ring is full at %1.")
                .arg(Format.pct(root.riskCeiling, 1))
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.spaceXs

            Check { text: qsTr("Position Size: up to %1 risk").arg(Format.pct(root.limits.risk_per_trade_pct, 1)) }
            Check { text: qsTr("SL: Set Automatically") }
            Check { text: qsTr("TP: Set Automatically") }
            Check {
                ok: !root.account.dailyLossReached
                text: qsTr("Daily Loss: %1 of %2").arg(Format.pct(root.dayLossPct, 2)).arg(Format.pct(root.limits.max_daily_loss_pct, 1))
            }
            Check {
                ok: !root.account.halted
                text: qsTr("Max Drawdown: %1 of %2").arg(Format.pct(root.account.maxDrawdownPct, 1)).arg(Format.pct(root.limits.max_drawdown_pct, 0))
            }
            NxButton {
                visible: root.account.halted
                text: qsTr("Resume trading")
                onClicked: trading.resumeTrading()
            }
            Item { Layout.fillHeight: true }
        }
    }
}
