// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import "Format.js" as Format

// One row of the desk's conversation (a row of desk.messages): a message of
// the person (right, with what the desk took from it), an answer of the desk
// (left, written word by word, with Stop and Ask again), or a note from the
// market, the analysis team or the desk itself (a thin line).
Item {
    id: root

    required property double messageId
    required property string role      // "you" | "desk" | "note"
    required property string text
    required property string phase     // see DeskMessages::Message::state
    required property string kind
    required property double time
    required property string by
    required property var tags
    required property double ms
    required property string detail

    readonly property bool working: phase === "waiting" || phase === "thinking" || phase === "writing"
    readonly property bool canRetry: phase === "failed" || phase === "stopped" || phase === "interrupted"

    implicitHeight: loader.implicitHeight
    height: implicitHeight

    Loader {
        id: loader
        width: parent.width
        sourceComponent: root.role === "you" ? mine : root.role === "desk" ? answer : noteLine
    }

    // ---- a message of the person --------------------------------------------------
    Component {
        id: mine
        ColumnLayout {
            spacing: 3
            Rectangle {
                Layout.alignment: Qt.AlignRight
                Layout.preferredWidth: Math.min(body.implicitWidth + Theme.spaceMd * 2, root.width * 0.78)
                Layout.preferredHeight: body.implicitHeight + Theme.spaceSm * 2
                radius: Theme.radiusMd
                color: Theme.alpha(Theme.accent, 0.18)
                border.width: Theme.borderWidth
                border.color: Theme.alpha(Theme.accent, 0.45)
                TextEdit {
                    id: body
                    x: Theme.spaceMd
                    y: Theme.spaceSm
                    width: Math.min(implicitWidth, root.width * 0.78 - Theme.spaceMd * 2)
                    text: root.text
                    readOnly: true
                    selectByMouse: true
                    wrapMode: Text.Wrap
                    color: Theme.textPrimary
                    selectionColor: Theme.alpha(Theme.accent, 0.5)
                    font.pixelSize: Theme.fontSizeBody
                }
            }
            // What the desk took from it, as soon as it was read.
            Flow {
                Layout.alignment: Qt.AlignRight
                Layout.preferredWidth: root.width * 0.78
                visible: root.tags.length > 0
                spacing: 4
                layoutDirection: Qt.RightToLeft
                Repeater {
                    model: root.tags
                    Rectangle {
                        required property string modelData
                        height: 20
                        width: tag.implicitWidth + 12
                        radius: 10
                        color: Theme.alpha(Theme.accent, 0.10)
                        border.width: 1
                        border.color: Theme.alpha(Theme.accent, 0.35)
                        Label {
                            id: tag
                            anchors.centerIn: parent
                            text: parent.modelData
                            color: Theme.accent
                            font.pixelSize: Theme.fontSizeSmall - 1
                        }
                    }
                }
            }
            Label {
                Layout.alignment: Qt.AlignRight
                text: Format.clock(root.time) + "  ·  " + (root.phase === "accepted" ? qsTr("Accepted")
                    : root.phase === "read" ? qsTr("Read") + (root.detail !== "" ? "  ·  " + root.detail : "")
                    : (root.detail !== "" ? root.detail : qsTr("Understood")))
                color: Theme.textMuted
                font.pixelSize: Theme.fontSizeSmall - 1
            }
        }
    }

    // ---- an answer of the desk -------------------------------------------------------
    Component {
        id: answer
        ColumnLayout {
            spacing: 3
            opacity: root.phase === "superseded" ? 0.45 : 1
            RowLayout {
                spacing: Theme.spaceXs
                NxIcon {
                    name: root.kind === "live" ? "activity" : root.kind === "info" ? "info" : "cpu"
                    size: Theme.iconSmall
                    color: Theme.accent
                }
                Label {
                    text: root.by
                    color: Theme.accent
                    font.pixelSize: Theme.fontSizeSmall
                    font.weight: Theme.weightSemibold
                }
                Label {
                    Layout.maximumWidth: root.width * 0.5
                    text: root.phase === "waiting" ? qsTr("waiting for its turn…")
                        : root.phase === "thinking" ? qsTr("thinking…")
                        : root.phase === "writing" ? qsTr("writing…")
                        : root.phase === "stopped" ? qsTr("stopped")
                        : root.phase === "failed" ? qsTr("no answer: %1").arg(root.detail)
                        : root.phase === "interrupted" ? qsTr("cut off: %1").arg(root.detail)
                        : root.phase === "superseded" ? qsTr("asked again below")
                        : root.ms > 0 ? qsTr("%1  ·  %2 s").arg(Format.clock(root.time)).arg((root.ms / 1000).toFixed(1))
                        : Format.clock(root.time)
                    color: root.phase === "failed" ? Theme.warning : Theme.textMuted
                    font.pixelSize: Theme.fontSizeSmall - 1
                    elide: Text.ElideRight
                }
                // Lit while the answer is really under way.
                Rectangle {
                    visible: root.working
                    Layout.preferredWidth: 8
                    Layout.preferredHeight: 8
                    radius: 4
                    color: Theme.accent
                    SequentialAnimation on opacity {
                        running: root.working
                        loops: Animation.Infinite
                        NumberAnimation { from: 1; to: 0.25; duration: 600 }
                        NumberAnimation { from: 0.25; to: 1; duration: 600 }
                    }
                }
                NxButton {
                    visible: root.working && root.kind === "model"
                    kind: "ghost"
                    iconName: "stop"
                    text: qsTr("Stop")
                    Layout.preferredHeight: 26
                    onClicked: desk.stopAnswer(root.messageId)
                }
                NxButton {
                    visible: root.canRetry
                    kind: "ghost"
                    iconName: "refresh-cw"
                    text: qsTr("Ask again")
                    Layout.preferredHeight: 26
                    onClicked: desk.retryAnswer(root.messageId)
                }
            }
            Rectangle {
                Layout.preferredWidth: Math.min(body.implicitWidth + Theme.spaceMd * 2, root.width * 0.88)
                Layout.preferredHeight: body.implicitHeight + Theme.spaceSm * 2
                visible: root.text !== ""
                radius: Theme.radiusMd
                color: Theme.mix(Theme.bgRaised, Theme.accent, 0.05)
                border.width: Theme.borderWidth
                border.color: Theme.alpha(Theme.textSecondary, 0.25)
                TextEdit {
                    id: body
                    x: Theme.spaceMd
                    y: Theme.spaceSm
                    width: Math.min(implicitWidth, root.width * 0.88 - Theme.spaceMd * 2)
                    text: root.text
                    textFormat: root.kind === "model" ? TextEdit.MarkdownText : TextEdit.PlainText
                    readOnly: true
                    selectByMouse: true
                    wrapMode: Text.Wrap
                    color: Theme.textPrimary
                    selectionColor: Theme.alpha(Theme.accent, 0.5)
                    font.pixelSize: Theme.fontSizeBody
                }
            }
        }
    }

    // ---- a note: the market, the team, the desk -------------------------------------
    Component {
        id: noteLine
        RowLayout {
            id: line
            readonly property color tone: root.kind === "warning" ? Theme.warning
                : root.kind === "market" ? Theme.success : Theme.accent
            spacing: Theme.spaceSm
            Rectangle {
                Layout.preferredWidth: 3
                Layout.fillHeight: true
                radius: 1.5
                color: line.tone
            }
            NxIcon {
                Layout.alignment: Qt.AlignTop
                name: root.kind === "market" ? "activity" : root.kind === "team" ? "users"
                    : root.kind === "warning" ? "alert-triangle" : "info"
                size: Theme.iconSmall
                color: line.tone
            }
            Label {
                Layout.fillWidth: true
                text: root.text
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.Wrap
            }
            Label {
                Layout.alignment: Qt.AlignTop
                text: Format.clock(root.time)
                color: Theme.textMuted
                font.pixelSize: Theme.fontSizeSmall - 1
            }
        }
    }
}
