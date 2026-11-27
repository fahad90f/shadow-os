// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.trading

// The AI desk: a conversation with an analyst that reads every message as it
// arrives, keeps the analysis of the markets discussed, and watches the live
// market at the same time. The desk runs from the start of the application,
// whichever page is open; this page shows it.
Item {
    id: page

    signal openPage(string key)

    NxConfirmDialog {
        id: clearDialog
        title: qsTr("Clear the conversation?")
        message: qsTr("Every message and everything the desk has worked out from them (levels, signals, plan, conclusions) is removed. Answers being written are stopped. The paper account and the analysis team's calls are not touched.")
        confirmText: qsTr("Clear")
        destructive: true
        onConfirmed: desk.clearConversation()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spaceMd
        spacing: Theme.spaceSm + 2

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceSm
            ColumnLayout {
                spacing: 0
                Label {
                    text: qsTr("AI Desk")
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontSizeTitle
                    font.weight: Theme.weightSemibold
                }
                Label {
                    text: qsTr("Reads every message as it arrives  •  keeps the analysis  •  watches the live market  •  answers with %1").arg(desk.settings.deepName)
                    color: Theme.accent
                    font.pixelSize: Theme.fontSizeSmall
                    font.italic: true
                    font.letterSpacing: 0.8
                }
            }
            Item { Layout.fillWidth: true }
            Label {
                text: qsTr("Chart follows the conversation")
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSizeSmall
            }
            Switch {
                checked: desk.settings.followChart
                onToggled: desk.setFollowChart(checked)
                Accessible.name: qsTr("Chart follows the conversation")
            }
            NxButton {
                kind: "ghost"
                iconName: "trash-2"
                text: qsTr("Clear")
                enabled: desk.messages.count > 0
                onClicked: clearDialog.open()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.spaceSm + 2
            DeskChat {
                Layout.fillWidth: true
                Layout.fillHeight: true
            }
            DeskAnalysis {
                Layout.preferredWidth: Math.max(380, Math.min(480, page.width * 0.34))
                Layout.fillHeight: true
            }
        }
    }
}
