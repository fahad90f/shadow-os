// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.assistant

// Settings, as one calm scrolling page with a numbered index under her face:
// 01 Voice · 02 Models · 03 Behaviour · 04 Tools · 05 Activity. Every switch
// writes straight through the configuration's own properties (persisted).
Item {
    id: page
    required property StageModel stage
    readonly property AssistantConfig cfg: stage.cfg
    readonly property VoiceController voice: stage.voice
    readonly property ModelCatalog catalog: stage.agent.catalog
    readonly property SkillLibrary skills: stage.agent.skills
    property string section: "voice"
    readonly property bool wide: width >= 900
    readonly property var sections: [
        { key: "voice", label: qsTr("Voice") }, { key: "models", label: qsTr("Models") },
        { key: "behaviour", label: qsTr("Behaviour") }, { key: "tools", label: qsTr("Tools") },
        { key: "skills", label: qsTr("Skills") }, { key: "activity", label: qsTr("Activity") }
    ]

    // The section chosen in the index stays marked while the page sits where
    // the jump put it (the last section is too short to reach the top, so its
    // position alone would mark the one before it); scrolling marks by position.
    property string jumped: ""
    property real jumpY: -1
    function jump(key: string) {
        const target = key === "models" ? secModels : key === "behaviour" ? secBehaviour
                     : key === "tools" ? secTools : key === "skills" ? secSkills : key === "activity" ? secActivity : secVoice
        flick.contentY = Math.max(0, Math.min(target.y - 8, flick.contentHeight - flick.height))
        jumped = key
        jumpY = flick.contentY
    }
    onSectionChanged: Qt.callLater(jump, section)
    onVisibleChanged: if (visible) Qt.callLater(jump, section)
    readonly property string inView: {
        if (jumped !== "" && Math.abs(flick.contentY - jumpY) < 1)
            return jumped
        if (flick.atYEnd && flick.contentHeight > flick.height + 1)
            return "activity"
        const y = flick.contentY + 80
        return y >= secActivity.y ? "activity" : y >= secSkills.y ? "skills" : y >= secTools.y ? "tools" : y >= secBehaviour.y ? "behaviour"
             : y >= secModels.y ? "models" : "voice"
    }
    // the content column: the same place as History's (centred, clear of the index)
    readonly property real colW: wide ? Math.min(720, width - 272) : Math.min(720, width - 44)
    readonly property real colX: wide ? Math.max(232, Math.round((width - colW) / 2)) : 16
    function reload() {
        stage.reload()
        stage.whisper(qsTr("Configuration reloaded"), "info")
    }
    // What the natural voice is made of right now (the card's title already says "ready").
    readonly property string neuralDetail: {
        const parts = []
        parts.push(voice.resolvedTtsEngine === "espeak" ? qsTr("speaking with eSpeak NG")
                   : qsTr("%1 · Kokoro voices").arg(stage.voiceName))
        const stt = cfg.sttModel !== "auto" && cfg.sttModel !== "" ? cfg.sttModel
                  : voice.sttModels.length > 0 ? voice.sttModels[0] : ""
        parts.push(stt === "" ? qsTr("no speech recognition")
                   : qsTr("%1 speech recognition").arg(stt.charAt(0).toUpperCase() + stt.slice(1)))
        if (stage.micMuted)
            parts.push(qsTr("microphone muted"))
        else if (stage.armed)
            parts.push(qsTr("listening for “Shadow”"))
        else if (!stage.wakeEnabled)
            parts.push(qsTr("wake word off"))
        return parts.join(" · ")
    }

    // ---- index (wide) ---------------------------------------------------------------
    Column {
        visible: page.wide
        x: 34
        y: 132
        width: 170
        spacing: 2
        Repeater {
            model: page.sections
            delegate: Item {
                id: idx
                required property var modelData
                required property int index
                readonly property bool on: page.inView === modelData.key
                width: 170
                height: 36
                activeFocusOnTab: true
                Accessible.role: Accessible.Link
                Accessible.name: modelData.label
                Keys.onReturnPressed: page.jump(idx.modelData.key)
                Keys.onSpacePressed: page.jump(idx.modelData.key)
                Rectangle {
                    anchors.fill: parent
                    radius: 10
                    color: idxHover.hovered ? Pe.alpha(Pe.ink, 0.05) : "transparent"
                    border.width: idx.activeFocus ? 2 : 0
                    border.color: Pe.cyan
                }
                Rectangle {
                    visible: idx.on
                    x: 0; width: 2; height: 18
                    anchors.verticalCenter: parent.verticalCenter
                    radius: 1
                    gradient: Gradient {
                        GradientStop { position: 0; color: Pe.cyan }
                        GradientStop { position: 1; color: Pe.violet }
                    }
                }
                Text {
                    textFormat: Text.PlainText
                    x: 14
                    anchors.verticalCenter: parent.verticalCenter
                    text: "0" + (idx.index + 1)
                    font.family: Pe.mono
                    font.pixelSize: Pe.small
                    color: idx.on ? Pe.her : Pe.inkMute
                }
                Text {
                    textFormat: Text.PlainText
                    x: 44
                    anchors.verticalCenter: parent.verticalCenter
                    text: idx.modelData.label
                    font.family: Pe.sans
                    font.pixelSize: 14
                    font.weight: idx.on ? Font.DemiBold : Font.Normal
                    color: idx.on ? Pe.ink : Pe.inkSoft
                }
                HoverHandler { id: idxHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: page.jump(idx.modelData.key) }
            }
        }
        Item { width: 1; height: 18 }
        Rectangle { x: 14; width: 140; height: 1; color: Pe.line }
        Item { width: 1; height: 12 }
        PeButton {
            kind: "ghost"; compact: true
            iconName: "undo"
            text: qsTr("History & undo")
            onClicked: page.stage.view = "history"
        }
        PeButton {
            kind: "ghost"; compact: true
            iconName: "refresh"
            text: qsTr("Reload configuration")
            tip: qsTr("Read %1 again").arg(page.cfg.configPath)
            onClicked: page.reload()
        }
    }

    // ---- index (narrow): a row of chips -----------------------------------------------------
    Flickable {
        visible: !page.wide
        x: 12
        y: 64
        width: parent.width - 24
        height: 40
        contentWidth: chipRow.implicitWidth
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        Row {
            id: chipRow
            spacing: 6
            Repeater {
                model: page.sections
                delegate: PeButton {
                    required property var modelData
                    compact: true
                    kind: page.inView === modelData.key ? "primary" : "secondary"
                    text: modelData.label
                    onClicked: page.jump(modelData.key)
                }
            }
            PeButton {
                compact: true
                kind: "ghost"
                iconName: "undo"
                tip: qsTr("History & undo")
                onClicked: page.stage.view = "history"
            }
            PeButton {
                compact: true
                kind: "ghost"
                iconName: "refresh"
                text: qsTr("Reload")
                tip: qsTr("Reload configuration")
                onClicked: page.reload()
            }
        }
    }

    // ---- content -----------------------------------------------------------------------------
    Flickable {
        id: flick
        x: page.wide ? 232 : 16
        y: page.wide ? 72 : 108
        width: parent.width - x - (page.wide ? 40 : 16)
        height: parent.height - y
        contentHeight: content.implicitHeight + 60
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: PeScrollBar {}

        ColumnLayout {
            id: content
            x: page.colX - flick.x
            width: page.colW
            spacing: 0

            // ================= 01 VOICE =================
            SectionHead {
                id: secVoice
                number: "01"; title: qsTr("Voice")
                lead: qsTr("How Shadow sounds and when she listens. Everything here runs on this computer.")
            }

            // engine status
            Rectangle {
                Layout.fillWidth: true
                Layout.topMargin: 6
                implicitHeight: engineCol.implicitHeight + 32
                radius: 14
                color: Pe.card
                border.width: 1
                border.color: page.stage.neural ? Pe.alpha(Pe.ok, 0.35)
                            : page.stage.engine === "starting" ? Pe.line : Pe.alpha(Pe.caution, 0.45)
                ColumnLayout {
                    id: engineCol
                    x: 18; y: 16
                    width: parent.width - 36
                    spacing: 12
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 14
                        Rectangle {
                            Layout.alignment: Qt.AlignTop
                            implicitWidth: 40; implicitHeight: 40; radius: 20
                            color: Pe.alpha(page.stage.neural ? Pe.ok : Pe.caution, 0.12)
                            PeIcon {
                                anchors.centerIn: parent
                                name: page.stage.neural ? "sound" : page.stage.engine === "error" ? "alert-triangle" : "download"
                                size: 18
                                color: page.stage.neural ? Pe.ok : Pe.caution
                            }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Text {
                                textFormat: Text.PlainText
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                text: page.stage.neural ? qsTr("Natural voice · ready")
                                    : page.stage.installing ? qsTr("Installing the natural voice… %1%").arg(Math.round(page.stage.installProgress * 100))
                                    : page.stage.engine === "starting" ? qsTr("Starting the natural voice…")
                                    : page.stage.engine === "basic" ? qsTr("Basic voice only")
                                    : page.stage.engine === "error" ? qsTr("The voice engine stopped")
                                    : qsTr("No voice engine")
                                font.family: Pe.sans; font.pixelSize: 15; font.weight: Font.DemiBold
                                color: Pe.ink
                            }
                            Text {
                                textFormat: Text.PlainText
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                maximumLineCount: 6
                                elide: Text.ElideRight
                                text: page.stage.neural ? page.neuralDetail
                                    : page.stage.installing ? page.stage.installStatus
                                    : page.stage.engineMessage
                                font.family: Pe.sans; font.pixelSize: Pe.small
                                color: Pe.inkSoft
                            }
                            Text {
                                textFormat: Text.PlainText
                                visible: !page.stage.installing && page.stage.installStatus !== "" && !page.stage.neural
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                text: qsTr("Last install: %1").arg(page.stage.installStatus)
                                font.family: Pe.sans; font.pixelSize: Pe.small
                                color: Pe.caution
                            }
                        }
                        PeButton {
                            Layout.alignment: Qt.AlignTop
                            visible: !page.stage.installing
                            compact: true; kind: "ghost"; iconName: "refresh"
                            text: qsTr("Re-check")
                            tip: qsTr("Look for the voice engine again and restart it")
                            onClicked: page.voice.refresh()
                        }
                        PeButton {
                            Layout.alignment: Qt.AlignTop
                            visible: !page.stage.installing && (page.stage.engine === "missing" || page.stage.engine === "basic")
                            kind: "accent"; iconName: "download"
                            text: qsTr("Install")
                            tip: qsTr("Download and install the natural voice for this user (about 550 MB)")
                            onClicked: page.voice.installEngine()
                        }
                        PeButton {
                            Layout.alignment: Qt.AlignTop
                            visible: page.stage.installing
                            compact: true; kind: "secondary"
                            text: qsTr("Cancel")
                            onClicked: page.voice.cancelInstall()
                        }
                    }
                    Rectangle {
                        visible: page.stage.installing
                        Layout.fillWidth: true
                        Layout.leftMargin: 54
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
                    // the microphone, live
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 14
                        Text {
                            textFormat: Text.PlainText
                            Layout.preferredWidth: 40
                            text: qsTr("MIC")
                            horizontalAlignment: Text.AlignHCenter
                            font.family: Pe.sans; font.pixelSize: Pe.label; font.weight: Font.Bold
                            font.letterSpacing: Pe.labelTracking
                            color: Pe.inkMute
                        }
                        LevelDots {
                            level: page.stage.micMuted ? 0 : page.stage.micLevel
                            dots: Math.max(8, Math.min(30, Math.floor((engineCol.width - 300) / 10)))
                        }
                        Text {
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            text: page.stage.micMuted ? qsTr("muted — the microphone is closed")
                                : (page.cfg.inputDevice !== "" ? page.cfg.inputDevice : qsTr("default input"))
                                  + " · " + (page.voice.resolvedCaptureTool !== "" ? page.voice.resolvedCaptureTool : qsTr("no capture tool"))
                            font.family: Pe.sans; font.pixelSize: Pe.small
                            color: page.stage.micMuted ? Pe.alarm : Pe.inkMute
                        }
                        Text {
                            textFormat: Text.PlainText
                            text: qsTr("Mute")
                            font.family: Pe.sans; font.pixelSize: Pe.small; font.weight: Font.Medium
                            color: page.stage.micMuted ? Pe.alarm : Pe.inkSoft
                        }
                        PeSwitch {
                            text: qsTr("Mute the microphone")
                            checked: page.stage.micMuted
                            onToggled: page.stage.setMuted(checked)
                        }
                    }
                    // what was detected, honestly
                    Flow {
                        Layout.fillWidth: true
                        Layout.leftMargin: 54
                        spacing: 6
                        Repeater {
                            model: [
                                { label: qsTr("Speech"), ok: page.voice.ttsAvailable },
                                { label: qsTr("Recognition"), ok: page.voice.sttAvailable },
                                { label: qsTr("Wake word"), ok: page.voice.wakeAvailable },
                                { label: qsTr("Microphone"), ok: page.voice.captureAvailable },
                                { label: qsTr("Typing"), ok: page.voice.typeAvailable }
                            ]
                            delegate: Rectangle {
                                id: capChip
                                required property var modelData
                                height: 22
                                width: capRow.implicitWidth + 16
                                radius: 11
                                color: Pe.alpha(modelData.ok ? Pe.ok : Pe.caution, 0.1)
                                Accessible.role: Accessible.StaticText
                                Accessible.name: modelData.label + ": " + (modelData.ok ? qsTr("available") : qsTr("not available"))
                                Row {
                                    id: capRow
                                    anchors.centerIn: parent
                                    spacing: 5
                                    PeIcon {
                                        anchors.verticalCenter: parent.verticalCenter
                                        size: 11
                                        name: capChip.modelData.ok ? "check" : "close"
                                        color: capChip.modelData.ok ? Pe.ok : Pe.caution
                                    }
                                    Text {
                                        textFormat: Text.PlainText
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: capChip.modelData.label
                                        font.family: Pe.sans; font.pixelSize: Pe.label; font.weight: Font.Medium
                                        color: capChip.modelData.ok ? Pe.inkSoft : Pe.chipText(Pe.caution)
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // voices
            Text {
                textFormat: Text.PlainText
                Layout.topMargin: 26
                text: qsTr("HER VOICE")
                font.family: Pe.sans; font.pixelSize: Pe.label; font.weight: Font.Bold
                font.letterSpacing: Pe.labelTracking
                color: Pe.inkMute
            }
            GridLayout {
                Layout.fillWidth: true
                Layout.topMargin: 10
                columns: content.width >= 560 ? 3 : 2
                columnSpacing: 10
                rowSpacing: 10
                Repeater {
                    model: page.stage.voices
                    delegate: VoiceCard {
                        required property var modelData
                        Layout.fillWidth: true
                        Layout.fillHeight: true     // one height for the cards of a row
                        voiceInfo: modelData
                        selected: page.cfg.voice === modelData.id
                        onChosen: page.cfg.voice = modelData.id
                        onPreview: page.voice.previewVoice(modelData.id)
                    }
                }
            }

            // pace
            SettingRow {
                Layout.fillWidth: true
                Layout.topMargin: 14
                title: qsTr("Speaking pace")
                note: qsTr("Slower for long explanations, faster for quick answers.")
                Text {
                    textFormat: Text.PlainText
                    anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                    visible: content.width >= 560
                    text: qsTr("slower")
                    font.family: Pe.sans; font.pixelSize: Pe.small; color: Pe.inkMute
                }
                PeSlider {
                    id: pace
                    anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                    width: Math.min(220, content.width * 0.32)
                    from: 0.7; to: 1.4; stepSize: 0.05
                    value: page.cfg.speechRate
                    onMoved: page.cfg.speechRate = Math.round(value * 20) / 20
                    Accessible.name: qsTr("Speaking pace")
                }
                Text {
                    textFormat: Text.PlainText
                    anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                    visible: content.width >= 560
                    text: qsTr("faster")
                    font.family: Pe.sans; font.pixelSize: Pe.small; color: Pe.inkMute
                }
                Text {
                    textFormat: Text.PlainText
                    anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                    width: 42
                    horizontalAlignment: Text.AlignRight
                    text: pace.value.toFixed(2).replace(/0$/, "") + "×"
                    font.family: Pe.mono; font.pixelSize: Pe.ui; color: Pe.ink
                }
            }
            SettingRow {
                Layout.fillWidth: true
                title: qsTr("Listen for “Shadow”")
                note: page.stage.micMuted ? qsTr("The microphone is off — turn it on to use the wake word.")
                      : !page.voice.wakeAvailable && page.stage.wakeEnabled
                      ? qsTr("On, but it needs the natural voice engine to listen.")
                      : qsTr("Hands-free, on this computer only. Nothing is captured or sent until you say her name.")
                noteColor: !page.voice.wakeAvailable && page.stage.wakeEnabled && !page.stage.micMuted ? Pe.caution : Pe.inkMute
                PeSegmented {
                    visible: page.stage.wakeEnabled && content.width >= 600
                    anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                    accessibleName: qsTr("Wake word sensitivity")
                    model: [{ value: "low", label: qsTr("Calm") }, { value: "normal", label: qsTr("Normal") },
                            { value: "high", label: qsTr("Eager") }]
                    value: page.cfg.wakeSensitivity
                    onPicked: (v) => page.cfg.wakeSensitivity = v
                }
                PeSwitch {
                    anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                    text: qsTr("Listen for “Shadow”")
                    checked: page.stage.wakeEnabled
                    enabled: !page.stage.micMuted
                    onToggled: page.stage.setWake(checked)
                }
            }
            SettingRow {
                Layout.fillWidth: true
                visible: page.stage.wakeEnabled && content.width < 600
                title: qsTr("Wake word sensitivity")
                note: qsTr("Calm ignores more near-misses; Eager answers sooner.")
                PeSegmented {
                    anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                    accessibleName: qsTr("Wake word sensitivity")
                    model: [{ value: "low", label: qsTr("Calm") }, { value: "normal", label: qsTr("Normal") },
                            { value: "high", label: qsTr("Eager") }]
                    value: page.cfg.wakeSensitivity
                    onPicked: (v) => page.cfg.wakeSensitivity = v
                }
            }
            SettingRow {
                Layout.fillWidth: true
                title: qsTr("Follow-up")
                note: qsTr("After she answers out loud, she keeps listening for 5 seconds — no wake word needed.")
                PeSwitch {
                    anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                    text: qsTr("Follow-up")
                    checked: page.cfg.followUp
                    onToggled: page.cfg.followUp = checked
                }
            }
            SettingRow {
                Layout.fillWidth: true
                title: qsTr("Speak replies aloud")
                note: qsTr("Answers to spoken questions are read out. Code is never read; links become “a link”.")
                PeSwitch {
                    anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                    text: qsTr("Speak replies aloud")
                    checked: page.cfg.speakReplies
                    onToggled: page.cfg.speakReplies = checked
                }
            }
            SettingRow {
                Layout.fillWidth: true
                title: qsTr("Speech engine")
                note: page.stage.engine === "basic" ? qsTr("Only the basic voice (eSpeak NG) is installed.")
                      : page.voice.resolvedTtsEngine !== ""
                      ? qsTr("Natural is the neural voice; Basic is eSpeak NG. Speaking with: %1.")
                        .arg(page.voice.resolvedTtsEngine === "kokoro" ? qsTr("Natural") : qsTr("Basic"))
                      : qsTr("Natural is the neural voice; Basic is eSpeak NG. Nothing can speak right now.")
                PeSegmented {
                    anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                    accessibleName: qsTr("Speech engine")
                    model: [{ value: "auto", label: qsTr("Auto") }, { value: "neural", label: qsTr("Natural") },
                            { value: "espeak", label: qsTr("Basic") }]
                    value: page.cfg.ttsEngine
                    onPicked: (v) => page.cfg.ttsEngine = v
                }
            }
            SettingRow {
                Layout.fillWidth: true
                title: qsTr("Speech recognition")
                note: page.voice.sttModels.length === 0 ? qsTr("Not installed yet — it comes with the natural voice.")
                      : qsTr("Moonshine is fast; Whisper is more accurate with accents and noise. Installed: %1.")
                        .arg(page.voice.sttModels.join(", "))
                PeSegmented {
                    anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                    accessibleName: qsTr("Speech recognition model")
                    model: [{ value: "auto", label: qsTr("Auto") },
                            { value: "moonshine", label: "Moonshine", enabled: page.voice.sttModels.indexOf("moonshine") >= 0 },
                            { value: "whisper", label: "Whisper", enabled: page.voice.sttModels.indexOf("whisper") >= 0 }]
                    value: page.cfg.sttModel
                    onPicked: (v) => page.cfg.sttModel = v
                }
            }
            SettingRow {
                Layout.fillWidth: true
                divider: false
                title: qsTr("Microphone input")
                note: qsTr("Leave empty for the system's default input, or enter a PipeWire/PulseAudio source or an ALSA device.")
                Rectangle {
                    anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                    width: Math.min(240, content.width * 0.36)
                    height: 34
                    radius: 17
                    color: Pe.sunken
                    border.width: 1
                    border.color: deviceField.activeFocus ? Pe.alpha(Pe.violet, 0.6) : Pe.line
                    TextField {
                        id: deviceField
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        background: null
                        text: page.cfg.inputDevice
                        placeholderText: qsTr("Default input")
                        placeholderTextColor: Pe.inkMute
                        color: Pe.ink
                        font.family: Pe.mono
                        font.pixelSize: Pe.small
                        verticalAlignment: TextInput.AlignVCenter
                        selectByMouse: true
                        Accessible.name: qsTr("Microphone input device")
                        onEditingFinished: {
                            if (text.trim() !== page.cfg.inputDevice)
                                page.cfg.inputDevice = text.trim()
                        }
                    }
                }
            }

            // ================= 02 MODELS =================
            SectionHead {
                id: secModels
                number: "02"; title: qsTr("Models")
                lead: page.stage.localMode ? qsTr("The mind behind her: a model on this computer.")
                    : page.stage.nvidia ? qsTr("The mind behind her: NVIDIA's hosted models. Several work on each request at the same time, and one combines their work into the answer.")
                    : qsTr("The mind behind her: Claude, from Anthropic.")
            }
            SettingRow {
                Layout.fillWidth: true
                divider: false
                title: qsTr("API key")
                note: page.cfg.localMode ? qsTr("Not needed: she runs on a local model")
                    : !page.cfg.apiKeyConfigured ? qsTr("Paste your key below to wake her up.")
                    : page.cfg.apiKeySource === "Settings"
                      ? (page.cfg.apiKeyHint !== "" ? qsTr("Saved here · ends in %1").arg(page.cfg.apiKeyHint) : qsTr("Saved here"))
                    : page.cfg.apiKeyHint !== "" ? qsTr("From %1 · ends in %2").arg(page.cfg.apiKeySource).arg(page.cfg.apiKeyHint)
                    : qsTr("From %1").arg(page.cfg.apiKeySource)
                StatusChip {
                    readonly property bool rejected: page.catalog.keyState === "rejected" && !page.cfg.localMode
                    ok: (page.cfg.apiKeyConfigured && !rejected) || page.cfg.localMode
                    text: page.cfg.localMode ? qsTr("Offline") : !page.cfg.apiKeyConfigured ? qsTr("Not configured")
                        : rejected ? qsTr("Not accepted") : page.catalog.keyState === "ok" ? qsTr("Working") : qsTr("Configured")
                }
            }
            KeyField {
                Layout.fillWidth: true
                stage: page.stage
            }
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                Layout.bottomMargin: 12
                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                lineHeight: 1.25
                text: qsTr("Saved in a file only you can read. Optional instead: the SHADOW_ASSISTANT_API_KEY environment variable, or key= under [api] in %1.").arg(page.cfg.configPath)
                font.family: Pe.sans; font.pixelSize: Pe.small; color: Pe.inkMute
            }
            Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Pe.line }
            SettingRow {
                Layout.fillWidth: true
                title: qsTr("Service")
                note: page.cfg.baseUrlOverridden ? qsTr("Overridden by SHADOW_ASSISTANT_BASE_URL") : page.cfg.providerName
                Text {
                    textFormat: Text.PlainText
                    anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                    width: Math.min(implicitWidth, content.width * 0.55)
                    elide: Text.ElideMiddle
                    text: page.cfg.baseUrl
                    font.family: Pe.mono; font.pixelSize: Pe.small; color: Pe.inkSoft
                }
            }

            // ---- NVIDIA: the team ----
            SettingRow {
                visible: page.stage.nvidia
                Layout.fillWidth: true
                title: qsTr("Work as a team")
                note: page.cfg.teamMode ? qsTr("On — every request goes to several models at once. Each writes its own answer or plan; the lead model combines them, replies, and is the only one that runs tools.")
                                        : qsTr("Off — one model answers on its own: quicker, less thorough.")
                PeSwitch {
                    anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                    text: qsTr("Work as a team")
                    checked: page.cfg.teamMode
                    onToggled: page.cfg.teamMode = checked
                }
            }
            Repeater {
                model: page.stage.nvidia ? page.catalog.groups : []
                delegate: TeamGroup {
                    required property var modelData
                    Layout.fillWidth: true
                    group: modelData
                    teamMode: page.cfg.teamMode
                }
            }
            SettingRow {
                visible: page.stage.nvidia
                Layout.fillWidth: true
                title: qsTr("Model list")
                note: page.catalog.error !== "" ? qsTr("Could not be read: %1").arg(page.catalog.error)
                    : page.catalog.known ? qsTr("NVIDIA offers %1 models. A model it stops offering is left out of the team by itself.").arg(page.catalog.count)
                    : qsTr("Not read yet — every model above is tried.")
                noteColor: page.catalog.error !== "" ? Pe.chipText(Pe.caution) : Pe.inkMute
                PeButton {
                    anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                    compact: true
                    kind: "secondary"
                    iconName: "refresh"
                    text: page.catalog.refreshing ? qsTr("Reading…") : qsTr("Read again")
                    enabled: !page.catalog.refreshing
                    onClicked: page.catalog.readAgain()
                }
            }

            // ---- Claude (an sk-ant- key): one model per job ----
            Repeater {
                model: page.stage.nvidia || page.stage.localMode ? []
                     : [[qsTr("Brain"), qsTr("chat, tools, vision"), page.cfg.brainModel],
                        [qsTr("Planner"), qsTr("used by Think hard"), page.cfg.plannerModel],
                        [qsTr("Fast"), qsTr("background helpers"), page.cfg.fastModel],
                        [qsTr("Omni"), qsTr("voice & image"), page.cfg.omniModel]]
                delegate: SettingRow {
                    id: modelRow
                    required property var modelData
                    Layout.fillWidth: true
                    title: modelData[0]
                    note: modelData[1]
                    Text {
                        textFormat: Text.PlainText
                        anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                        text: modelRow.modelData[2]
                        font.family: Pe.mono; font.pixelSize: Pe.small; color: Pe.inkSoft
                    }
                }
            }
            SettingRow {
                Layout.fillWidth: true
                divider: false
                title: qsTr("Run offline")
                note: page.stage.localMode ? qsTr("On — using %1 on this computer. Nothing you type or say leaves the machine.").arg(page.cfg.localModel)
                                           : qsTr("Use a local model instead of the cloud (set it up with: shadow-assistant-local setup).")
                PeSwitch {
                    anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                    text: qsTr("Run offline")
                    checked: page.stage.localMode
                    onToggled: page.cfg.localMode = checked
                }
            }

            // ================= 03 BEHAVIOUR =================
            SectionHead {
                id: secBehaviour
                number: "03"; title: qsTr("Behaviour")
                lead: qsTr("Who she is and how much she does before asking you.")
            }
            SettingRow {
                Layout.fillWidth: true
                title: qsTr("Mode")
                note: page.stage.role === "pentest"
                      ? qsTr("Penetration testing: offensive-security prompt and seven pentest tools. Only on systems you own or are contracted in writing to test.")
                      : qsTr("Standard assistant with the full productivity toolset.")
                noteColor: page.stage.role === "pentest" ? Pe.caution : Pe.inkMute
                PeSegmented {
                    anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                    accessibleName: qsTr("Mode")
                    model: [{ value: "general", label: qsTr("General") }, { value: "pentest", label: qsTr("Pentest") }]
                    value: page.stage.role
                    onPicked: (v) => page.cfg.role = v
                }
            }
            SettingRow {
                Layout.fillWidth: true
                divider: content.width < 640
                title: qsTr("Autonomy")
                note: page.stage.autonomy === "autopilot"
                      ? qsTr("Runs every action with no confirmation, however risky. Use with care.")
                      : page.stage.autonomy === "ask_risky" ? qsTr("Read-only tools run on their own; anything that changes your system asks first.")
                      : qsTr("Every tool call waits for your yes.")
                noteColor: page.stage.autonomy === "autopilot" ? Pe.alarm : Pe.inkMute
                PeSegmented {
                    visible: content.width >= 640
                    anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                    accessibleName: qsTr("Autonomy")
                    model: page.autonomyChoices
                    value: page.stage.autonomy
                    onPicked: (v) => page.cfg.autonomy = v
                }
            }
            PeSegmented {
                visible: content.width < 640
                Layout.topMargin: 4
                Layout.bottomMargin: 8
                accessibleName: qsTr("Autonomy")
                model: page.autonomyChoices
                value: page.stage.autonomy
                onPicked: (v) => page.cfg.autonomy = v
            }

            // ================= 04 TOOLS =================
            SectionHead {
                id: secTools
                number: "04"; title: qsTr("Tools")
                lead: qsTr("What she may do. A switched-off tool is never offered to the model.")
            }
            GridLayout {
                Layout.fillWidth: true
                columns: content.width >= 600 ? 2 : 1
                columnSpacing: 24
                rowSpacing: 0
                Repeater {
                    model: page.stage.tools
                    delegate: SettingRow {
                        id: toolRow
                        required property var modelData
                        Layout.fillWidth: true
                        title: modelData.label
                        note: modelData.name + (page.stage.asksFirst(modelData) ? qsTr(" · asks first") : "")
                        noteColor: modelData.risky ? Pe.caution : Pe.inkMute
                        PeSwitch {
                            anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                            text: toolRow.modelData.label
                            checked: page.cfg.enabledTools.indexOf(toolRow.modelData.name) >= 0
                            onToggled: page.cfg.setToolEnabled(toolRow.modelData.name, checked)
                        }
                    }
                }
            }

            // ================= 05 SKILLS =================
            SectionHead {
                id: secSkills
                number: "05"; title: qsTr("Skills")
                lead: qsTr("Short instructions that help her with a kind of task. The ones that fit a request are added to her thinking for that message; nothing is added when none fit.")
            }
            SettingRow {
                Layout.fillWidth: true
                divider: false
                title: qsTr("Use skills")
                note: page.cfg.skillsEnabled ? qsTr("On — %1 skills are available in this mode.").arg(page.skills.count)
                                             : qsTr("Off — no skill is ever added to a request.")
                PeSwitch {
                    anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                    text: qsTr("Use skills")
                    checked: page.cfg.skillsEnabled
                    onToggled: page.cfg.skillsEnabled = checked
                }
            }
            Repeater {
                model: page.skills.groups
                delegate: ColumnLayout {
                    id: skillGroup
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.topMargin: 8
                    spacing: 0
                    Text {
                        textFormat: Text.PlainText
                        Layout.topMargin: 10
                        Layout.bottomMargin: 2
                        text: skillGroup.modelData.title
                        font.family: Pe.sans; font.pixelSize: Pe.label; font.weight: Font.Bold
                        font.letterSpacing: Pe.labelTracking
                        color: skillGroup.modelData.group === "pentest" ? Pe.caution : Pe.inkMute
                    }
                    Repeater {
                        model: skillGroup.modelData.items
                        delegate: SettingRow {
                            id: skillRow
                            required property var modelData
                            Layout.fillWidth: true
                            title: modelData.title
                            note: modelData.summary + (modelData.available ? "" : qsTr("  ·  only in Pentest mode"))
                            opacity: modelData.available && page.cfg.skillsEnabled ? 1 : 0.5
                            PeSwitch {
                                anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                                text: skillRow.modelData.title
                                enabled: page.cfg.skillsEnabled
                                checked: skillRow.modelData.enabled
                                onToggled: page.skills.setSkillEnabled(skillRow.modelData.name, checked)
                            }
                        }
                    }
                }
            }
            SettingRow {
                Layout.fillWidth: true
                divider: false
                title: qsTr("Your own skills")
                note: qsTr("Markdown files in ~/.local/share/shadow/assistant/skills override a built-in of the same name.")
                PeButton {
                    anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                    compact: true
                    kind: "secondary"
                    iconName: "refresh"
                    text: qsTr("Reload skills")
                    onClicked: {
                        page.skills.reload()
                        page.stage.whisper(qsTr("Skills reloaded"), "info")
                    }
                }
            }

            // ================= 06 ACTIVITY =================
            SectionHead {
                id: secActivity
                number: "06"; title: qsTr("Activity")
                lead: qsTr("Every tool call this session, newest first.")
            }
            Repeater {
                model: page.stage.toolLog
                delegate: SettingRow {
                    id: logRow
                    required property var modelData
                    Layout.fillWidth: true
                    title: page.stage.toolLabel(modelData.name)
                    note: modelData.preview
                    Text {
                        textFormat: Text.PlainText
                        anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                        text: logRow.modelData.time
                        font.family: Pe.mono; font.pixelSize: Pe.small; color: Pe.inkMute
                    }
                    StatusChip {
                        tone: logRow.modelData.status === "done" ? Pe.ok : logRow.modelData.status === "error" ? Pe.alarm
                            : logRow.modelData.status === "denied" ? Pe.inkMute : Pe.cyan
                        text: logRow.modelData.status === "done" ? qsTr("done") : logRow.modelData.status === "error" ? qsTr("failed")
                            : logRow.modelData.status === "denied" ? qsTr("denied") : qsTr("running")
                    }
                }
            }
            Text {
                textFormat: Text.PlainText
                visible: page.stage.toolLog.length === 0
                Layout.topMargin: 8
                text: qsTr("Nothing yet — actions she takes will show up here.")
                font.family: Pe.sans; font.pixelSize: Pe.ui; color: Pe.inkMute
            }
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                Layout.topMargin: 36
                wrapMode: Text.Wrap
                text: qsTr("Which models are in each team, the local model and other advanced options live in %1 (Reload configuration picks up changes).").arg(page.cfg.configPath)
                font.family: Pe.sans; font.pixelSize: Pe.small; color: Pe.inkMute
            }
        }
    }

    readonly property var autonomyChoices: [
        { value: "ask_every", label: qsTr("Ask every time") }, { value: "ask_risky", label: qsTr("Ask if it changes things") },
        { value: "autopilot", label: qsTr("Autopilot") }
    ]

    // One team: what it is for, and its models. The lead is marked; a model the
    // service does not offer right now is dimmed.
    component TeamGroup: Item {
        id: tg
        property var group: ({})
        property bool teamMode: true
        readonly property var members: group.members !== undefined ? group.members : []
        implicitHeight: tgCol.implicitHeight + 24
        Accessible.role: Accessible.Grouping
        Accessible.name: qsTr("%1 team").arg(group.title !== undefined ? group.title : "")
        Column {
            id: tgCol
            y: 12
            width: parent.width
            spacing: 8
            Row {
                spacing: 10
                Text {
                    id: tgTitle
                    textFormat: Text.PlainText
                    text: tg.group.title !== undefined ? tg.group.title : ""
                    font.family: Pe.sans; font.pixelSize: 14; font.weight: Font.Medium
                    color: Pe.ink
                }
                Text {
                    textFormat: Text.PlainText
                    anchors.baseline: tgTitle.baseline
                    text: tg.group.note !== undefined ? tg.group.note : ""
                    font.family: Pe.sans; font.pixelSize: Pe.small
                    color: Pe.inkMute
                }
            }
            Flow {
                width: parent.width
                spacing: 6
                Repeater {
                    model: tg.members
                    delegate: Rectangle {
                        id: chip
                        required property var modelData
                        readonly property bool lead: modelData.lead === true
                        // with the team off only the lead works
                        readonly property bool idle: !modelData.available || (!tg.teamMode && !lead && tg.group.kind !== "vision")
                        height: 24
                        width: chipRow.implicitWidth + 18
                        radius: 12
                        color: lead ? Pe.alpha(Pe.violet, 0.14) : "transparent"
                        border.width: 1
                        border.color: lead ? Pe.alpha(Pe.violet, 0.55) : Pe.lineStrong
                        opacity: idle ? 0.45 : 1
                        Accessible.role: Accessible.StaticText
                        Accessible.name: modelData.name + (lead ? qsTr(", leads") : "")
                                         + (!modelData.available ? qsTr(", not offered right now") : "")
                        Row {
                            id: chipRow
                            anchors.centerIn: parent
                            spacing: 6
                            Text {
                                textFormat: Text.PlainText
                                anchors.verticalCenter: parent.verticalCenter
                                text: chip.modelData.name
                                font.family: Pe.mono; font.pixelSize: 11
                                font.strikeout: !chip.modelData.available
                                color: Pe.ink
                            }
                            Text {
                                textFormat: Text.PlainText
                                visible: chip.lead
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("LEADS")
                                font.family: Pe.sans; font.pixelSize: 9; font.weight: Font.Bold; font.letterSpacing: 0.8
                                color: Pe.her
                            }
                        }
                        HoverHandler { id: chipHover }
                        ToolTip.visible: chipHover.hovered
                        ToolTip.delay: 400
                        ToolTip.text: chip.modelData.id + (!chip.modelData.available ? "\n" + qsTr("NVIDIA does not offer this model right now")
                                                           : chip.lead ? "\n" + qsTr("Combines the team's work, answers, and runs the tools") : "")
                    }
                }
            }
        }
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: Pe.line
        }
    }
    component SectionHead: ColumnLayout {
        id: sh
        property string number: ""
        property string title: ""
        property string lead: ""
        Layout.fillWidth: true
        Layout.topMargin: number === "01" ? 4 : 52
        Layout.bottomMargin: 6
        spacing: 4
        Row {
            spacing: 12
            Text {
                textFormat: Text.PlainText
                anchors.baseline: shTitle.baseline
                text: sh.number
                font.family: Pe.mono; font.pixelSize: 13
                color: Pe.her
            }
            Text {
                id: shTitle
                textFormat: Text.PlainText
                text: sh.title
                font.family: Pe.sans; font.pixelSize: 28; font.weight: Font.Light
                color: Pe.ink
                Accessible.role: Accessible.Heading
                Accessible.name: text
            }
        }
        Text {
            textFormat: Text.PlainText
            Layout.fillWidth: true
            Layout.leftMargin: 32
            wrapMode: Text.WrapAtWordBoundaryOrAnywhere
            text: sh.lead
            font.family: Pe.sans; font.pixelSize: Pe.ui; lineHeight: 1.3
            color: Pe.inkSoft
        }
    }
    component StatusChip: Rectangle {
        id: chip
        property bool ok: true
        property color tone: ok ? Pe.ok : Pe.caution
        property string text: ""
        anchors.verticalCenter: parent ? parent.verticalCenter : undefined
        height: 22
        width: chipText.implicitWidth + 18
        radius: 11
        color: Pe.alpha(tone, 0.13)
        Accessible.role: Accessible.StaticText
        Accessible.name: text
        Text {
            id: chipText
            textFormat: Text.PlainText
            anchors.centerIn: parent
            text: chip.text
            font.family: Pe.sans; font.pixelSize: Pe.small; font.weight: Font.DemiBold
            color: Pe.chipText(chip.tone)
        }
    }
}
