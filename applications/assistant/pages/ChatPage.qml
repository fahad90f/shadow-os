// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import "../components"

// The conversation view: a presence header, the scrolling transcript (with a
// welcoming empty state), and the composer.
Item {
    id: page
    property var controller

    readonly property var agent: controller
    readonly property var messages: controller ? controller.messages : null

    function sendCurrent() {
        const text = input.text.trim()
        if (text === "" || !agent.canSend)
            return
        agent.sendMessage(text)
        input.clear()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---- header ----
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: headerRow.implicitHeight + Theme.spaceMd * 2
            color: Theme.alpha(Theme.bgSurface, Appearance.transparency ? 0.6 : 1.0)
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.borderSubtle }
            RowLayout {
                id: headerRow
                anchors.fill: parent
                anchors.leftMargin: Theme.spaceLg
                anchors.rightMargin: Theme.spaceLg
                anchors.topMargin: Theme.spaceMd
                anchors.bottomMargin: Theme.spaceMd
                spacing: Theme.spaceMd

                AssistantOrb {
                    orbState: page.agent ? page.agent.orbState : "idle"
                    implicitWidth: 40; implicitHeight: 40
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Text {
                        text: {
                            if (!page.agent || page.agent.activeId === "") return qsTr("New conversation")
                            return page.agent.conversations.titleOf(page.agent.activeId) || qsTr("Conversation")
                        }
                        color: Theme.textPrimary
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeBodyLarge
                        font.weight: Theme.weightSemibold
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Text {
                        text: page.agent && page.agent.thinkHard
                            ? qsTr("Deep reasoning (planner model)") : qsTr("Nemotron · ready")
                        color: Theme.textMuted
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeCaption
                    }
                }
                NxIconButton {
                    iconName: "lab"
                    tooltip: qsTr("Think hard — route to the deep planner model")
                    checkable: true
                    checked: page.agent ? page.agent.thinkHard : false
                    onToggled: page.agent.thinkHard = checked
                }
                NxButton {
                    visible: page.agent && page.agent.busy
                    text: qsTr("Stop")
                    kind: "danger"
                    iconName: "stop"
                    onClicked: page.agent.stop()
                }
            }
        }

        // ---- transcript ----
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: Theme.spaceMd
            topMargin: Theme.spaceLg
            bottomMargin: Theme.spaceLg
            leftMargin: Theme.spaceLg
            rightMargin: Theme.spaceLg
            model: page.messages
            cacheBuffer: 4000
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            delegate: MessageBubble {
                controller: page.agent
            }

            onCountChanged: positionViewAtEnd()
            Connections {
                target: page.messages
                function onCountChanged() { Qt.callLater(list.positionViewAtEnd) }
            }

            // Empty state: hero orb + greeting + starter prompts.
            Item {
                anchors.fill: parent
                visible: page.messages && page.messages.count === 0
                ColumnLayout {
                    anchors.centerIn: parent
                    width: Math.min(parent.width - Theme.spaceXl * 2, 520)
                    spacing: Theme.spaceLg
                    AssistantOrb {
                        Layout.alignment: Qt.AlignHCenter
                        orbState: page.agent ? page.agent.orbState : "idle"
                        implicitWidth: 180; implicitHeight: 180
                    }
                    Text {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        text: qsTr("How can I help?")
                        color: Theme.textPrimary
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeHeading
                        font.weight: Theme.weightSemibold
                    }
                    Text {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                        text: qsTr("Ask a question or give me a task. I can run commands, read and write files, and open apps — always with your confirmation.")
                        color: Theme.textSecondary
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeBody
                    }
                    Flow {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.fillWidth: true
                        spacing: Theme.spaceSm
                        Repeater {
                            model: [qsTr("Summarise my system info"),
                                    qsTr("What's using my disk space?"),
                                    qsTr("List files in my home folder")]
                            delegate: NxButton {
                                required property string modelData
                                text: modelData
                                kind: "secondary"
                                enabled: page.agent && page.agent.canSend
                                onClicked: { input.text = modelData; page.sendCurrent() }
                            }
                        }
                    }
                }
            }
        }

        // ---- composer ----
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: composer.implicitHeight + Theme.spaceMd * 2
            color: Theme.bgSurface
            Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: Theme.borderSubtle }
            RowLayout {
                id: composer
                anchors.fill: parent
                anchors.margins: Theme.spaceMd
                spacing: Theme.spaceSm

                Rectangle {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    radius: Theme.radiusMd
                    color: Theme.bgInput
                    border.color: input.activeFocus ? Theme.accent : Theme.border
                    border.width: 1
                    implicitHeight: Math.min(Math.max(inputScroll.implicitHeight, Theme.controlHeight), 160)

                    ScrollView {
                        id: inputScroll
                        anchors.fill: parent
                        anchors.margins: Theme.spaceXs
                        TextArea {
                            id: input
                            placeholderText: page.agent && !page.agent.config.apiKeyConfigured
                                ? qsTr("Add your NVIDIA API key in Settings to begin…")
                                : qsTr("Message Shadow Assistant…   (Enter to send, Shift+Enter for a new line)")
                            enabled: page.agent && page.agent.canSend
                            wrapMode: TextArea.Wrap
                            color: Theme.textPrimary
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeBody
                            background: null
                            Keys.onPressed: (event) => {
                                if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                                        && !(event.modifiers & Qt.ShiftModifier)) {
                                    page.sendCurrent()
                                    event.accepted = true
                                }
                            }
                        }
                    }
                }
                NxIconButton {
                    Layout.alignment: Qt.AlignBottom
                    iconName: "arrow-up-circle"
                    iconSize: Theme.icon
                    tooltip: qsTr("Send")
                    enabled: page.agent && page.agent.canSend && input.text.trim() !== ""
                    onClicked: page.sendCurrent()
                }
            }
        }
    }
}
