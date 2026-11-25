// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import "Format.js" as Format

// The account's total profit or loss since it started, large.
GlowCard {
    id: root

    readonly property real value: trading.account.totalPnl
    readonly property color figure: value > 0 ? Theme.success : value < 0 ? Theme.danger : Theme.textPrimary

    glow: true
    tone: value < 0 ? Theme.danger : Theme.success
    padding: Theme.spaceSm + 2
    implicitWidth: 236
    implicitHeight: 64
    Accessible.name: qsTr("Total profit and loss: %1 %2").arg(Format.signedMoney(value)).arg(trading.accountCurrency)

    RowLayout {
        anchors.fill: parent
        spacing: Theme.spaceSm
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0
            Label {
                text: qsTr("Total P&L")
                color: root.tone
                font.pixelSize: Theme.fontSizeSmall
                font.weight: Theme.weightMedium
            }
            RowLayout {
                spacing: Theme.spaceXs
                Label {
                    text: Format.signedMoney(root.value)
                    color: root.figure
                    font.pixelSize: Theme.fontSizeTitle
                    font.weight: Theme.weightSemibold
                }
                Label {
                    Layout.alignment: Qt.AlignBottom
                    Layout.bottomMargin: 3
                    text: trading.accountCurrency
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSizeCaption
                }
            }
        }
        // A small bar figure, rising for a profit and falling for a loss.
        Row {
            Layout.alignment: Qt.AlignBottom
            spacing: 3
            Repeater {
                model: root.value < 0 ? [22, 17, 12, 7] : [7, 12, 17, 22]
                Rectangle {
                    required property int modelData
                    anchors.bottom: parent.bottom
                    width: 6
                    height: modelData + 6
                    radius: 1.5
                    color: root.tone
                }
            }
        }
    }
}
