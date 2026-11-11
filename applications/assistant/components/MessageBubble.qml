// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui

// One row in the chat transcript. User messages sit to the right in an accent
// bubble; assistant messages render Markdown; tool calls render as ToolCards;
// errors render as an inline danger banner. "tool" result rows are hidden (the
// result is shown inside the assistant message's ToolCard).
Item {
    id: bubble
    required property string kind
    required property string content
    required property bool streaming
    required property var toolCalls
    property var controller

    readonly property bool isUser: kind === "user"
    readonly property bool isAssistant: kind === "assistant"
    readonly property bool isError: kind === "error"
    readonly property bool isToolResult: kind === "tool"

    width: ListView.view ? ListView.view.width : parent.width
    visible: !isToolResult
    implicitHeight: visible ? col.implicitHeight : 0

    ColumnLayout {
        id: col
        width: parent.width
        spacing: Theme.spaceXs

        // ---- error ----
        NxBanner {
            visible: bubble.isError
            Layout.fillWidth: true
            kind: "danger"
            title: qsTr("Something went wrong")
            message: bubble.content
        }

        // ---- user / assistant text ----
        RowLayout {
            Layout.fillWidth: true
            visible: (bubble.isUser || bubble.isAssistant) && (bubble.content !== "" || bubble.streaming)
            Item { visible: bubble.isUser; Layout.fillWidth: true }

            NxCard {
                id: textCard
                Layout.maximumWidth: Math.round(parent.width * (bubble.isUser ? 0.82 : 1.0))
                Layout.fillWidth: !bubble.isUser
                padding: Theme.spaceMd
                // Accent tint for the user; glass surface for the assistant.
                property color tint: bubble.isUser ? Theme.alpha(Theme.accent, 0.16) : Theme.bgRaised

                Rectangle {
                    anchors.fill: parent
                    radius: Theme.radiusMd
                    color: textCard.tint
                    border.color: bubble.isUser ? Theme.alpha(Theme.accent, 0.4) : Theme.borderSubtle
                    border.width: 1
                    z: -1
                }

                ColumnLayout {
                    width: parent.width
                    spacing: Theme.spaceXs
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spaceXs
                        NxIcon {
                            visible: bubble.isAssistant
                            name: "shadow"; size: Theme.iconSmall; color: Theme.accent
                        }
                        Text {
                            text: bubble.isUser ? qsTr("You") : qsTr("Assistant")
                            color: Theme.textMuted
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeCaption
                            font.weight: Theme.weightMedium
                        }
                        Item { Layout.fillWidth: true }
                        BusyIndicator {
                            visible: bubble.streaming && bubble.content === ""
                            running: visible
                            implicitWidth: Theme.iconSmall; implicitHeight: Theme.iconSmall
                        }
                        NxIconButton {
                            visible: !bubble.streaming && bubble.content !== ""
                            iconName: "copy"
                            tooltip: qsTr("Copy message")
                            onClicked: if (bubble.controller) bubble.controller.copyToClipboard(bubble.content)
                        }
                    }
                    TextEdit {
                        id: body
                        Layout.fillWidth: true
                        text: bubble.content
                        readOnly: true
                        selectByMouse: true
                        wrapMode: Text.Wrap
                        textFormat: bubble.isAssistant ? TextEdit.MarkdownText : TextEdit.PlainText
                        color: Theme.textPrimary
                        selectionColor: Theme.alpha(Theme.accent, 0.4)
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeBody
                    }
                }
            }
            Item { visible: bubble.isAssistant; Layout.fillWidth: true; Layout.maximumWidth: 1 }
        }

        // ---- tool calls ----
        Repeater {
            model: bubble.isAssistant ? bubble.toolCalls : []
            delegate: ToolCard {
                required property var modelData
                Layout.fillWidth: true
                call: modelData
                controller: bubble.controller
            }
        }
    }
}
