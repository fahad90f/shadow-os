// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import org.shadow.assistant

// One line of the dialogue. Speaker names sit in the left margin like a
// play script (above the text on narrow windows); a name is only repeated
// when the speaker changes. "tool" rows stay hidden (their result lives in
// the action line) and take no space at all.
Item {
    id: line
    required property StageModel stage
    required property int index
    required property string kind
    required property string content
    required property bool streaming
    required property var toolCalls
    // layout of the script column (StagePage)
    required property real colX
    required property real textW
    required property real gutter

    readonly property var calls: stage.callsOf(toolCalls)
    readonly property bool isUser: kind === "user"
    readonly property bool isError: kind === "error"
    readonly property bool isHer: kind === "assistant"
    readonly property bool hidden: kind === "tool" || (isHer && content === "" && !streaming && calls.length === 0)
    readonly property bool waiting: isHer && streaming && content === "" && calls.length === 0
    readonly property string speaker: isUser ? "you" : "shadow"
    readonly property string prevSpeaker: {
        if (stage.structRevision < 0)  // a dependency (a bare read could be compiled away)
            return ""
        for (let i = index - 1; i >= 0; --i) {
            if (stage.hiddenAt(i))
                continue
            return stage.kindAt(i) === "user" ? "you" : "shadow"
        }
        return ""
    }
    readonly property bool showName: !hidden && speaker !== prevSpeaker
    readonly property bool narrow: gutter === 0
    readonly property real topGap: hidden ? 0 : index === 0 ? 6 : showName ? 30 : 12
    readonly property real nameH: narrow && showName ? 22 : 0
    readonly property bool done: isHer && content !== "" && !streaming
    // the start of the message, to tell its tools apart (screen readers)
    readonly property string gist: content.length > 60 ? content.slice(0, 57).replace(/\s+/g, " ") + "…"
                                                       : content.replace(/\s+/g, " ")
    // Where the sentence she is speaking starts, in this line's coordinates
    // (the view keeps it in sight while she speaks).
    function spokenY(): real {
        const prose = proseLoader.item as ProseBlocks
        return body.y + proseLoader.y + (prose ? prose.highlightY() : 0)
    }

    width: ListView.view ? ListView.view.width : 0
    height: hidden ? 0 : topGap + nameH + body.implicitHeight + 4
    visible: !hidden
    Accessible.role: Accessible.ListItem
    Accessible.name: (isUser ? qsTr("You") : qsTr("Shadow")) + ": "
                     + (streaming ? qsTr("writing…") : isError ? stage.errorHeadline(content) : content.slice(0, 400))

    HoverHandler { id: hover }

    // speaker name, in the margin
    Row {
        visible: line.showName
        x: line.narrow ? line.colX : line.colX - line.gutter
        y: line.topGap + (line.narrow ? 0 : (line.isUser ? 5 : 3))
        width: line.narrow ? implicitWidth : line.gutter - 22
        layoutDirection: line.narrow ? Qt.LeftToRight : Qt.RightToLeft
        spacing: 7
        Accessible.ignored: true
        Text {
            textFormat: Text.PlainText
            text: line.isUser ? qsTr("YOU") : qsTr("SHADOW")
            font.family: Pe.sans
            font.pixelSize: Pe.label
            font.weight: Font.Bold
            font.letterSpacing: Pe.labelTracking
            color: line.isUser ? Pe.you : line.isError ? Pe.alarm : Pe.her
        }
        Rectangle {
            visible: !line.isUser
            anchors.verticalCenter: parent.verticalCenter
            width: 5; height: 5; radius: 2.5
            color: line.isError ? Pe.alarm : Pe.her
            opacity: 0.8
        }
    }

    // wide windows: copy / read aloud wait in the margin, shown on hover or focus
    Row {
        id: marginTools
        visible: !line.narrow && line.done
        x: line.colX - 22 - width
        y: line.topGap + (line.showName ? 24 : 0)
        spacing: 0
        opacity: hover.hovered || copyBtn.activeFocus || aloudBtn.activeFocus ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: Pe.tQuick } }
        PeIconButton {
            id: copyBtn
            size: 28; iconSize: 15; iconName: "copy"; tip: qsTr("Copy")
            accessibleName: qsTr("Copy her answer: %1").arg(line.gist)
            onClicked: line.stage.copy(line.content)
        }
        PeIconButton {
            id: aloudBtn
            size: 28; iconSize: 15; iconName: "volume-high"; tip: qsTr("Read aloud")
            accessibleName: qsTr("Read her answer aloud: %1").arg(line.gist)
            enabled: line.stage.voice.ttsAvailable
            onClicked: line.stage.readAloud(line.index, line.content)
        }
    }

    Column {
        id: body
        x: line.colX
        y: line.topGap + line.nameH
        width: line.textW
        spacing: 12

        // what you said
        TextEdit {
            visible: line.isUser
            width: parent.width
            readOnly: true
            selectByMouse: true
            wrapMode: TextEdit.Wrap
            textFormat: TextEdit.RichText
            text: line.isUser ? "<p style=\"line-height:145%; margin:0;\">"
                  + line.content.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/\n/g, "<br>")
                  + "</p>" : ""
            font.family: Pe.sans
            font.pixelSize: Pe.lead
            color: Pe.ink
            selectionColor: Pe.alpha(Pe.cyan, 0.32)
            selectedTextColor: Pe.ink
        }

        // she is still thinking: three quiet stars
        Row {
            id: waitDots
            visible: line.waiting
            spacing: 6
            height: 24
            // one star at a time lights up (4 Hz, not a 60 fps animation)
            property int lit: 0
            Timer {
                interval: 260
                repeat: true
                running: line.waiting && line.stage.animate && !line.stage.frozen
                onTriggered: waitDots.lit = (waitDots.lit + 1) % 3
            }
            Repeater {
                model: 3
                delegate: Rectangle {
                    required property int index
                    anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                    width: 5; height: 5; radius: 2.5
                    color: Pe.her
                    opacity: waitDots.lit === index ? 1 : 0.35
                }
            }
            Text {
                textFormat: Text.PlainText
                anchors.verticalCenter: parent.verticalCenter
                leftPadding: 4
                text: line.stage.thinkHard ? qsTr("thinking hard") : qsTr("thinking")
                font.family: Pe.sans
                font.pixelSize: Pe.ui
                font.italic: true
                color: Pe.inkMute
            }
        }

        // her words
        Loader {
            id: proseLoader
            active: line.isHer && line.content !== ""
            visible: active
            width: parent.width
            sourceComponent: ProseBlocks {
                stage: line.stage
                width: line.textW
                markdown: line.content
                streaming: line.streaming
                highlight: line.stage.spokenRow === line.index ? line.stage.spokenSentence : ""
            }
        }

        // what she did
        Repeater {
            model: line.isHer ? line.calls : []
            delegate: ActionLine {
                required property var modelData
                stage: line.stage
                width: body.width
                call: modelData
            }
        }

        // something went wrong: said plainly, with a way forward
        // (only error rows build this: its bindings would otherwise follow
        // every streamed word of every reply)
        Loader {
            active: line.isError
            visible: active
            width: parent.width
            sourceComponent: Column {
                id: errorBlock
                width: line.textW
                spacing: 8
                readonly property var lines: line.content.split("\n")
                readonly property bool needsKey: line.stage.errorNeedsKey(line.content)
                Text {
                    textFormat: Text.PlainText
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: line.stage.errorHeadline(line.content)
                    font.family: Pe.sans
                    font.pixelSize: Pe.lead
                    color: Pe.ink
                }
                Text {
                    textFormat: Text.PlainText
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: errorBlock.lines[0]
                    font.family: Pe.sans
                    font.pixelSize: Pe.body
                    lineHeight: 1.45
                    color: Pe.inkSoft
                }
                Text {
                    textFormat: Text.PlainText
                    visible: errorBlock.lines.length > 1
                    width: parent.width
                    wrapMode: Text.WrapAnywhere
                    text: errorBlock.lines.slice(1).join("\n")
                    font.family: Pe.mono
                    font.pixelSize: Pe.small
                    color: Pe.inkMute
                }
                Flow {
                    width: parent.width
                    topPadding: 6
                    spacing: 8
                    PeButton {
                        visible: errorBlock.needsKey
                        kind: "primary"; compact: true
                        iconName: "key"
                        text: qsTr("Check my key")
                        onClicked: line.stage.openSettings("claude")
                    }
                    PeButton {
                        // only the latest failure can be tried again (the turn itself is retried)
                        visible: line.index === line.stage.lastShownRow
                        kind: errorBlock.needsKey ? "secondary" : "primary"; compact: true
                        iconName: "refresh"
                        text: qsTr("Try again")
                        enabled: line.stage.canSend
                        onClicked: line.stage.retry()
                    }
                    PeButton {
                        kind: "ghost"; compact: true
                        iconName: "copy"
                        text: qsTr("Copy details")
                        onClicked: line.stage.copy(line.content)
                    }
                }
            }
        }

        // narrow windows: her quiet tools follow the text
        Row {
            visible: line.narrow && line.done
            spacing: 2
            PeIconButton {
                size: 28; iconSize: 15; iconName: "copy"; tip: qsTr("Copy")
                accessibleName: qsTr("Copy her answer: %1").arg(line.gist)
                onClicked: line.stage.copy(line.content)
            }
            PeIconButton {
                size: 28; iconSize: 15; iconName: "volume-high"; tip: qsTr("Read aloud")
                accessibleName: qsTr("Read her answer aloud: %1").arg(line.gist)
                enabled: line.stage.voice.ttsAvailable
                onClicked: line.stage.readAloud(line.index, line.content)
            }
        }
    }
}
