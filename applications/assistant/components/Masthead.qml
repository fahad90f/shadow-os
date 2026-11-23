// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import org.shadow.assistant

// The top line. Left: the conversation title (opens the list) and the cue
// line — what Shadow is doing right now, with Stop. Right: the wake word,
// the microphone, a new conversation, history and settings.
Item {
    id: head
    required property StageModel stage
    property string mode: "stage"        // stage | settings | history | setup
    property real leftInset: 24
    property bool showWordmark: false
    readonly property bool compact: width < 860
    signal titleClicked()
    signal backClicked()
    height: 64
    // The horizontal span free between her words on the left and the tools on
    // the right (window coordinates): whispers float there, over nothing.
    readonly property real usedLeft: head.mode === "stage" ? leftBox.x + Math.max(titleRow.width, cueRow.width)
        : head.mode === "setup" ? brand.x + brand.width
        : leftBox.x + pageTitleRow.width
    readonly property real usedRight: rightRow.x
    // While a whisper shows it takes the top line, never the dialogue: on
    // wide windows the quiet middle (the title and the cue line make room),
    // on narrower ones the place of the title and the cue line themselves.
    property bool whispering: false
    readonly property bool wideTop: width >= 1000
    readonly property real whisperReserve: wideTop && whispering ? 340 : 0
    readonly property real laneLeft: head.mode === "setup" ? brand.x + brand.width + 20
        : wideTop ? usedLeft + 20 : leftBox.x
    readonly property real laneRight: usedRight - 16

    // ---- left ---------------------------------------------------------------------
    Row {
        id: brand
        visible: head.showWordmark
        x: 28
        anchors.verticalCenter: parent.verticalCenter
        spacing: 8
        Accessible.ignored: true
        Repeater {
            model: 3
            delegate: Rectangle {
                required property int index
                anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                width: index === 1 ? 5 : 3; height: width; radius: width / 2
                color: Pe.brandAt(index / 2)
            }
        }
        Text {
            textFormat: Text.PlainText
            anchors.verticalCenter: parent.verticalCenter
            leftPadding: 2
            text: "SHADOW"
            font.family: Pe.sans
            font.pixelSize: 12
            font.weight: Font.Bold
            font.letterSpacing: 3.2
            color: Pe.ink
        }
    }

    Item {
        id: leftBox
        x: head.showWordmark ? brand.x + brand.width + 28 : head.leftInset
        width: Math.max(0, rightRow.x - x - 16)
        height: parent.height
        // narrow windows: a whisper stands in for the title and the cue line
        opacity: head.whispering && !head.wideTop && head.mode !== "setup" ? 0 : 1
        visible: opacity > 0.01
        Behavior on opacity { NumberAnimation { duration: Pe.tQuick } }

        // stage: title + cue line
        Column {
            visible: head.mode === "stage"
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width
            spacing: 3
            Item {
                id: titleButton
                // only the title and its chevron open the list (not the empty top line)
                width: titleRow.width + 12
                height: 24
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: qsTr("Conversations: %1").arg(head.stage.activeTitle)
                Accessible.description: qsTr("Opens the list of conversations (Ctrl+K)")
                Keys.onReturnPressed: head.titleClicked()
                Keys.onEnterPressed: head.titleClicked()
                Keys.onSpacePressed: head.titleClicked()
                Rectangle {
                    anchors.fill: titleRow
                    anchors.margins: -6
                    radius: 8
                    color: titleTap.pressed ? Pe.alpha(Pe.ink, 0.1) : titleHover.hovered ? Pe.alpha(Pe.ink, 0.06) : "transparent"
                    border.width: titleButton.activeFocus ? 2 : 0
                    border.color: Pe.cyan
                }
                Row {
                    id: titleRow
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 6
                    Text {
                        textFormat: Text.PlainText
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.min(implicitWidth, Math.max(80, leftBox.width - 30 - head.whisperReserve))
                        elide: Text.ElideRight
                        maximumLineCount: 1
                        text: head.stage.activeTitleLine
                        font.family: Pe.sans
                        font.pixelSize: 15
                        font.weight: Font.DemiBold
                        color: Pe.ink
                    }
                    PeIcon {
                        anchors.verticalCenter: parent.verticalCenter
                        name: head.stage.conversationsOpen ? "chevron-up" : "chevron-down"
                        size: 14
                        color: Pe.inkMute
                    }
                }
                HoverHandler { id: titleHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { id: titleTap; onTapped: head.titleClicked() }
                ToolTip.visible: titleHover.hovered
                ToolTip.text: qsTr("Conversations (Ctrl+K)")
                ToolTip.delay: 600
            }
            Row {
                id: cueRow
                spacing: 8
                height: 22
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 6; height: 6; radius: 3
                    color: head.stage.cueTone
                }
                Text {
                    textFormat: Text.PlainText
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(implicitWidth, Math.max(60, leftBox.width - head.whisperReserve - 14
                                                            - (tagRow.visible ? tagRow.width + 8 : 0)
                                                            - (stopButton.visible ? stopButton.width + 8 : 0)))
                    elide: Text.ElideRight
                    text: head.stage.cueText
                    font.family: Pe.sans
                    font.pixelSize: Pe.small
                    color: Pe.inkSoft
                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }
                Row {
                    id: tagRow
                    visible: head.stage.cueTags.length > 0
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 8
                    Repeater {
                        model: head.stage.cueTags
                        delegate: Rectangle {
                            id: tag
                            required property var modelData
                            anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                            height: 18
                            width: tagText.implicitWidth + 14
                            radius: 9
                            color: Pe.alpha(tag.modelData.tone, 0.14)
                            Text {
                                id: tagText
                                textFormat: Text.PlainText
                                anchors.centerIn: parent
                                text: tag.modelData.text
                                font.family: Pe.sans
                                font.pixelSize: 10
                                font.weight: Font.Bold
                                font.letterSpacing: 0.8
                                color: Pe.chipText(tag.modelData.tone)
                            }
                        }
                    }
                }
                Rectangle {
                    id: skillChip
                    visible: head.stage.activeSkills.length > 0
                    anchors.verticalCenter: parent.verticalCenter
                    height: 18
                    width: skillRow.implicitWidth + 14
                    radius: 9
                    color: Pe.alpha(Pe.her, 0.16)
                    Accessible.role: Accessible.StaticText
                    Accessible.name: qsTr("Using skill %1").arg(head.stage.activeSkills.join(", "))
                    Row {
                        id: skillRow
                        anchors.centerIn: parent
                        spacing: 4
                        PeIcon { anchors.verticalCenter: parent.verticalCenter; name: "lab"; size: 11; color: Pe.her }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            textFormat: Text.PlainText
                            text: head.stage.activeSkills.length === 0 ? ""
                                  : head.stage.activeSkills[0].replace(/-/g, " ")
                                    + (head.stage.activeSkills.length > 1 ? " +" + (head.stage.activeSkills.length - 1) : "")
                            font.family: Pe.sans; font.pixelSize: 10; font.weight: Font.Bold; font.letterSpacing: 0.6
                            color: Pe.chipText(Pe.her)
                        }
                    }
                }
                PeButton {
                    id: stopButton
                    visible: head.stage.working || head.stage.speaking || head.stage.hasPending
                    anchors.verticalCenter: parent.verticalCenter
                    height: 22
                    compact: true
                    leftPadding: 9; rightPadding: 10
                    kind: "secondary"
                    iconName: "stop"
                    text: qsTr("Stop")
                    hint: "Esc"
                    tip: head.stage.hasPending ? qsTr("Stop: say no to the question and stop") : qsTr("Stop answering and speaking (Esc)")
                    onClicked: head.stage.stop()
                }
            }
        }

        // settings / history: back + title
        Row {
            id: pageTitleRow
            visible: head.mode === "settings" || head.mode === "history"
            anchors.verticalCenter: parent.verticalCenter
            spacing: 10
            PeIconButton {
                anchors.verticalCenter: parent.verticalCenter
                iconName: "arrow-left"
                tip: qsTr("Back to the conversation (Esc)")
                onClicked: head.backClicked()
            }
            Text {
                textFormat: Text.PlainText
                anchors.verticalCenter: parent.verticalCenter
                text: head.mode === "settings" ? qsTr("Settings") : qsTr("History & undo")
                font.family: Pe.sans
                font.pixelSize: 22
                font.weight: Font.Light
                color: Pe.ink
                Accessible.role: Accessible.Heading
                Accessible.name: text
            }
            // a question waits on the stage: say so here, one click from it
            PeButton {
                visible: head.stage.hasPending
                anchors.verticalCenter: parent.verticalCenter
                compact: true
                kind: "secondary"
                iconName: "help"
                text: qsTr("Shadow is waiting for your answer")
                tip: qsTr("Back to her question (Esc)")
                onClicked: head.backClicked()
            }
        }
    }

    // ---- right --------------------------------------------------------------------
    Row {
        id: rightRow
        anchors.right: parent.right
        anchors.rightMargin: head.compact ? 12 : 22
        anchors.verticalCenter: parent.verticalCenter
        spacing: 4

        // the wake word: always visible, always one click away
        Rectangle {
            id: wakeChip
            visible: head.mode !== "setup"
            anchors.verticalCenter: parent.verticalCenter
            height: 32
            width: head.compact ? 32 : wakeRow.implicitWidth + 24
            radius: 16
            color: wakeTap.pressed ? Pe.alpha(Pe.ink, 0.1) : wakeHover.hovered ? Pe.alpha(Pe.ink, 0.06) : "transparent"
            border.width: wakeChip.activeFocus ? 2 : 1
            border.color: wakeChip.activeFocus ? Pe.cyan : head.stage.armed ? Pe.alpha(Pe.cyan, 0.35) : Pe.line
            Accessible.role: Accessible.CheckBox
            Accessible.name: qsTr("Listen for the wake word “Shadow”")
            Accessible.description: head.stage.wakeText
            Accessible.checked: head.stage.wakeEnabled
            activeFocusOnTab: true
            Keys.onSpacePressed: head.stage.setWake(!head.stage.wakeEnabled)
            Keys.onReturnPressed: head.stage.setWake(!head.stage.wakeEnabled)
            Row {
                id: wakeRow
                anchors.centerIn: parent
                spacing: 8
                Item {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 10; height: 10
                    // armed: every few seconds a faint ring pings around the dot
                    // (two discrete frames, no running animation)
                    Rectangle {
                        anchors.centerIn: parent
                        width: 18; height: 18; radius: 9
                        color: "transparent"
                        border.width: 1
                        border.color: Pe.alpha(head.stage.wakeTone, 0.55)
                        visible: head.stage.armed && ping.on
                        Timer {
                            id: ping
                            property bool on: false
                            property int n: 0
                            interval: 700
                            repeat: true
                            running: head.stage.armed && head.stage.animate && head.stage.appActive && !head.stage.frozen
                            onTriggered: {
                                n = (n + 1) % 6
                                on = n === 0
                            }
                            onRunningChanged: if (!running) on = false
                        }
                    }
                    Rectangle {
                        anchors.centerIn: parent
                        width: 10; height: 10; radius: 5
                        color: Pe.alpha(head.stage.wakeTone, 0.25)
                        visible: head.stage.armed
                    }
                    Rectangle {
                        anchors.centerIn: parent
                        width: 6; height: 6; radius: 3
                        color: head.stage.wakeTone
                    }
                }
                Text {
                    textFormat: Text.PlainText
                    visible: !head.compact
                    anchors.verticalCenter: parent.verticalCenter
                    text: head.stage.wakeText
                    font.family: Pe.sans
                    font.pixelSize: Pe.small
                    font.weight: Font.Medium
                    color: head.stage.armed ? Pe.ink : Pe.inkSoft
                }
            }
            HoverHandler { id: wakeHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { id: wakeTap; onTapped: head.stage.setWake(!head.stage.wakeEnabled) }
            ToolTip.visible: wakeHover.hovered
            ToolTip.text: (head.compact ? head.stage.wakeText + " — " : "")
                          + (head.stage.wakeEnabled ? qsTr("Stop listening for “Shadow” (Ctrl+Shift+W)")
                                                    : qsTr("Listen for “Shadow” (Ctrl+Shift+W)"))
            ToolTip.delay: 500
        }
        Item { width: 6; height: 1 }
        PeIconButton {
            visible: head.mode !== "setup"
            anchors.verticalCenter: parent.verticalCenter
            iconName: head.stage.micMuted ? "mic-off" : "mic"
            checkable: true
            checked: head.stage.micMuted
            activeTint: Pe.alarm
            tip: head.stage.micMuted ? qsTr("Microphone off — turn it on (Ctrl+M)") : qsTr("Turn the microphone off (Ctrl+M)")
            onClicked: head.stage.setMuted(!head.stage.micMuted)
        }
        PeIconButton {
            visible: head.mode === "stage"
            anchors.verticalCenter: parent.verticalCenter
            iconName: "plus"
            enabled: !head.stage.busy
            tip: qsTr("New conversation (Ctrl+N)")
            onClicked: head.stage.newConversation()
        }
        PeIconButton {
            visible: head.mode !== "setup"
            anchors.verticalCenter: parent.verticalCenter
            iconName: "undo"
            checkable: true
            checked: head.mode === "history"
            activeTint: Pe.her
            tip: qsTr("History & undo (Ctrl+H)")
            onClicked: head.stage.view = head.mode === "history" ? "stage" : "history"
        }
        PeIconButton {
            anchors.verticalCenter: parent.verticalCenter
            iconName: "settings"
            checkable: true
            checked: head.mode === "settings"
            activeTint: Pe.her
            tip: qsTr("Settings (Ctrl+,)")
            onClicked: head.stage.view = head.mode === "settings" ? "stage" : "settings"
        }
    }
}
