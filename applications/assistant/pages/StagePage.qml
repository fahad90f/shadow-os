// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.assistant

// The stage. With nothing to read, Shadow stands in the spotlight (face,
// greeting, live caption while you speak). As soon as there is something to
// read she steps aside to the corner and the dialogue takes the stage, set
// like a script. When she needs a decision she steps forward next to the
// question card. The window reads faceRect to move her face.
Item {
    id: page
    required property StageModel stage
    property real topInset: 64
    readonly property bool wide: width >= 900
    readonly property var pending: stage.pendingInfo.call
    // A pending decision keeps her beside the card, even while she listens
    // for a spoken "yes" / "no".
    readonly property bool asking: stage.hasPending && pending !== null
    // Push-to-talk and "Shadow, …" bring her into the spotlight with a live
    // caption; a follow-up listen after a spoken reply does not (you are
    // reading her answer), nor does a spoken yes/no (she stays by the card).
    readonly property bool captionListening: stage.showsListening && stage.listenMode !== "followup"
                                             && stage.listenMode !== "yesno"
    readonly property bool spotlight: (!stage.hasReading || captionListening || stage.showsWake) && !asking

    function focusPrompt() {
        promptBar.input.forceActiveFocus()
    }
    // the conversation view (the window scrolls what Tab reaches into view)
    readonly property Flickable scriptView: script
    function stopFollowing() {
        script.pinned = script.atYEnd
    }
    // the prompt has the keyboard whenever the stage is shown
    onVisibleChanged: if (visible) Qt.callLater(focusPrompt)
    Component.onCompleted: if (visible) Qt.callLater(focusPrompt)

    // ---- geometry -------------------------------------------------------------------
    readonly property real gutter: wide ? 124 : 0
    readonly property real side: wide ? 40 : 16
    // The text column is centred on the window; its speaker margin hangs to the
    // left, clear of her face in the corner.
    readonly property real minColX: wide ? 230 : side
    readonly property real textW: Math.max(200, Math.min(680, width - side - Math.max(minColX, (width - 680) / 2)))
    readonly property real colX: Math.round(Math.max(minColX, (width - textW) / 2))
    readonly property bool roomyHint: height >= 640 && spotlight && !stage.showsListening
    readonly property real bottomBlock: promptBar.height + (wide ? 26 : 14) + (roomyHint ? 26 : 0)

    readonly property real spotSize: Math.max(120, Math.min(380, width * 0.34,
                                      (height - topInset - bottomBlock - spotText.implicitHeight - 30) / 0.86))
    readonly property real spotY: topInset + Math.max(0, (height - topInset - bottomBlock
                                  - (spotSize * 0.86 + 6 + spotText.implicitHeight)) * 0.45)
    readonly property rect dockRect: wide ? Qt.rect(18, 2, 104, 104) : Qt.rect(12, 9, 46, 46)
    readonly property real askFace: Math.max(96, Math.min(156, askCard.height * 0.7, askCard.x - 30))
    readonly property rect faceRect: asking
        ? (wide ? Qt.rect(Math.max(8, askCard.x - askFace - 14), askCard.y + (askCard.height - askFace) / 2 - 6, askFace, askFace)
                : Qt.rect(askCard.x + 14, askCard.y + 16, 50, 50))
        : spotlight ? Qt.rect((width - spotSize) / 2, spotY, spotSize, spotSize) : dockRect

    // ---- the dialogue ------------------------------------------------------------------
    ListView {
        id: script
        anchors.fill: parent
        anchors.topMargin: page.topInset + 6
        // while she asks, the dialogue ends just above the question card
        anchors.bottomMargin: page.bottomBlock + (page.asking ? askCard.height + 14 : 0)
        visible: opacity > 0.01
        opacity: page.spotlight ? 0 : 1
        Behavior on opacity { NumberAnimation { duration: Pe.tDock } }
        clip: true
        model: page.stage.messages
        spacing: 0
        cacheBuffer: 1600
        boundsBehavior: Flickable.StopAtBounds
        // While she writes, room is kept below her words (the margin), so the
        // view moves in steps of a third of a page instead of with every word:
        // each move repaints the whole view, which matters without a GPU.
        readonly property bool growing: page.stage.phase === "streaming" || page.stage.phase === "thinking"
        bottomMargin: growing ? Math.round(height * 0.33) : 16
        topMargin: 12
        ScrollBar.vertical: PeScrollBar {}
        Accessible.role: Accessible.List
        Accessible.name: qsTr("Conversation")
        delegate: ScriptLine {
            stage: page.stage
            colX: page.colX
            textW: page.textW
            gutter: page.gutter
        }

        // keep following a growing answer while the reader is at the end
        property bool pinned: true
        // While she speaks an answer, the view follows the sentence she is
        // saying (it trails the text that streams in), not the end of the text.
        readonly property bool followSpeech: page.stage.speaking && page.stage.spokenRow >= 0
        function follow() {
            if (pinned)
                Qt.callLater(followNow)
        }
        function followNow() {
            if (!pinned)
                return
            if (followSpeech && showSpoken())
                return
            // the end of her words is still in view: nothing has to move
            if (growing && originY + contentHeight <= contentY + height - 4)
                return
            positionViewAtEnd()
        }
        // Brings the sentence being spoken into the upper part of the view.
        function showSpoken(): bool {
            const row = page.stage.spokenRow
            if (row < 0 || row >= count)
                return false
            let line = itemAtIndex(row) as ScriptLine
            if (!line) {
                positionViewAtIndex(row, ListView.Beginning)
                line = itemAtIndex(row) as ScriptLine
            }
            if (!line)
                return false
            const y = line.y + line.spokenY()
            if (y >= contentY + height * 0.08 && y <= contentY + height * 0.7)
                return true
            const lo = originY - topMargin
            const hi = Math.max(lo, originY + contentHeight + bottomMargin - height)
            contentY = Math.max(lo, Math.min(hi, y - height * 0.3))
            return true
        }
        Connections {
            target: page.stage
            function onSpokenRowChanged() { script.follow() }
            function onSpokenSentenceChanged() { script.follow() }
            function onSpeakingChanged() { script.follow() }
        }
        onMovementStarted: pinned = false
        onMovementEnded: pinned = atYEnd
        onContentHeightChanged: follow()
        onGrowingChanged: follow()
        onCountChanged: {
            pinned = true
            follow()
        }
        onHeightChanged: follow()
        Component.onCompleted: follow()
    }

    // ---- the spotlight ---------------------------------------------------------------------
    Column {
        id: spotText
        x: (page.width - width) / 2
        y: page.spotY + page.spotSize * 0.86 + 6
        width: Math.min(620, page.width - 40)
        spacing: 10
        visible: opacity > 0.01
        opacity: page.spotlight ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: Pe.tDock } }

        readonly property bool needsVoice: page.stage.engine === "missing" || page.stage.engine === "basic"
                                           || page.stage.engine === "installing" || page.stage.engine === "error"
        // Your words (live, or the line of the turn she now works on) come
        // first; in the middle of a conversation the spotlight never falls
        // back to the greeting, not even in the instant it goes down.
        readonly property string liveMode: page.stage.showsWake ? "wake"
            : page.captionListening ? "caption"
            : page.stage.lastUserLine !== "" && page.stage.working ? "cue"
            : page.stage.hasReading ? "caption"
            : page.stage.micMuted ? "muted"
            : needsVoice ? "voiceless"
            : "home"
        // What the spotlight shows follows the stage only while it is up; as
        // it fades out it keeps what it showed (never the greeting or an
        // earlier question in the middle of a conversation).
        // (follows the stage from the first frame; the Binding below takes
        // over while the spotlight is up and leaves the last value when it goes)
        property string mode: liveMode
        Binding {
            target: spotText
            property: "mode"
            when: page.spotlight
            value: spotText.liveMode
            restoreMode: Binding.RestoreNone
        }
        // the words of the caption, kept the same way (the final transcript
        // stays while the caption fades)
        property string heldCaption: liveCaption
        // the transcript of this listen, once it is final
        property string heardAtStart: ""
        Connections {
            target: page
            function onCaptionListeningChanged() {
                if (page.captionListening)
                    spotText.heardAtStart = page.stage.lastHeard
            }
        }
        readonly property string newlyHeard: page.stage.lastHeard !== heardAtStart ? page.stage.lastHeard : ""
        readonly property string liveCaption: page.stage.partialText !== "" ? page.stage.partialText
            : newlyHeard !== "" ? newlyHeard
            : page.stage.voiceState === "transcribing" ? qsTr("Catching that…")
            : qsTr("I'm listening…")
        Binding {
            target: spotText
            property: "heldCaption"
            when: page.spotlight && spotText.liveMode === "caption"
            value: spotText.liveCaption
            restoreMode: Binding.RestoreNone
        }
        property string heldUserLine: page.stage.lastUserLine
        Binding {
            target: spotText
            property: "heldUserLine"
            when: page.spotlight
            value: page.stage.lastUserLine
            restoreMode: Binding.RestoreNone
        }

        // greeting (home, muted, voiceless)
        Text {
            textFormat: Text.PlainText
            visible: spotText.mode === "home" || spotText.mode === "muted" || spotText.mode === "voiceless"
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            text: spotText.mode === "muted" ? qsTr("I'm not listening.")
                : spotText.mode !== "voiceless" ? page.stage.greeting
                : page.stage.engine === "installing" ? qsTr("I'm getting my voice.")
                : page.stage.engine === "error" ? qsTr("I lost my voice.")
                : page.stage.engine === "basic" ? page.stage.greeting
                : qsTr("I don't have my voice yet.")
            font.family: Pe.sans
            font.pixelSize: page.height < 640 ? 26 : Pe.display
            font.weight: Font.Light
            color: Pe.ink
            Accessible.role: Accessible.Heading
            Accessible.name: text
        }
        Text {
            textFormat: Text.PlainText
            visible: spotText.mode === "home" || spotText.mode === "muted" || spotText.mode === "voiceless"
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            lineHeight: 1.3
            text: spotText.mode === "muted"
                  ? qsTr("The microphone is off, so I can't hear anything. Typing still works.")
                  : spotText.mode === "voiceless"
                  ? (page.stage.engine === "installing"
                     ? qsTr("You can type to me while my natural voice installs. It runs entirely on this computer.")
                     : page.stage.engine === "basic"
                     ? qsTr("I can answer with a basic voice, but to hear you I need my natural voice.")
                     : page.stage.engine === "error"
                     ? qsTr("My voice engine stopped. Typing still works.")
                     : qsTr("You can type to me. To talk, install my natural voice — it runs entirely on this computer."))
                  : page.stage.armed ? qsTr("Say “Shadow” whenever you need me — or type below.")
                  : page.stage.canTalk ? qsTr("Press Ctrl+Space and talk, or type below.")
                  : page.stage.wakeEnabled && page.stage.engine === "starting" ? qsTr("My voice is starting — or type below.")
                  : qsTr("Type below, or press Ctrl+Space to talk.")
            font.family: Pe.sans
            font.pixelSize: Pe.body
            color: Pe.inkSoft
        }

        // muted: one clear way back
        Item {
            visible: spotText.mode === "muted"
            width: parent.width
            height: 48
            PeButton {
                anchors.centerIn: parent
                kind: "primary"
                iconName: "mic"
                text: qsTr("Turn the microphone on")
                hint: "Ctrl+M"
                onClicked: page.stage.setMuted(false)
            }
        }

        // no (natural) voice: install it — progress fills her mouth, too
        Rectangle {
            visible: spotText.mode === "voiceless"
            width: parent.width
            height: installCol.implicitHeight + 32
            radius: Pe.rSheet
            color: Pe.card
            border.width: 1
            border.color: Pe.line
            ColumnLayout {
                id: installCol
                x: 20; y: 16
                width: parent.width - 40
                spacing: 10
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    Text {
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        text: page.stage.installing ? qsTr("Installing the natural voice")
                            : page.stage.engine === "error" ? qsTr("Voice engine")
                            : qsTr("Natural voice engine")
                        font.family: Pe.sans
                        font.pixelSize: Pe.ui
                        font.weight: Font.DemiBold
                        color: Pe.ink
                    }
                    Text {
                        textFormat: Text.PlainText
                        visible: page.stage.installing
                        text: Math.round(page.stage.installProgress * 100) + "%"
                        font.family: Pe.mono
                        font.pixelSize: Pe.ui
                        color: Pe.her
                    }
                }
                Rectangle {
                    visible: page.stage.installing
                    Layout.fillWidth: true
                    implicitHeight: 6
                    radius: 3
                    color: Pe.line
                    Accessible.role: Accessible.ProgressBar
                    Accessible.name: qsTr("Installing the natural voice")
                    Rectangle {
                        width: parent.width * page.stage.installProgress
                        height: parent.height
                        radius: 3
                        gradient: Gradient {
                            orientation: Gradient.Horizontal
                            GradientStop { position: 0.0; color: Pe.mouthA }
                            GradientStop { position: 1.0; color: Pe.mouthB }
                        }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    Text {
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        maximumLineCount: 4
                        elide: Text.ElideRight
                        text: page.stage.installing ? page.stage.installStatus
                              : page.stage.installStatus !== "" && page.stage.installProgress < 1 ? page.stage.installStatus
                              : page.stage.engine === "missing"
                              ? qsTr("Heart, Bella, Emma and three more voices, speech recognition and the wake word — about 550 MB, all offline.")
                              : page.stage.engineMessage
                        font.family: Pe.sans
                        font.pixelSize: Pe.small
                        color: Pe.inkSoft
                    }
                    PeButton {
                        visible: page.stage.installing
                        compact: true
                        kind: "ghost"
                        text: qsTr("Cancel")
                        onClicked: page.stage.voice.cancelInstall()
                    }
                    PeButton {
                        visible: !page.stage.installing && page.stage.engine !== "error"
                        kind: "accent"
                        iconName: "download"
                        text: qsTr("Install")
                        onClicked: page.stage.voice.installEngine()
                    }
                    PeButton {
                        visible: !page.stage.installing && page.stage.engine === "error"
                        kind: "secondary"
                        iconName: "refresh"
                        text: qsTr("Re-check")
                        onClicked: page.stage.voice.refresh()
                    }
                }
            }
        }

        // cues to try (home)
        Text {
            textFormat: Text.PlainText
            visible: spotText.mode === "home"
            topPadding: 14
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: page.stage.canTalk || page.stage.armed ? qsTr("TRY SAYING") : qsTr("TRY ASKING")
            font.family: Pe.sans
            font.pixelSize: Pe.label
            font.weight: Font.Bold
            font.letterSpacing: Pe.labelTracking
            color: Pe.inkMute
        }
        Flow {
            id: cues
            visible: spotText.mode === "home"
            width: parent.width
            spacing: 8
            readonly property var items: page.width >= 820
                ? [qsTr("What's using my disk space?"), qsTr("Summarise my system info"), qsTr("List files in my home folder")]
                : [qsTr("What's using my disk space?"), qsTr("Summarise my system info")]
            // centre the flow's rows
            leftPadding: Math.max(0, (width - cueRow.implicitWidth) / 2)
            Row {
                id: cueRow
                spacing: 8
                Repeater {
                    model: cues.items
                    delegate: PeButton {
                        required property string modelData
                        compact: true
                        kind: "secondary"
                        text: "“" + modelData + "”"
                        tip: qsTr("Ask this now")
                        enabled: page.stage.canSend
                        onClicked: {
                            page.stage.send(modelData)
                            page.focusPrompt()
                        }
                    }
                }
            }
        }

        // live caption while you speak
        Item {
            visible: spotText.mode === "caption" || spotText.mode === "wake"
            width: parent.width
            height: captionText.implicitHeight
            Text {
                id: captionText
                textFormat: Text.PlainText
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: spotText.mode === "wake" ? qsTr("Yes?") : spotText.heldCaption
                font.family: Pe.sans
                font.pixelSize: page.height < 640 ? 24 : 30
                font.weight: Font.Light
                color: page.stage.partialText !== "" || spotText.newlyHeard !== "" || spotText.mode === "wake" ? Pe.ink : Pe.inkMute
                lineHeight: 1.2
                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }
            // The same words laid out the same way, never shown: where the last
            // line ends, so the caret sits after the last word, not at the end
            // of the widest line. (Its own copy: watching the shown caption's
            // lines would lay it out in several passes, and her face, sized
            // from the caption's height, would be resized in between.)
            Text {
                id: captionMeasure
                visible: false
                width: captionText.width
                text: captionText.text
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                horizontalAlignment: Text.AlignHCenter
                font: captionText.font
                lineHeight: captionText.lineHeight
                property real lastLineWidth: 0
                property real lastLineBottom: 0
                onLineLaidOut: (line) => {
                    if (line.isLast) {
                        lastLineWidth = line.implicitWidth
                        lastLineBottom = line.y + line.height
                    }
                }
            }
            // the caret that follows her transcription (2 Hz timer, not a 60 fps animation)
            Rectangle {
                id: caret
                visible: spotText.mode === "caption" && page.stage.partialText !== ""
                width: 2
                height: captionText.font.pixelSize * 1.05
                radius: 1
                x: Math.min(parent.width - 2, (captionText.width + captionMeasure.lastLineWidth) / 2 + 4)
                y: Math.max(0, (captionMeasure.lastLineBottom > 0 ? captionMeasure.lastLineBottom : captionText.height) - height - 4)
                color: Pe.cyan
                Timer {
                    interval: 520
                    repeat: true
                    running: page.stage.animate && !page.stage.frozen && caret.visible
                    onTriggered: caret.opacity = caret.opacity > 0.5 ? 0.15 : 1
                }
            }
        }
        Text {
            textFormat: Text.PlainText
            visible: spotText.mode === "caption"
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            text: qsTr("Pause when you're done — or press Ctrl+Space to send now")
            font.family: Pe.sans
            font.pixelSize: Pe.small
            color: Pe.inkMute
        }

        // your first question, while she thinks it over
        Column {
            visible: spotText.mode === "cue"
            width: parent.width
            spacing: 8
            Text {
                textFormat: Text.PlainText
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: page.stage.voice.voiceTurn && page.stage.lastHeard === spotText.heldUserLine
                      ? qsTr("YOU SAID") : qsTr("YOU")
                font.family: Pe.sans
                font.pixelSize: Pe.label
                font.weight: Font.Bold
                font.letterSpacing: Pe.labelTracking
                color: Pe.you
            }
            Text {
                textFormat: Text.PlainText
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                maximumLineCount: 4
                elide: Text.ElideRight
                text: spotText.heldUserLine
                font.family: Pe.sans
                font.pixelSize: 22
                font.weight: Font.Light
                color: Pe.ink
            }
            Text {
                textFormat: Text.PlainText
                width: parent.width
                topPadding: 6
                horizontalAlignment: Text.AlignHCenter
                text: page.stage.thinkHard ? qsTr("Thinking it through, carefully…") : qsTr("Thinking…")
                font.family: Pe.sans
                font.pixelSize: Pe.ui
                font.italic: true
                color: Pe.her
            }
        }
    }

    // ---- the question --------------------------------------------------------------------
    AskCard {
        id: askCard
        stage: page.stage
        visible: opacity > 0.01
        opacity: page.asking ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: Pe.tDock } }
        x: page.wide ? page.colX : 12
        width: page.wide ? Math.min(page.textW, 640) : page.width - 24
        y: page.height - page.bottomBlock - height - 14
        call: page.pending
        pendingCount: page.stage.pendingInfo.count
        faceInset: page.wide ? 0 : 60
        compact: !page.wide || page.height < 640
    }

    // ---- still listening after a spoken answer (no wake word needed) ----------------------
    Rectangle {
        id: followCue
        readonly property bool on: page.stage.capturing && page.stage.listenMode === "followup"
        visible: opacity > 0.01
        opacity: on ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: Pe.tQuick } }
        anchors.horizontalCenter: promptBar.horizontalCenter
        y: promptBar.y - height - 10
        height: 30
        width: followRow.implicitWidth + 28
        radius: 15
        color: Pe.card
        border.width: 1
        border.color: Pe.alpha(Pe.cyan, 0.45)
        Accessible.role: Accessible.Indicator
        Accessible.name: qsTr("Still listening, go ahead")
        Row {
            id: followRow
            anchors.centerIn: parent
            spacing: 10
            LevelBars {
                anchors.verticalCenter: parent.verticalCenter
                level: page.stage.drawnMicLevel
                maxHeight: 14
            }
            Text {
                textFormat: Text.PlainText
                anchors.verticalCenter: parent.verticalCenter
                text: page.stage.voiceState === "transcribing" ? qsTr("Catching that…") : qsTr("Still listening, go ahead")
                font.family: Pe.sans
                font.pixelSize: Pe.small
                font.weight: Font.DemiBold
                color: Pe.ink
            }
            Text {
                textFormat: Text.PlainText
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Esc stops")
                font.family: Pe.sans
                font.pixelSize: Pe.label
                color: Pe.inkMute
            }
        }
    }

    // ---- the prompt ----------------------------------------------------------------------
    PromptBar {
        id: promptBar
        stage: page.stage
        // centred under the text column, but never closer than `side` to an edge
        width: page.wide ? Math.min(page.textW + 96, page.width - 2 * page.side) : page.width - 24
        x: page.wide ? Math.round(Math.max(page.side, Math.min(page.width - page.side - width,
                                                             page.colX + page.textW / 2 - width / 2)))
                     : 12
        y: page.height - height - (page.wide ? 26 : 14) - (page.roomyHint ? 26 : 0)
    }
    Row {
        visible: page.roomyHint
        anchors.horizontalCenter: promptBar.horizontalCenter
        y: promptBar.y + promptBar.height + 10
        spacing: 16
        Accessible.ignored: true
        Repeater {
            model: [["Ctrl+Space", qsTr("talk")], ["Enter", qsTr("send")], ["Shift+Enter", qsTr("new line")],
                    ["Ctrl+K", qsTr("conversations")]]
            delegate: Row {
                id: hintItem
                required property var modelData
                spacing: 6
                Text {
                    textFormat: Text.PlainText
                    text: hintItem.modelData[0]
                    font.family: Pe.mono
                    font.pixelSize: 10
                    color: Pe.inkSoft
                }
                Text {
                    textFormat: Text.PlainText
                    text: hintItem.modelData[1]
                    font.family: Pe.sans
                    font.pixelSize: Pe.label
                    color: Pe.inkMute
                }
            }
        }
    }
}
