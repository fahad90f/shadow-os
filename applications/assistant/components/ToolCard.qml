// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui

// One tool call in the transcript. While "pending" it is a confirmation card
// showing the exact action (command/argv, path, …) with Run / Deny. After it
// resolves it shows the status and a collapsible result.
NxCard {
    id: card
    property var call           // {id,name,summary,preview,status,result,risky,changesSystem}
    property var controller     // the Assistant backend
    Layout.fillWidth: true
    padding: Theme.spaceMd
    accentBorder: call.status === "pending"

    readonly property string status: call ? call.status : ""
    readonly property bool pending: status === "pending"

    ColumnLayout {
        width: parent.width
        spacing: Theme.spaceSm

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceSm
            NxIcon {
                name: card.call && card.call.risky ? "terminal"
                    : card.call && card.call.changesSystem ? "edit" : "search"
                size: Theme.iconSmall
                color: card.pending ? Theme.accent
                    : card.status === "done" ? Theme.success
                    : card.status === "error" ? Theme.danger
                    : card.status === "denied" ? Theme.textMuted : Theme.textSecondary
            }
            Text {
                Layout.fillWidth: true
                text: (card.call ? card.call.summary : "") + (card.call && card.call.changesSystem ? "" : "")
                color: Theme.textPrimary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
                font.weight: Theme.weightSemibold
                elide: Text.ElideRight
            }
            NxBadge {
                visible: !card.pending
                text: card.status === "done" ? qsTr("done")
                    : card.status === "error" ? qsTr("failed")
                    : card.status === "running" ? qsTr("running")
                    : card.status === "denied" ? qsTr("denied") : card.status
                kind: card.status === "done" ? "success"
                    : card.status === "error" ? "danger"
                    : card.status === "denied" ? "neutral" : "accent"
            }
            NxBadge {
                visible: card.pending && card.call && card.call.risky
                text: qsTr("needs your approval")
                kind: "warning"
            }
        }

        // The exact action, shown verbatim (monospace).
        Rectangle {
            Layout.fillWidth: true
            visible: card.call && card.call.preview !== ""
            color: Theme.bgInput
            radius: Theme.radiusSm
            border.color: Theme.borderSubtle
            border.width: 1
            implicitHeight: previewText.implicitHeight + Theme.spaceSm * 2
            Text {
                id: previewText
                anchors.fill: parent
                anchors.margins: Theme.spaceSm
                text: card.call ? card.call.preview : ""
                color: Theme.textPrimary
                font.family: Theme.fontMono
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
            }
        }

        // Confirmation controls.
        RowLayout {
            visible: card.pending
            Layout.fillWidth: true
            spacing: Theme.spaceSm
            Item { Layout.fillWidth: true }
            NxButton {
                text: qsTr("Deny")
                kind: "ghost"
                onClicked: if (card.controller) card.controller.confirmTool(-1, card.call.id, false)
            }
            NxButton {
                text: qsTr("Run")
                kind: card.call && card.call.risky ? "danger" : "primary"
                iconName: "play"
                onClicked: if (card.controller) card.controller.confirmTool(-1, card.call.id, true)
            }
        }

        // Result (collapsible).
        ColumnLayout {
            Layout.fillWidth: true
            visible: !card.pending && card.call && card.call.result !== ""
            spacing: Theme.spaceXxs
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceXs
                NxIconButton {
                    iconName: resultBox.visible ? "chevron-down" : "chevron-right"
                    tooltip: resultBox.visible ? qsTr("Hide output") : qsTr("Show output")
                    onClicked: resultBox.visible = !resultBox.visible
                }
                Text {
                    text: qsTr("Output")
                    color: Theme.textSecondary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                }
                Item { Layout.fillWidth: true }
                NxIconButton {
                    iconName: "copy"
                    tooltip: qsTr("Copy output")
                    onClicked: if (card.controller) card.controller.copyToClipboard(card.call.result)
                }
            }
            Rectangle {
                id: resultBox
                visible: card.status === "error"
                Layout.fillWidth: true
                color: Theme.bgBase
                radius: Theme.radiusSm
                border.color: Theme.borderSubtle
                border.width: 1
                implicitHeight: Math.min(resultText.implicitHeight + Theme.spaceSm * 2, 220)
                clip: true
                Flickable {
                    anchors.fill: parent
                    anchors.margins: Theme.spaceSm
                    contentHeight: resultText.implicitHeight
                    contentWidth: width
                    clip: true
                    Text {
                        id: resultText
                        width: parent.width
                        text: card.call ? card.call.result : ""
                        color: Theme.textSecondary
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fontSizeSmall
                        wrapMode: Text.Wrap
                        textFormat: Text.PlainText
                    }
                }
            }
        }
    }
}
