// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Templates as T
import org.shadow.assistant

// The cue line at the bottom of the stage: talk, type, think hard, send.
// While she listens for a command (push-to-talk, after "Shadow", a spoken
// yes/no) it turns into the listening bar (level, Cancel, Done), so a turn
// can be finished early and sent (Ctrl+Space again). A follow-up listen
// after a spoken reply keeps the text field (StagePage shows a small cue).
Item {
    id: bar
    required property StageModel stage
    readonly property bool listening: stage.showsListening && stage.listenMode !== "followup"
    property alias input: field
    implicitHeight: listening ? 60 : Math.max(58, scroller.implicitHeight + 16)

    function sendNow() {
        if (stage.send(field.text))
            field.clear()
    }

    Rectangle {
        id: shell
        anchors.fill: parent
        radius: bar.listening || bar.height < 70 ? height / 2 : 22
        color: Pe.card
        border.width: 1
        border.color: bar.listening ? Pe.alpha(Pe.cyan, 0.55)
                    : field.activeFocus ? Pe.alpha(Pe.violet, 0.6) : Pe.line
        Behavior on border.color { ColorAnimation { duration: Pe.tQuick } }
    }

    // ---- typing --------------------------------------------------------------------
    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 9
        anchors.rightMargin: 9
        spacing: 8
        visible: !bar.listening

        PeIconButton {
            Layout.alignment: Qt.AlignBottom
            Layout.bottomMargin: 8
            size: 40
            iconSize: 19
            quiet: false
            iconName: bar.stage.micMuted ? "mic-off" : "mic"
            tint: bar.stage.micMuted ? Pe.inkFaint : Pe.ink
            enabled: bar.stage.canTalk
            tip: bar.stage.talkBlocker !== "" ? bar.stage.talkBlocker : qsTr("Talk to Shadow (Ctrl+Space)")
            onClicked: bar.stage.talk()
        }

        ScrollView {
            id: scroller
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            Layout.maximumHeight: 132
            implicitHeight: Math.min(132, field.implicitHeight)
            TextArea {
                id: field
                textFormat: TextEdit.PlainText
                wrapMode: TextArea.Wrap
                background: null
                padding: 6
                topPadding: 9
                bottomPadding: 9
                font.family: Pe.sans
                font.pixelSize: Pe.body
                color: Pe.ink
                placeholderTextColor: Pe.inkMute
                selectionColor: Pe.alpha(Pe.violet, 0.38)
                selectedTextColor: Pe.ink
                placeholderText: !bar.stage.keyConfigured ? qsTr("Add your Claude API key first")
                               : bar.stage.hasPending ? qsTr("Answer Shadow's question above — or type ahead")
                               : bar.stage.working ? qsTr("Type ahead — you can send when she's done")
                               : bar.stage.armed ? qsTr("Type to Shadow, or just say “Shadow”…")
                               : qsTr("Type to Shadow…")
                Accessible.name: qsTr("Message to Shadow")
                Accessible.description: qsTr("Enter sends, Shift+Enter starts a new line")
                Keys.onPressed: (event) => {
                    if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter)
                            && !(event.modifiers & Qt.ShiftModifier)) {
                        bar.sendNow()
                        event.accepted = true
                    }
                }
                // Tab / Shift+Tab move on (a tab character is never wanted in a message)
                Keys.onTabPressed: (event) => {
                    const next = field.nextItemInFocusChain(true)
                    if (next)
                        next.forceActiveFocus(Qt.TabFocusReason)
                    event.accepted = true
                }
                Keys.onBacktabPressed: (event) => {
                    const prev = field.nextItemInFocusChain(false)
                    if (prev)
                        prev.forceActiveFocus(Qt.BacktabFocusReason)
                    event.accepted = true
                }
            }
        }

        PeButton {
            Layout.alignment: Qt.AlignBottom
            Layout.bottomMargin: 11
            visible: bar.width > 560
            compact: true
            kind: "ghost"
            iconName: "lab"
            text: bar.stage.thinkHard ? qsTr("Thinking hard") : qsTr("Think hard")
            tip: qsTr("Route the next request to the planner model (Ctrl+T)")
            checkable: true
            checked: bar.stage.thinkHard
            onClicked: bar.stage.setThinkHard(!bar.stage.thinkHard)
            Rectangle {
                z: -1
                anchors.fill: parent
                radius: height / 2
                visible: bar.stage.thinkHard
                color: Pe.alpha(Pe.violet, 0.16)
                border.width: 1
                border.color: Pe.alpha(Pe.violet, 0.5)
            }
        }
        PeIconButton {
            Layout.alignment: Qt.AlignBottom
            Layout.bottomMargin: 11
            visible: bar.width <= 560
            size: 32
            iconName: "lab"
            checkable: true
            checked: bar.stage.thinkHard
            activeTint: Pe.violet
            tip: qsTr("Think hard (Ctrl+T)")
            onClicked: bar.stage.setThinkHard(!bar.stage.thinkHard)
        }

        // send: lit with her colours when there is something to send
        T.Button {
            id: sendButton
            Layout.alignment: Qt.AlignBottom
            Layout.bottomMargin: 8
            implicitWidth: 40
            implicitHeight: 40
            readonly property bool ready: bar.stage.canSend && field.text.trim() !== ""
            enabled: ready
            focusPolicy: Qt.StrongFocus
            hoverEnabled: true
            Accessible.role: Accessible.Button
            Accessible.name: qsTr("Send")
            ToolTip.visible: hovered
            ToolTip.text: bar.stage.canSend ? qsTr("Send (Enter)") : qsTr("You can send when she's done")
            ToolTip.delay: 500
            onClicked: bar.sendNow()
            background: Rectangle {
                radius: 20
                color: Pe.alpha(Pe.ink, 0.06)
                Rectangle {
                    anchors.fill: parent
                    radius: 20
                    visible: sendButton.ready
                    opacity: sendButton.down ? 0.85 : 1
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0.0; color: Pe.dark ? "#22D8F5" : "#1BA9C9" }
                        GradientStop { position: 1.0; color: Pe.dark ? "#A585FF" : "#7B52E6" }
                    }
                }
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: -3
                    radius: width / 2
                    color: "transparent"
                    border.width: 2
                    border.color: Pe.cyan
                    visible: sendButton.visualFocus
                }
            }
            contentItem: Item {
                PeIcon {
                    anchors.centerIn: parent
                    name: "arrow-up"
                    size: 18
                    color: sendButton.ready ? "#06080C" : Pe.inkFaint
                }
            }
        }
    }

    // ---- listening -----------------------------------------------------------------
    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 22
        anchors.rightMargin: 10
        spacing: 14
        visible: bar.listening

        LevelBars { level: bar.stage.drawnMicLevel; maxHeight: 24 }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 1
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                elide: Text.ElideRight
                text: bar.stage.listenMode === "yesno" || bar.stage.voiceState === "transcribing"
                      ? bar.stage.listeningText : qsTr("Listening")
                font.family: Pe.sans
                font.pixelSize: Pe.ui
                font.weight: Font.DemiBold
                color: Pe.ink
            }
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                elide: Text.ElideRight
                text: bar.stage.listenMode === "yesno" ? qsTr("Answer the question above — “yes” runs it, “no” doesn't")
                    : bar.stage.voiceState === "transcribing" ? qsTr("One moment…")
                    : qsTr("Speak naturally — I'll stop when you pause")
                font.family: Pe.sans
                font.pixelSize: Pe.small
                color: Pe.inkMute
            }
        }
        PeButton {
            kind: "ghost"
            text: qsTr("Cancel")
            hint: bar.width > 520 ? "Esc" : ""
            onClicked: bar.stage.cancelTalk()
        }
        PeButton {
            kind: "accent"
            iconName: "check"
            text: qsTr("Done")
            hint: bar.width > 520 ? "Ctrl+Space" : ""
            enabled: bar.stage.canFinishTalk
            onClicked: bar.stage.finishTalk()
        }
    }
}
