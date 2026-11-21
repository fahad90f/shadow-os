// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import org.shadow.assistant

// Shadow asks before she acts. The persona steps forward next to this card
// (the window moves her face into `faceInset` on narrow windows), asks one
// clear question, shows the exact command, and waits for Run / Deny,
// Alt+Y / Alt+N, or a spoken "yes" / "no" (VoiceController asks aloud and
// listens; listenMode "yesno" while it does).
Rectangle {
    id: card
    required property StageModel stage
    property var call: null          // the first pending call
    property int pendingCount: 1
    property real faceInset: 0       // room on the left for her face (narrow windows)
    property bool compact: false
    // The call on the card: kept after it is answered, so the card fades out
    // whole (question, command, buttons) instead of as an empty shell.
    property var shown: null
    onCallChanged: if (call) shown = call
    onOpacityChanged: if (opacity === 0 && !call) shown = null
    readonly property bool listening: stage.capturing && stage.listenMode === "yesno"
    readonly property bool risky: shown ? shown.risky === true : false
    readonly property bool shell: shown ? (shown.name === "run_shell_command" || shown.name === "run_in_terminal") : false
    readonly property string preview: shown ? (shown.preview || "") : ""
    // A spoken answer works while she listens for one, or in a voice turn
    // (she asks aloud first); otherwise "Shadow, yes" does (wake word) or
    // Ctrl+Space and "yes" (push-to-talk).
    readonly property string voiceHint: listening ? qsTr("Say yes or no — I'm listening…")
        : stage.voice.voiceTurn && (stage.canTalk || stage.armed) ? qsTr("or just say “yes” or “no”")
        : stage.armed ? qsTr("or say “Shadow, yes”")
        : stage.canTalk ? qsTr("or press Ctrl+Space and say “yes”")
        : ""

    implicitHeight: col.implicitHeight + (compact ? 28 : 40)
    radius: Pe.rSheet
    color: Pe.dark ? "#0D1220" : "#FFFFFF"
    border.width: 1
    border.color: Pe.dark ? "#2A2F4D" : "#D9D4F2"

    Accessible.role: Accessible.AlertMessage
    Accessible.name: (shown ? stage.askQuestion(shown.name) : "") + " " + preview

    // a thread of her light along the top edge
    Rectangle {
        x: 18; width: parent.width - 36; y: 0; height: 2
        radius: 1
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: Pe.alpha(Pe.cyan, 0.0) }
            GradientStop { position: 0.3; color: card.risky ? Pe.caution : Pe.cyan }
            GradientStop { position: 0.7; color: card.risky ? Pe.alarm : Pe.violet }
            GradientStop { position: 1.0; color: Pe.alpha(Pe.violet, 0.0) }
        }
    }

    ColumnLayout {
        id: col
        x: (card.compact ? 18 : 24) + card.faceInset
        y: card.compact ? 14 : 20
        width: parent.width - (card.compact ? 36 : 48) - card.faceInset
        spacing: card.compact ? 9 : 12

        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            Text {
                textFormat: Text.PlainText
                text: qsTr("SHADOW ASKS")
                font.family: Pe.sans
                font.pixelSize: Pe.label
                font.weight: Font.Bold
                font.letterSpacing: Pe.labelTracking
                color: Pe.her
            }
            Rectangle {
                visible: card.risky
                implicitHeight: 20
                implicitWidth: riskText.implicitWidth + 16
                radius: 10
                color: Pe.alpha(Pe.caution, 0.14)
                Text {
                    id: riskText
                    textFormat: Text.PlainText
                    anchors.centerIn: parent
                    text: card.shell ? qsTr("runs in your shell") : qsTr("changes your system")
                    font.family: Pe.sans
                    font.pixelSize: Pe.label
                    font.weight: Font.DemiBold
                    color: Pe.chipText(Pe.caution)
                }
            }
            Item { Layout.fillWidth: true }
            Text {
                textFormat: Text.PlainText
                visible: card.pendingCount > 1
                text: qsTr("1 of %1").arg(card.pendingCount)
                font.family: Pe.sans
                font.pixelSize: Pe.small
                color: Pe.inkMute
            }
        }

        Text {
            textFormat: Text.PlainText
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: card.shown ? card.stage.askQuestion(card.shown.name) : ""
            font.family: Pe.sans
            font.pixelSize: card.compact ? 19 : 23
            font.weight: Font.Light
            color: Pe.ink
        }

        // the exact thing, verbatim (calls without arguments have none to show)
        Rectangle {
            visible: card.preview !== ""
            Layout.fillWidth: true
            implicitHeight: Math.min(cmd.implicitHeight, card.compact ? 120 : 200) + 24
            radius: Pe.rCard
            color: Pe.dark ? "#070A10" : "#F3F4F8"
            border.width: 1
            border.color: Pe.line
            clip: true
            Text {
                textFormat: Text.PlainText
                visible: card.shell
                x: 14; y: 12
                text: "$"
                font.family: Pe.mono
                font.pixelSize: 14
                color: Pe.cyan
            }
            Flickable {
                id: cmdFlick
                x: card.shell ? 32 : 14
                y: 12
                width: parent.width - x - 14
                height: parent.height - 24
                contentHeight: cmd.implicitHeight
                interactive: contentHeight > height
                boundsBehavior: Flickable.StopAtBounds
                clip: true
                TextEdit {
                    id: cmd
                    width: cmdFlick.width
                    text: card.preview
                    readOnly: true
                    selectByMouse: true
                    activeFocusOnPress: false
                    wrapMode: TextEdit.WrapAnywhere
                    textFormat: TextEdit.PlainText
                    font.family: Pe.mono
                    font.pixelSize: 14
                    color: Pe.ink
                    selectionColor: Pe.alpha(Pe.violet, 0.38)
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            PeIcon {
                name: card.risky ? "alert-triangle" : "shield-check"
                size: 14
                color: card.risky ? Pe.caution : Pe.ok
            }
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: card.stage.riskNote(card.shown)
                font.family: Pe.sans
                font.pixelSize: Pe.small
                color: Pe.inkSoft
            }
        }

        // a spoken answer that decided nothing
        Text {
            textFormat: Text.PlainText
            visible: card.stage.unclearAnswer !== "" && !card.listening
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: qsTr("I heard “%1” — that's not a yes or a no. Use the buttons, or press the mic and say it again.")
                  .arg(card.stage.unclearAnswer)
            font.family: Pe.sans
            font.pixelSize: Pe.small
            color: Pe.caution
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 4
            spacing: 10
            PeButton {
                kind: card.risky ? "danger" : "primary"
                iconName: "play"
                text: qsTr("Run")
                hint: "Alt+Y"
                enabled: card.shown !== null
                onClicked: if (card.call) card.stage.confirm(card.call.id, true)
            }
            PeButton {
                kind: "secondary"
                text: qsTr("Deny")
                hint: "Esc"
                enabled: card.shown !== null
                onClicked: if (card.call) card.stage.confirm(card.call.id, false)
            }
            PeButton {
                visible: card.pendingCount > 1 && !card.compact
                kind: "ghost"
                text: qsTr("Run all %1").arg(card.pendingCount)
                onClicked: card.stage.confirmAll(true)
            }
            Item { Layout.fillWidth: true }
            Row {
                visible: card.voiceHint !== ""
                spacing: 8
                LevelBars {
                    visible: card.listening
                    anchors.verticalCenter: parent.verticalCenter
                    level: card.stage.drawnMicLevel
                    maxHeight: 16
                }
                PeIcon {
                    visible: !card.listening
                    anchors.verticalCenter: parent.verticalCenter
                    name: "mic"
                    size: 14
                    color: Pe.inkMute
                }
                Text {
                    textFormat: Text.PlainText
                    anchors.verticalCenter: parent.verticalCenter
                    text: card.voiceHint
                    font.family: Pe.sans
                    font.pixelSize: Pe.small
                    font.weight: card.listening ? Font.DemiBold : Font.Normal
                    color: card.listening ? Pe.cyan : Pe.inkMute
                }
            }
        }
    }
}
