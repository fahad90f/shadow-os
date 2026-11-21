// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import org.shadow.assistant
import "markdown.js" as Md

// The one place the UI reads state from and sends intents to. Every value
// comes from the real backend — the Assistant (agent) and VoiceController v2
// (voice) — and every component gets this object passed in explicitly.
//
// The voice ⇄ agent wiring lives in C++ (voice.agent = agent): transcripts are
// sent, replies spoken, confirmations asked aloud and voice turns ended there.
// The UI only presents and sends the user's own intents.
Item {
    id: sm
    visible: false

    required property Assistant agent
    required property VoiceController voice

    // ---- window state (Main.qml owns the window; pages read and set this) -----------
    property string view: "stage"             // stage | settings | history
    property bool conversationsOpen: false
    property string settingsSection: "voice"  // voice | models | behaviour | tools | activity
    property bool onScreen: true              // window shown and not minimised
    property bool appActive: true             // the app has the focus
    // Screenshots (SHADOW_SCREENSHOT) draw one fixed instant of every animation.
    readonly property bool frozen: agent.testFrozen
    // Animations run only while someone can see them, and never with reduce motion.
    readonly property bool animate: onScreen && !Pe.reduceMotion

    signal whispered(string text, string kind)
    signal promptFocusRequested()

    function whisper(text: string, kind: string) {
        whispered(text, kind)
    }
    function openSettings(section: string) {
        settingsSection = section === "claude" ? "models" : section
        view = "settings"
        conversationsOpen = false
    }
    function showStage() {
        view = "stage"
    }
    function focusPrompt() {
        view = "stage"
        conversationsOpen = false
        promptFocusRequested()
    }
    // the keyboard goes back to the prompt when the list of conversations closes
    onConversationsOpenChanged: {
        if (!conversationsOpen && view === "stage")
            promptFocusRequested()
    }

    // ---- agent -----------------------------------------------------------------------
    readonly property AssistantConfig cfg: agent.config
    readonly property MessageModel messages: agent.messages
    readonly property ConversationStore conversations: agent.conversations
    readonly property string activeId: agent.activeId
    readonly property string phase: agent.phase
    readonly property bool busy: agent.busy
    // A question waiting for a decision is read from the rows themselves (a
    // conversation can be switched to, or restored, with one still open), so
    // the card and Stop always show for it, and nothing is "sendable" then.
    readonly property bool hasPending: pendingInfo.count > 0
    readonly property bool canSend: agent.canSend && !hasPending
    readonly property bool working: phase === "thinking" || phase === "streaming" || busy
    readonly property bool thinkHard: agent.thinkHard
    readonly property bool keyConfigured: cfg.apiKeyConfigured
    readonly property var toolLog: agent.toolLog
    readonly property bool localMode: cfg.localMode
    readonly property string role: cfg.role
    readonly property string autonomy: cfg.autonomy

    // Message rows (roles of MessageModel).
    readonly property int kindRole: 257
    readonly property int contentRole: 258
    readonly property int streamingRole: 259
    readonly property int toolCallsRole: 260

    function rowCount(): int {
        return messages.count
    }
    function kindAt(i: int): string {
        if (i < 0 || i >= messages.count)
            return ""
        return messages.data(messages.index(i, 0), kindRole)
    }
    function contentAt(i: int): string {
        if (i < 0 || i >= messages.count)
            return ""
        return messages.data(messages.index(i, 0), contentRole)
    }
    function streamingAt(i: int): bool {
        if (i < 0 || i >= messages.count)
            return false
        return messages.data(messages.index(i, 0), streamingRole) === true
    }
    function callsAt(i: int): var {
        if (i < 0 || i >= messages.count)
            return []
        return callsOf(messages.data(messages.index(i, 0), toolCallsRole))
    }
    // Tool calls arrive as a JS array or a sequence wrapper.
    function callsOf(v: var): var {
        if (v === undefined || v === null)
            return []
        if (Array.isArray(v))
            return v
        const a = []
        if (typeof v.length === "number")
            for (let i = 0; i < v.length; ++i)
                a.push(v[i])
        return a
    }
    // Rows the script does not draw: tool results and empty finished replies.
    function hiddenAt(i: int): bool {
        const k = kindAt(i)
        if (k === "tool")
            return true
        return k === "assistant" && contentAt(i) === "" && !streamingAt(i) && callsAt(i).length === 0
    }

    // structRevision changes when rows come or go or a row changes anything but
    // its text (a streamed delta touches only the text of the growing row).
    property int structRevision: 0
    Connections {
        target: sm.messages
        function onRowsInserted() { sm.structRevision++; sm.recomputeReading() }
        function onRowsRemoved() { sm.structRevision++; sm.recomputeReading() }
        function onModelReset() { sm.structRevision++; sm.recomputeReading() }
        function onDataChanged(topLeft, bottomRight, roles) {
            if (roles.length === 1 && roles[0] === sm.contentRole) {
                if (!sm.hasReading && sm.contentAt(topLeft.row) !== "")
                    sm.hasReading = true
                return
            }
            sm.structRevision++
            sm.recomputeReading()
        }
    }

    // Is there anything to read yet? (Otherwise she stays in the spotlight.)
    property bool hasReading: false
    function recomputeReading() {
        const n = messages.count
        let reading = n > 2
        for (let i = 0; i < n && !reading; ++i) {
            const k = kindAt(i)
            reading = k === "error" || k === "tool" || (k === "assistant" && contentAt(i) !== "")
        }
        hasReading = reading
    }
    Component.onCompleted: recomputeReading()

    // The first tool call waiting for a decision, and how many wait.
    readonly property var pendingInfo: {
        if (structRevision < 0)  // a dependency (a bare read could be compiled away)
            return { call: null, count: 0 }
        let first = null, n = 0
        const count = messages.count
        for (let i = 0; i < count; ++i) {
            if (kindAt(i) !== "assistant")
                continue
            const cs = callsAt(i)
            for (let k = 0; k < cs.length; ++k) {
                if (cs[k].status === "pending") {
                    if (!first)
                        first = cs[k]
                    ++n
                }
            }
        }
        return { call: first, count: n }
    }
    readonly property string firstUserLine: {
        if (structRevision < 0)  // a dependency (a bare read could be compiled away)
            return ""
        return kindAt(0) === "user" ? contentAt(0) : ""
    }
    // What you said last (the current turn).
    readonly property string lastUserLine: {
        if (structRevision < 0)  // a dependency (a bare read could be compiled away)
            return ""
        for (let i = messages.count - 1; i >= 0; --i)
            if (kindAt(i) === "user")
                return contentAt(i)
        return ""
    }
    // The last row of the conversation that is drawn (for "Try again").
    readonly property int lastShownRow: {
        if (structRevision < 0)  // a dependency (a bare read could be compiled away)
            return -1
        for (let i = messages.count - 1; i >= 0; --i)
            if (!hiddenAt(i))
                return i
        return -1
    }

    // Reactive conversation title (ConversationStore.titleOf() alone never
    // re-evaluates inside a binding).
    property int convRevision: 0
    Connections {
        target: sm.conversations
        function onDataChanged() { sm.convRevision++ }
        function onModelReset() { sm.convRevision++ }
        function onRowsInserted() { sm.convRevision++ }
        function onRowsRemoved() { sm.convRevision++ }
    }
    readonly property string activeTitle: {
        if (convRevision < 0)  // a dependency (a bare read could be compiled away)
            return ""
        if (activeId === "")
            return qsTr("New conversation")
        const i = conversations.indexOfId(activeId)
        if (i < 0)
            return qsTr("New conversation")
        return conversations.data(conversations.index(i, 0), 258 /* title */) || qsTr("Conversation")
    }
    // The title on one line (a first message with line breaks names it).
    readonly property string activeTitleLine: activeTitle.replace(/\s+/g, " ").trim()
    function oneLine(text: string): string {
        return String(text).replace(/\s+/g, " ").trim()
    }

    // ---- voice (VoiceController v2) -----------------------------------------------------
    readonly property string voiceState: voice.state
    readonly property bool capturing: voice.capturing
    readonly property bool speaking: voice.speaking
    readonly property string listenMode: voice.listenMode
    readonly property real micLevel: voice.micLevel
    readonly property real speechLevel: voice.speechLevel
    // The level as her face draws it (sampled on its clock; -1 when it does
    // not tick): the level bars follow the same samples, so the face and the
    // bars change in one repaint instead of two unsynchronised streams.
    property real faceLevel: -1
    readonly property real drawnMicLevel: faceLevel >= 0 && mood === "listening" ? faceLevel : micLevel
    readonly property string partialText: voice.partialText
    readonly property string lastHeard: voice.lastHeard
    readonly property string engine: voice.engine
    readonly property string engineMessage: voice.engineMessage
    readonly property bool installing: voice.installing
    readonly property real installProgress: voice.installProgress
    readonly property string installStatus: voice.installStatus
    readonly property bool armed: voice.armed
    readonly property bool neural: engine === "neural"
    // No voice at all, or not yet: her mouth is a dotted line.
    readonly property bool voiceless: engine === "missing" || engine === "installing" || engine === "error"
    readonly property bool micMuted: cfg.micMuted
    readonly property bool wakeEnabled: cfg.wakeEnabled
    readonly property bool wakeOn: wakeEnabled && !micMuted && neural
    // Push-to-talk can start (VoiceController refuses with a reason otherwise).
    readonly property bool canTalk: !micMuted && voice.sttAvailable && voice.captureAvailable
    readonly property string talkBlocker: micMuted ? qsTr("The microphone is off (Ctrl+M turns it on)")
        : installing ? qsTr("The natural voice is installing")
        : engine === "starting" ? qsTr("The voice engine is starting…")
        : !voice.sttAvailable ? qsTr("Talking needs speech recognition (Settings › Voice)")
        : !voice.captureAvailable ? qsTr("No microphone is available")
        : ""
    readonly property var voices: voice.voices
    readonly property string voiceName: {
        const id = cfg.voice
        for (let i = 0; i < voices.length; ++i)
            if (voices[i].id === id)
                return voices[i].name
        return id
    }

    // ---- the sentence being spoken, located in the conversation ----------------------------
    // voice.spokenSentence is the sentence as the controller sent it; it is
    // looked for in the message being read aloud, else in her rows of the
    // current turn (after your last line), in order. Not found: no highlight.
    property int spokenRow: -1
    property int readAloudRow: -1
    property int lastSpokenRow: -1
    property bool readAloudStarted: false
    readonly property string spokenSentence: voice.spokenSentence
    onSpokenSentenceChanged: resolveSpoken()
    function resolveSpoken() {
        const s = spokenSentence
        if (s === "") {
            spokenRow = -1
            return
        }
        const rows = []
        if (readAloudRow >= 0 && readAloudRow < messages.count) {
            rows.push(readAloudRow)
        } else {
            for (let i = messages.count - 1; i >= 0; --i) {
                const k = kindAt(i)
                if (k === "user")
                    break
                if (k === "assistant")
                    rows.unshift(i)
            }
        }
        let start = rows.indexOf(lastSpokenRow)
        if (start < 0)
            start = 0
        for (let j = 0; j < rows.length; ++j) {
            const r = rows[(start + j) % rows.length]
            if (Md.containsSentence(contentAt(r), s)) {
                spokenRow = r
                lastSpokenRow = r
                return
            }
        }
        spokenRow = -1
    }
    Connections {
        target: sm.voice
        function onStateChanged() {
            if (sm.voice.speaking) {
                sm.readAloudStarted = sm.readAloudRow >= 0
            } else if (sm.readAloudStarted) {
                sm.readAloudRow = -1
                sm.readAloudStarted = false
            }
        }
        function onVoiceTurnChanged() {
            if (sm.voice.voiceTurn) {
                sm.readAloudRow = -1
                sm.lastSpokenRow = -1
            }
        }
    }

    // ---- what you said to a spoken yes/no that decided nothing ----------------------------
    // (shown on the question card: "I heard “maybe” — use the buttons")
    property string unclearAnswer: ""
    property string yesNoHeardBefore: ""
    property bool yesNoDecided: false
    property string previousListenMode: ""
    onListenModeChanged: {
        if (listenMode === "yesno") {
            yesNoHeardBefore = voice.lastHeard
            yesNoDecided = false
            unclearAnswer = ""
        } else if (previousListenMode === "yesno") {
            Qt.callLater(checkYesNoAnswer)  // the transcript and the decision follow in the same event
        }
        previousListenMode = listenMode
    }
    function checkYesNoAnswer() {
        const heard = voice.lastHeard
        if (!yesNoDecided && hasPending && heard !== "" && heard !== yesNoHeardBefore)
            unclearAnswer = heard
    }
    Connections {
        target: sm.voice
        function onYesNoResult() { sm.yesNoDecided = true }
    }
    onHasPendingChanged: {
        if (!hasPending)
            unclearAnswer = ""
    }

    // ---- errors -------------------------------------------------------------------------
    // The last failure of the conversation (her face looks concerned until
    // the next request). An undo that fails is not one: it only whispers.
    property string lastError: ""
    property bool undoing: false
    Connections {
        target: sm.agent
        function onErrorMessage(text) {
            if (!sm.undoing)
                sm.lastError = text
            sm.whisper(text.split("\n")[0], "danger")
        }
        function onPhaseChanged() {
            if (sm.agent.phase === "thinking")
                sm.lastError = ""
        }
    }
    readonly property bool recentError: lastError !== "" && !working && !hasPending

    // ---- the wake word heard: a flash of light ------------------------------------------------
    property bool wakeFlash: false
    property real burst: 1.0
    // the ring of light as the face draws it (a forced "wake" shows it mid-bloom)
    readonly property real faceBurst: testOrb === "wake" ? 0.32 : burst
    function flashWake() {
        wakeFlash = true
        if (animate && !frozen)
            wakeBurst.restart()
        else
            burst = frozen ? 0.32 : 1.0
        wakeEnd.restart()
    }
    // The command was taken: no more "Yes?" (she thinks, answers or fails now).
    function clearWake() {
        if (!wakeFlash)
            return
        wakeEnd.stop()
        wakeBurst.stop()
        wakeFlash = false
        burst = 1.0
    }
    NumberAnimation { id: wakeBurst; target: sm; property: "burst"; from: 0; to: 1; duration: 760; easing.type: Easing.OutQuart }
    Timer {
        id: wakeEnd
        interval: sm.frozen ? 60000 : 900
        onTriggered: {
            sm.wakeFlash = false
            sm.burst = 1.0
        }
    }

    // ---- presence: her mood -----------------------------------------------------------------
    // SHADOW_ASSISTANT_TEST_ORB (screenshots) forces a presence:
    // idle → armed (rest when the wake word cannot listen), wake → startled,
    // listening / thinking / speaking as they are; any other value is passed
    // to the face (an unknown mood draws as rest), never a warning.
    readonly property string testOrb: agent.testOrb
    readonly property string mood: {
        if (testOrb !== "") {
            if (testOrb === "idle")
                return armed || wakeOn ? "armed" : "rest"
            if (testOrb === "wake")
                return "startled"
            return testOrb
        }
        if (!keyConfigured)
            return "dormant"
        if (wakeFlash)
            return "startled"
        if (capturing)
            return "listening"
        if (speaking)
            return "speaking"
        if (hasPending)
            return "asking"
        if (phase === "thinking")
            return "thinking"
        if (phase === "streaming")
            return "writing"
        if (recentError)
            return "concerned"
        if (micMuted)
            return "sleeping"
        if (armed)
            return "armed"
        return "rest"
    }
    // A forced listening/speaking presence shows a live-looking level.
    readonly property real level: mood === "listening" ? (testOrb !== "" ? Math.max(micLevel, 0.6) : micLevel)
        : mood === "speaking" ? (testOrb !== "" ? Math.max(speechLevel, 0.6) : speechLevel) : 0
    // What the stage shows around her follows the forced presence too
    // (after "Shadow" she listens, so a forced wake shows the listening bar).
    readonly property bool forcedListen: testOrb === "listening" || testOrb === "wake"
    readonly property bool showsListening: capturing || mood === "listening" || forcedListen
    readonly property bool showsWake: wakeFlash || mood === "startled"
    readonly property bool showsSpeaking: speaking || mood === "speaking"
    // "Done" can send what was heard so far
    readonly property bool canFinishTalk: voiceState === "listening" || forcedListen

    // ---- what she is doing, in words ------------------------------------------------------------
    readonly property string modelName: {
        if (nvidia)
            return cfg.teamMode ? qsTr("Model team") : shortModel(agent.activeModel)
        const id = localMode ? cfg.localModel : (thinkHard ? cfg.plannerModel : cfg.brainModel)
        const m = /^claude-([a-z]+)-(\d+)-(\d+)/.exec(id)
        return m ? "Claude " + m[1].charAt(0).toUpperCase() + m[1].slice(1) + " " + m[2] + "." + m[3] : id
    }

    // ---- the model team (NVIDIA): several models work on a request at the same time -------------
    readonly property bool nvidia: cfg.provider === "nvidia" && !localMode
    // the service said no to the key in use (when it was tried, or on a request)
    readonly property bool keyRejected: nvidia && agent.catalog.keyState === "rejected"
    // What the service is called in a sentence.
    readonly property string serviceName: localMode ? qsTr("The local model")
        : cfg.provider === "anthropic" ? qsTr("Claude") : qsTr("NVIDIA")
    readonly property var team: agent.team            // [{id, name, publisher, role, state, seconds}], lead first
    readonly property bool teamGathering: agent.teamGathering
    readonly property string teamKind: agent.teamKind
    readonly property var teamCounts: {
        let advisors = 0, done = 0
        let lead = ""
        for (let i = 0; i < team.length; ++i) {
            if (team[i].role === "lead") {
                lead = team[i].name
            } else {
                ++advisors
                if (team[i].state === "done")
                    ++done
            }
        }
        return { advisors: advisors, done: done, lead: lead }
    }
    // shown while the team works on the request in front of the user
    readonly property bool teamVisible: nvidia && team.length > 1 && (phase === "thinking" || phase === "streaming")
                                        && !hasPending
    readonly property string teamText: {
        const c = teamCounts
        if (teamGathering)
            return qsTr("%1 models are working on this · %2 of %3 ready").arg(team.length).arg(c.done).arg(c.advisors)
        if (c.done > 0)
            return qsTr("%1 answers, with the work of %2 others").arg(c.lead).arg(c.done)
        return qsTr("%1 answers").arg(c.lead)
    }
    function shortModel(id: string): string {
        const i = id.lastIndexOf("/")
        return i >= 0 ? id.slice(i + 1) : id
    }
    Connections {
        target: sm.agent.catalog
        // The first-start page is gone the moment a key is saved; the outcome
        // of trying the key still has to reach the user.
        function onKeyChecked(ok: bool, message: string) {
            if (sm.view !== "settings")
                sm.whisper(message, ok ? "success" : "warning")
        }
    }
    readonly property string listeningText: listenMode === "followup" ? qsTr("Still listening, go ahead")
        : listenMode === "yesno" ? qsTr("Say yes or no")
        : voiceState === "transcribing" ? qsTr("Catching that…")
        : qsTr("Listening…")
    readonly property string cueText: {
        switch (mood) {
        case "dormant": return qsTr("Waiting for an API key")
        case "startled": return qsTr("Yes?")
        case "listening": return listeningText
        case "speaking": return voice.resolvedTtsEngine === "espeak" ? qsTr("Speaking · Basic voice")
                                                                     : qsTr("Speaking · %1").arg(voiceName)
        case "asking": return qsTr("Waiting for your answer")
        case "thinking": return teamVisible && teamGathering ? qsTr("%1 models working together…").arg(team.length)
                              : thinkHard ? qsTr("Thinking hard · %1").arg(modelName) : qsTr("Thinking…")
        case "writing": return qsTr("Writing…")
        case "concerned": return qsTr("Something went wrong")
        case "sleeping": return qsTr("Microphone off · %1").arg(modelName)
        default: return keyRejected ? qsTr("The key was not accepted · see Settings") : qsTr("%1 · ready").arg(modelName)
        }
    }
    readonly property color cueTone: {
        switch (mood) {
        case "listening": case "startled": return Pe.cyan
        case "speaking": return Pe.rose
        case "asking": case "thinking": case "writing": return Pe.her
        case "concerned": return Pe.alarm
        case "sleeping": case "dormant": return Pe.inkMute
        default: return keyRejected ? Pe.caution : Pe.ok
        }
    }
    readonly property var cueTags: {
        const t = []
        if (localMode)
            t.push({ text: qsTr("OFFLINE"), tone: Pe.ok })
        if (role === "pentest")
            t.push({ text: qsTr("PENTEST"), tone: Pe.caution })
        return t
    }
    readonly property string wakeText: micMuted ? qsTr("Microphone off")
        : installing ? qsTr("Voice installing · %1%").arg(Math.round(installProgress * 100))
        : engine === "starting" ? qsTr("Voice starting…")
        : engine === "error" ? qsTr("Voice engine stopped")
        : voiceless ? qsTr("No voice engine")
        : showsListening ? qsTr("Listening")
        : armed ? qsTr("Listening for “Shadow”")
        : wakeOn ? qsTr("Wake word on")
        : wakeEnabled ? qsTr("Wake word unavailable")
        : qsTr("Wake word off")
    readonly property color wakeTone: micMuted ? Pe.alarm : armed || showsListening ? Pe.cyan
        : wakeOn ? Pe.alpha(Pe.cyan, 0.6)
        : voiceless || (wakeEnabled && !neural) ? Pe.caution : Pe.inkMute
    readonly property string greeting: {
        const h = frozen ? 9 : new Date().getHours()
        return h < 5 ? qsTr("Still up?") : h < 12 ? qsTr("Good morning.") : h < 18 ? qsTr("Good afternoon.")
             : qsTr("Good evening.")
    }

    // ---- words for tool calls ------------------------------------------------------------------
    readonly property var pastTenses: ({
        "Run a command": qsTr("Ran a command"), "Read a file": qsTr("Read a file"),
        "Write a file": qsTr("Wrote a file"), "Search files": qsTr("Searched files"),
        "Open an application": qsTr("Opened an application"),
        "Read system information": qsTr("Read system information"),
        "Open a URL in the browser": qsTr("Opened a link"), "Search the web": qsTr("Searched the web"),
        "Create a folder": qsTr("Created a folder"), "Move / rename": qsTr("Moved"), "Copy": qsTr("Copied"),
        "Delete": qsTr("Deleted"), "Write code to a file": qsTr("Wrote code to a file"),
        "Run in the terminal": qsTr("Ran in the terminal"), "Type into the focused window": qsTr("Typed into a window"),
        "Take a screenshot": qsTr("Took a screenshot"), "Run an Nmap scan": qsTr("Ran an Nmap scan"),
        "Look at the screen": qsTr("Looked at the screen"), "Look at a picture": qsTr("Looked at a picture"),
        "Enumerate a web target (Nikto + Gobuster)": qsTr("Enumerated a web target"),
        "Search Exploit-DB": qsTr("Searched Exploit-DB"), "Test for SQL injection (sqlmap)": qsTr("Tested for SQL injection"),
        "Crack a password hash": qsTr("Cracked a password hash"),
        "Brute-force a network service (Hydra)": qsTr("Brute-forced a network service"),
        "Capture network traffic (tcpdump)": qsTr("Captured network traffic")
    })
    function pastTense(summary: string, status: string): string {
        if (status === "pending" || status === "denied" || status === "running")
            return summary
        return pastTenses[summary] !== undefined ? pastTenses[summary] : summary
    }
    function askQuestion(name: string): string {
        switch (name) {
        case "run_shell_command": case "run_in_terminal": return qsTr("May I run this command?")
        case "write_file": case "write_code": return qsTr("May I write this file?")
        case "delete_path": return qsTr("May I delete this?")
        case "move_path": return qsTr("May I move this?")
        case "copy_path": return qsTr("May I copy this?")
        case "make_directory": return qsTr("May I create this folder?")
        case "open_url": return qsTr("May I open this link?")
        case "open_application": return qsTr("May I open this app?")
        case "type_text": return qsTr("May I type this into the focused window?")
        case "take_screenshot": return qsTr("May I take a screenshot?")
        case "look_at_image": return qsTr("May I send this picture to the vision models?")
        case "web_search": return qsTr("May I search the web for this?")
        case "read_file": return qsTr("May I read this file?")
        case "search_files": return qsTr("May I search your files?")
        case "get_system_info": return qsTr("May I read your system information?")
        default: return qsTr("May I go ahead with this?")
        }
    }
    function riskNote(c: var): string {
        if (!c)
            return ""
        if (c.name === "run_shell_command" || c.name === "run_in_terminal")
            return qsTr("Runs in your shell with your permissions. Read it before you say yes.")
        if (c.risky)
            return qsTr("This can change or remove things on your computer.")
        if (c.changesSystem)
            return qsTr("This changes your computer. File changes can be undone from History.")
        return qsTr("Read-only: nothing on your computer changes.")
    }
    function errorHeadline(text: string): string {
        if (/No API key|No Claude API key/.test(text))
            return qsTr("I have no API key yet.")
        if (/HTTP 402|credits are used up/.test(text))
            return qsTr("My key has run out of credit.")
        if (/HTTP 40[13]|rejected the key|refused this request|did not accept the API key/.test(text))
            return qsTr("%1 didn't accept my key.").arg(serviceName)
        if (/timed out|Network error|no data/.test(text))
            return qsTr("I couldn't reach %1.").arg(serviceName)
        if (/HTTP 429|HTTP 529|overloaded|Rate limited|limiting requests/.test(text))
            return qsTr("%1 is busy right now.").arg(serviceName)
        if (/Stopped after/.test(text))
            return qsTr("I stopped, to avoid going in circles.")
        if (/declined/.test(text))
            return qsTr("%1 declined this one.").arg(serviceName)
        if (/HTTP 404|not found|does not offer/.test(text))
            return qsTr("%1 couldn't find that model or endpoint.").arg(serviceName)
        return qsTr("Something went wrong.")
    }
    function errorNeedsKey(text: string): bool {
        return /HTTP 40[123]|rejected the key|did not accept the API key|No API key|No Claude API key/.test(text)
    }

    // ---- tools (toolregistry.cpp) --------------------------------------------------------------
    readonly property var tools: [
        { name: "run_shell_command", label: qsTr("Run shell commands"), changes: true, risky: true },
        { name: "run_in_terminal", label: qsTr("Run in terminal"), changes: true, risky: true },
        { name: "read_file", label: qsTr("Read files"), changes: false, risky: false },
        { name: "write_file", label: qsTr("Write files"), changes: true, risky: false },
        { name: "write_code", label: qsTr("Write code"), changes: true, risky: false },
        { name: "search_files", label: qsTr("Search files"), changes: false, risky: false },
        { name: "make_directory", label: qsTr("Create folders"), changes: true, risky: false },
        { name: "move_path", label: qsTr("Move / rename"), changes: true, risky: true },
        { name: "copy_path", label: qsTr("Copy files / folders"), changes: true, risky: false },
        { name: "delete_path", label: qsTr("Delete files / folders"), changes: true, risky: true },
        { name: "open_application", label: qsTr("Open applications"), changes: true, risky: false },
        { name: "open_url", label: qsTr("Open URLs / browser"), changes: true, risky: false },
        { name: "web_search", label: qsTr("Web search"), changes: true, risky: false },
        { name: "get_system_info", label: qsTr("Read system info"), changes: false, risky: false },
        { name: "type_text", label: qsTr("Type into focused window"), changes: true, risky: true },
        { name: "take_screenshot", label: qsTr("Take screenshots"), changes: true, risky: false },
        { name: "look_at_image", label: qsTr("Look at pictures and the screen"), changes: true, risky: false }
    ]
    function toolLabel(name: string): string {
        for (let i = 0; i < tools.length; ++i)
            if (tools[i].name === name)
                return tools[i].label
        return name.replace(/_/g, " ")
    }
    // assistant.cpp: autopilot never asks; otherwise changing tools always ask,
    // read-only ones only under "ask every time".
    function asksFirst(t: var): bool {
        // Assistant::needsConfirmation
        return autonomy !== "autopilot" && (t.changes || autonomy !== "ask_risky")
    }
    function toolOn(name: string): bool {
        return cfg.enabledTools.indexOf(name) >= 0
    }

    // ---- intents ---------------------------------------------------------------------------------
    // True only when the message really went out (the prompt is cleared
    // then): the agent's state is read now, not from a binding that may lag.
    function send(text: string): bool {
        const t = text.trim()
        if (t === "" || !agent.canSend || hasPending)
            return false
        const before = messages.count
        agent.sendMessage(t)
        return messages.count > before
    }
    // Stop: the request, a question waiting for you (denied) and her voice.
    function stop() {
        // (read first: denying resolves the question at once)
        const asked = hasPending
        if (asked)
            agent.confirmAll(false)
        if (working || asked)
            agent.stop()    // no answer is asked for with the denial
        voice.stopSpeaking()
    }
    // Push-to-talk: the first press listens, the second one sends what was said.
    function talk() {
        voice.toggleListening()
    }
    function finishTalk() {
        voice.finishListening()
    }
    function cancelTalk() {
        voice.cancel()
    }
    function confirm(callId: string, run: bool) {
        agent.confirmTool(-1, callId, run)
    }
    function confirmAll(run: bool) {
        agent.confirmAll(run)
    }
    function setThinkHard(on: bool) {
        agent.thinkHard = on
    }
    function newConversation(): bool {
        if (busy) {
            whisper(qsTr("Stop or finish the current answer first"), "warning")
            return false
        }
        // An empty conversation is already a new one: no second empty file
        // (every agent.newConversation() writes one).
        if (messages.count > 0 || activeId === "")
            agent.newConversation()
        view = "stage"
        conversationsOpen = false
        focusPrompt()
        return true
    }
    function selectConversation(id: string): bool {
        if (busy) {
            whisper(qsTr("Stop or finish the current answer before switching"), "warning")
            return false
        }
        agent.selectConversation(id)
        view = "stage"
        return true
    }
    function renameConversation(id: string, title: string) {
        const t = title.trim()
        if (t !== "")
            agent.renameConversation(id, t)
    }
    function deleteConversation(id: string): bool {
        if (busy && id === activeId) {
            whisper(qsTr("Stop or finish the current answer first"), "warning")
            return false
        }
        agent.deleteConversation(id)
        return true
    }
    // VoiceController ends any listen itself when the microphone is muted
    // (cancel() here would also stop her speech and end the voice turn).
    function setMuted(on: bool) {
        cfg.micMuted = on
    }
    function setWake(on: bool) {
        cfg.wakeEnabled = on
    }
    // "Try again" retries the failed turn itself (no second copy of your message).
    function retry() {
        if (!agent.retry())
            whisper(qsTr("There is nothing to try again"), "info")
    }
    // Undo from History: a failure is whispered (agent.errorMessage), but
    // the conversation is not marked as failed.
    function undo(id: string): string {
        undoing = true
        const why = agent.undoAction(id)
        undoing = false
        return why
    }
    function readAloud(row: int, text: string) {
        if (!cfg.speakReplies) {   // VoiceController.speak() stays silent then
            whisper(qsTr("Speaking is off — turn on “Speak replies aloud” in Settings › Voice"), "info")
            return
        }
        readAloudRow = row
        readAloudStarted = false
        voice.speak(text)
    }
    function copy(text: string) {
        agent.copyToClipboard(text)
        whisper(qsTr("Copied to the clipboard"), "success")
    }
    // Links in her answers: only web and e-mail links open (in the browser).
    function openLink(href: string) {
        if (Md.openableLink(href))
            Qt.openUrlExternally(href)
        else
            whisper(qsTr("Only web and e-mail links open from here: %1").arg(href.length > 60 ? href.slice(0, 57) + "…" : href),
                    "warning")
    }
    function reload() {
        agent.reloadConfig()
    }
}
