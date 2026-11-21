// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Shapes
import org.shadow.assistant

// Shadow Assistant — "Persona". One face of light (PersonaFace) that moves
// between three places: the spotlight (nothing to read yet, or while you
// speak), the corner (while you read), and beside the question card (when
// she needs your decision). Everything else is typography.
//
// State and intents live in StageModel (passed to every page explicitly);
// the voice ⇄ agent wiring lives in C++ (VoiceController.agent).
ApplicationWindow {
    id: window
    title: qsTr("Shadow Assistant")
    width: 1120
    height: 820
    minimumWidth: 720
    minimumHeight: 540
    color: Pe.bg
    // NxWindow's API, kept: window.toaster.show(text, kind)
    property alias toaster: whispers

    Assistant { id: agent }
    // With 'agent' set the controller does the voice ⇄ agent wiring itself
    // (transcripts sent, replies spoken, spoken yes/no, errors, Stop).
    VoiceController { id: voice; config: agent.config; agent: agent }
    StageModel {
        id: stage
        agent: agent
        voice: voice
        onScreen: window.visible && window.visibility !== Window.Minimized && window.visibility !== Window.Hidden
        appActive: Application.state === Qt.ApplicationActive
        // the level as her face draws it (one repaint per tick for the face and the level bars)
        faceLevel: persona.ticking || persona.sampling ? persona.levelNow : -1
        onWhispered: (text, kind) => whispers.show(text, kind)
        onPromptFocusRequested: Qt.callLater(stagePage.focusPrompt)
    }

    readonly property bool setupMode: stage.view === "stage" && !stage.keyConfigured

    // ---- life cycle --------------------------------------------------------------------
    // Started at login with --background: no window until "Shadow" is heard
    // or the assistant is opened.
    readonly property bool startHidden: Qt.application.arguments.indexOf("--background") >= 0
    visible: !startHidden
    // While the wake word is on, closing the window keeps the assistant
    // listening in the background; it quits once the wake word is off.
    readonly property bool keepListening: agent.config.wakeEnabled && voice.wakeAvailable
    onClosing: (close) => {
        if (window.keepListening) {
            close.accepted = false
            window.hide()
        }
    }
    function present() {
        // On Wayland a client cannot un-minimise or raise its own window
        // (KWin ignores both without an activation token, and Qt is never told
        // the window was minimised): a window that is not in front is mapped
        // anew, which brings it back and activates it.
        if (window.visible && !window.active && Qt.platform.pluginName.indexOf("wayland") === 0)
            window.hide()
        if (!window.visible)
            window.show()
        if (window.visibility === Window.Minimized)
            window.showNormal()
        window.raise()
        window.requestActivate()
    }
    // Opening the assistant again (dock, launcher) while it runs in the
    // background brings its window back (single instance: shadowApp).
    Connections {
        target: shadowApp // qmllint disable unqualified
        function onActivationRequested(args) { window.present() }
    }

    // ---- voice: presentation only (the wiring lives in VoiceController) -------------------
    readonly property string listeningWhisper: qsTr("Shadow is listening…")
    Connections {
        target: voice
        function onTranscriptReady(text) {
            window.commandTaken()
            stage.view = "stage"
            stage.conversationsOpen = false
        }
        function onHeard(text) {
            window.commandTaken()
            whispers.show(qsTr("Heard: %1").arg(text), "info")
        }
        function onWakeDetected() {
            stage.view = "stage"
            stage.conversationsOpen = false
            window.present()
            // "Shadow, <command>" in one breath delivers the command right
            // after the wake: she answers it, no "Yes?". Only a bare "Shadow"
            // opens a listen — then she looks up and the whisper says so.
            wakeCue.restart()
        }
        function onUnavailable(reason) { whispers.show(reason, "warning") }
    }
    function commandTaken() {
        wakeCue.stop()
        stage.clearWake()
        whispers.dismiss(window.listeningWhisper)
    }
    Timer {
        id: wakeCue
        interval: 160
        onTriggered: {
            if (voice.listenMode !== "wake" && !voice.capturing)
                return
            whispers.show(window.listeningWhisper, "info")
            stage.flashWake()
        }
    }

    Component.onCompleted: {
        if (agent.conversations.count > 0)
            agent.selectConversation(agent.conversations.data(agent.conversations.index(0, 0), 257 /* id */))
        else
            agent.newConversation()
        // Env-gated screenshot/test hooks (no effect in normal use):
        // SHADOW_ASSISTANT_TEST_VIEW = settings[-voice|-claude|-behaviour|-tools|-activity]
        // | history | conversations | stage | chat
        const v = agent.testView
        if (v === "history") {
            stage.view = "history"
        } else if (v === "settings" || v.indexOf("settings-") === 0) {
            stage.openSettings(v === "settings" ? "voice" : v.slice(9))
        } else if (v === "conversations") {
            stage.conversationsOpen = true
        }
        if (agent.testPrompt !== "")
            testPromptTimer.start()
    }
    Timer {
        id: testPromptTimer
        interval: 400
        onTriggered: {
            voice.beginVoiceTurn()      // treated as a spoken turn
            if (agent.canSend)
                agent.sendMessage(agent.testPrompt)
        }
    }

    // ---- keyboard ---------------------------------------------------------------------------
    // The question card is in front of you (not behind Settings, History or the list).
    readonly property bool questionShown: stagePage.asking && stagePage.visible && !stage.conversationsOpen
    Shortcut {
        sequence: "Ctrl+Space"
        onActivated: stage.talk()
    }
    Shortcut {
        sequence: "Esc"
        onActivated: {
            if (stage.capturing)
                stage.cancelTalk()
            else if (stage.conversationsOpen)
                stage.conversationsOpen = false
            else if (stagePage.asking && stage.view === "stage")
                stage.confirm(stagePage.pending.id, false)
            else if (stage.working || stage.speaking)
                stage.stop()
            else if (stage.view !== "stage")
                stage.view = "stage"
        }
    }
    Shortcut { sequence: "Ctrl+N"; onActivated: stage.newConversation() }
    Shortcut { sequence: "Ctrl+K"; onActivated: stage.conversationsOpen = !stage.conversationsOpen }
    Shortcut { sequence: "Ctrl+,"; onActivated: stage.view = stage.view === "settings" ? "stage" : "settings" }
    Shortcut { sequence: "Ctrl+H"; onActivated: stage.view = stage.view === "history" ? "stage" : "history" }
    Shortcut { sequence: "Ctrl+M"; onActivated: stage.setMuted(!stage.micMuted) }
    Shortcut { sequence: "Ctrl+Shift+W"; onActivated: stage.setWake(!stage.wakeEnabled) }
    Shortcut { sequence: "Ctrl+T"; onActivated: stage.setThinkHard(!stage.thinkHard) }
    Shortcut { sequence: "Ctrl+L"; onActivated: stage.focusPrompt() }
    // Run / Deny only answer a question you can see.
    Shortcut {
        sequence: "Alt+Y"
        enabled: window.questionShown
        onActivated: stage.confirm(stagePage.pending.id, true)
    }
    Shortcut {
        sequence: "Alt+N"
        enabled: window.questionShown
        onActivated: stage.confirm(stagePage.pending.id, false)
    }
    // F2 renames the open conversation; inside the list, the list's own F2
    // renames the highlighted row.
    Shortcut {
        sequence: "F2"
        enabled: stage.keyConfigured && !stage.conversationsOpen
        onActivated: {
            stage.conversationsOpen = true
            convSheet.renameOpen(stage.activeId)
        }
    }

    // The keyboard never stays on something you cannot see or use (a hidden
    // button would still take Space and Enter): it goes back to the prompt —
    // or into the open list of conversations. And whatever Tab reaches is
    // scrolled into view.
    onActiveFocusItemChanged: {
        Qt.callLater(window.keepFocusUseful)
        Qt.callLater(window.revealFocus)
    }
    Connections {
        target: window.activeFocusItem
        ignoreUnknownSignals: true
        function onVisibleChanged() { Qt.callLater(window.keepFocusUseful) }
        function onEnabledChanged() { Qt.callLater(window.keepFocusUseful) }
    }
    function keepFocusUseful() {
        if (!window.active)
            return
        const it = window.activeFocusItem
        if (it && it !== window.contentItem && it.visible && it.enabled)
            return
        if (stage.conversationsOpen)
            convSheet.focusFilter()
        else if (stagePage.visible)
            stagePage.focusPrompt()
    }
    function revealFocus() {
        const it = window.activeFocusItem
        if (!it)
            return
        for (let p = it.parent; p; p = p.parent) {
            // a Flickable (or ListView) between the item and the window
            const f = p as Flickable
            if (f)
                window.scrollIntoView(f, it)
        }
    }
    function scrollIntoView(f: Flickable, it: Item) {
        if (!f || !f.visible)
            return
        const r = f.contentItem.mapFromItem(it, 0, 0, it.width, it.height)
        const m = 16
        if (f.contentHeight + f.topMargin + f.bottomMargin > f.height + 1) {
            const lo = f.originY - f.topMargin
            const hi = Math.max(lo, f.originY + f.contentHeight + f.bottomMargin - f.height)
            let y = f.contentY
            if (r.y < y + m)
                y = r.y - m
            else if (r.y + r.height > y + f.height - m)
                y = Math.min(r.y - m, r.y + r.height - f.height + m)
            y = Math.max(lo, Math.min(hi, y))
            if (Math.abs(y - f.contentY) > 0.5) {
                f.contentY = y
                // the conversation stops following her words while you look elsewhere
                if (f === stagePage.scriptView)
                    stagePage.stopFollowing()
            }
        }
        if (f.contentWidth > f.width + 1) {
            const lo = f.originX
            const hi = Math.max(lo, f.originX + f.contentWidth - f.width)
            let x = f.contentX
            if (r.x < x + m)
                x = r.x - m
            else if (r.x + r.width > x + f.width - m)
                x = Math.min(r.x - m, r.x + r.width - f.width + m)
            x = Math.max(lo, Math.min(hi, x))
            if (Math.abs(x - f.contentX) > 0.5)
                f.contentX = x
        }
    }

    // ---- the room ------------------------------------------------------------------------------
    // A soft stage light that follows her (radial gradient: every renderer).
    Shape {
        id: stageLight
        anchors.fill: parent
        layer.enabled: true
        readonly property real cx: persona.x + persona.width / 2
        readonly property real cy: persona.y + persona.height * 0.48
        readonly property real r: Math.max(persona.width * 2.1, 300)
        ShapePath {
            strokeWidth: -1
            fillGradient: RadialGradient {
                centerX: stageLight.cx; centerY: stageLight.cy
                focalX: stageLight.cx; focalY: stageLight.cy
                centerRadius: stageLight.r; focalRadius: 0
                GradientStop { position: 0.0; color: Pe.dark ? Qt.rgba(0.42, 0.30, 0.84, 0.16) : Qt.rgba(1, 1, 1, 0.95) }
                GradientStop { position: 0.4; color: Pe.dark ? Qt.rgba(0.17, 0.36, 0.79, 0.06) : Qt.rgba(0.97, 0.96, 1, 0.55) }
                GradientStop { position: 1.0; color: Pe.dark ? Qt.rgba(0.17, 0.36, 0.79, 0) : Qt.rgba(0.97, 0.96, 1, 0) }
            }
            PathMove { x: 0; y: 0 }
            PathLine { x: stageLight.width; y: 0 }
            PathLine { x: stageLight.width; y: stageLight.height }
            PathLine { x: 0; y: stageLight.height }
            PathLine { x: 0; y: 0 }
        }
    }

    StagePage {
        id: stagePage
        stage: stage
        anchors.fill: parent
        visible: stage.view === "stage" && stage.keyConfigured
    }
    AwakeningPage {
        id: awakening
        stage: stage
        anchors.fill: parent
        visible: window.setupMode
    }
    BackstagePage {
        id: backstage
        stage: stage
        anchors.fill: parent
        visible: stage.view === "settings"
        section: stage.settingsSection
    }
    HistoryPage {
        id: historyPage
        stage: stage
        anchors.fill: parent
        visible: stage.view === "history"
    }

    Masthead {
        id: masthead
        stage: stage
        width: parent.width
        mode: window.setupMode ? "setup" : stage.view
        readonly property bool faceInCorner: !window.setupMode && (stage.view !== "stage" || !stagePage.spotlight)
        showWordmark: !faceInCorner
        leftInset: faceInCorner ? persona.dockRight + 14 : 24
        whispering: whispers.count > 0
        onTitleClicked: stage.conversationsOpen = !stage.conversationsOpen
        onBackClicked: stage.view = "stage"
    }

    // ---- her --------------------------------------------------------------------------------
    // Window resizes move her at once; changes of place glide.
    property bool resizing: false
    onWidthChanged: resizeSettle.restart()
    onHeightChanged: resizeSettle.restart()
    Timer {
        id: resizeSettle
        interval: 220
        onRunningChanged: window.resizing = running
    }

    PersonaFace {
        id: persona
        readonly property real dockRight: window.width >= 900 ? 122 : 58
        readonly property rect target: window.setupMode ? awakening.faceRect
            : stage.view === "stage" ? stagePage.faceRect
            : (window.width >= 900 ? Qt.rect(18, 2, 104, 104) : Qt.rect(12, 9, 46, 46))
        readonly property bool glide: !stage.frozen && !window.resizing
        x: target.x
        y: target.y
        width: target.width
        height: target.height
        // setup on a small window: she scrolls away with the page, fading
        // before she reaches the top line
        opacity: window.setupMode ? awakening.faceOpacity : 1
        Behavior on x { enabled: persona.glide; NumberAnimation { duration: Pe.tDock; easing.type: Easing.InOutCubic } }
        Behavior on y { enabled: persona.glide; NumberAnimation { duration: Pe.tDock; easing.type: Easing.InOutCubic } }
        Behavior on width { enabled: persona.glide; NumberAnimation { duration: Pe.tDock; easing.type: Easing.InOutCubic } }
        Behavior on height { enabled: persona.glide; NumberAnimation { duration: Pe.tDock; easing.type: Easing.InOutCubic } }
        mood: stage.mood
        level: stage.level
        voiceless: stage.voiceless
        installProgress: stage.installing ? stage.installProgress : -1
        // calm moods also rest while the app is not in front
        animate: stage.animate && (stage.appActive || (stage.mood !== "armed" && stage.mood !== "rest"))
        // reduce motion: the face holds still, but her mouth and ears still
        // follow the voice — a few times a second, not with every level
        sampleLevels: stage.onScreen && Pe.reduceMotion
        frozenClock: stage.frozen ? 2.0 : -1
        burst: stage.faceBurst
        Accessible.name: qsTr("Shadow: %1").arg(stage.cueText)
        Accessible.description: stage.canTalk ? qsTr("Tap to talk") : ""
        HoverHandler { cursorShape: stage.canTalk ? Qt.PointingHandCursor : Qt.ArrowCursor }
        TapHandler {
            enabled: stage.canTalk && stage.keyConfigured
            onTapped: stage.talk()
        }
    }

    // While she speaks, her voice runs from her mouth across the top of the stage.
    VoiceThread {
        id: voiceThread
        readonly property bool docked: stage.view === "stage" && stage.keyConfigured && !stagePage.spotlight && !stagePage.asking
        readonly property bool wideStage: window.width >= 900
        x: wideStage ? persona.x + persona.width * 0.6 : 64
        y: wideStage ? persona.y + persona.height * 0.652 - height / 2 : 50
        width: Math.max(0, window.width - x - 24)
        height: wideStage ? 22 : 16
        visible: docked && stage.showsSpeaking
        running: visible
        // in step with her face while it ticks (one repaint for both)
        sharedClock: persona.ticking ? persona.clock : -1
        level: (persona.ticking || persona.sampling) && stage.mood === "speaking" ? persona.levelNow
             : stage.mood === "speaking" ? stage.level : stage.speechLevel
        animate: stage.animate
        frozenClock: stage.frozen ? 2.0 : -1
    }

    ConversationSheet {
        id: convSheet
        stage: stage
        anchors.fill: parent
        open: stage.conversationsOpen
        anchorX: masthead.leftInset - 10
        anchorY: 58
        onCloseRequested: stage.conversationsOpen = false
    }

    // Whispers live in the top line and never cover the dialogue: in its
    // quiet middle on wide windows, in place of the title and the cue line
    // on narrower ones (Masthead makes room). One at a time.
    WhisperToaster {
        id: whispers
        anchors.fill: parent
        bandHeight: masthead.height
        maxWidth: 460
        laneLeft: masthead.laneLeft
        laneRight: masthead.laneRight
        sticky: stage.frozen
    }
}
