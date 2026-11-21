// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.assistant
import "pages"
import "components"

NxWindow {
    id: window
    title: qsTr("Shadow Assistant")
    width: 1120
    height: 820
    minimumWidth: 720
    minimumHeight: 540

    Assistant { id: agent }
    // With 'agent' set the controller does the voice ⇄ agent wiring itself
    // (transcripts sent, replies spoken, spoken yes/no, errors, Stop).
    VoiceController { id: voice; config: agent.config; agent: agent }

    // "chat" | "settings". The setup state replaces the chat page when no key.
    property string view: "chat"

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
        if (!window.visible)
            window.show()
        if (window.visibility === Window.Minimized)
            window.showNormal()
        window.raise()
        window.requestActivate()
    }
    // Opening the assistant again (dock, launcher) while it runs in the
    // background brings its window back.
    Connections {
        target: shadowApp
        function onActivationRequested(args) { window.present() }
    }

    // Composite presence state: the mic side (voice) takes precedence while a
    // voice turn is active, otherwise the model's chat phase drives the orb.
    readonly property string orbState: {
        if (agent.testOrb !== "") return agent.testOrb
        if (voice.state === "speaking") return "speaking"
        if (voice.state === "listening" || voice.state === "transcribing") return "listening"
        return agent.orbState
    }

    // Push-to-talk: the first click listens, the second one sends what was said.
    function toggleListen() {
        voice.toggleListening()
    }

    // ---- voice presentation (the wiring itself lives in VoiceController) ----
    Connections {
        target: voice
        function onTranscriptReady(text) { window.view = "chat" }
        function onHeard(text) { window.toaster.show(qsTr("Heard: %1").arg(text), "info") }
        function onWakeDetected() {
            window.view = "chat"
            window.present()
            window.toaster.show(qsTr("Shadow is listening…"), "info")
        }
        function onUnavailable(reason) { window.toaster.show(reason, "warning") }
    }

    Component.onCompleted: {
        if (agent.conversations.count > 0)
            agent.selectConversation(agent.conversations.data(agent.conversations.index(0, 0), 257 /*IdRole*/))
        else
            agent.newConversation()
        // Env-gated headless screenshot hooks (no effect in normal use).
        if (agent.testView !== "")
            window.view = agent.testView
        if (agent.testPrompt !== "")
            testPromptTimer.start()
    }
    Timer {
        id: testPromptTimer
        interval: 400
        onTriggered: {
            voice.beginVoiceTurn()      // treat as a spoken turn
            if (agent.canSend)
                agent.sendMessage(agent.testPrompt)
        }
    }

    // Background wash: deep base with a subtle accent aurora for the futuristic feel.
    Rectangle {
        anchors.fill: parent
        color: Theme.bgBase
        Rectangle {
            anchors.fill: parent
            gradient: Gradient {
                GradientStop { position: 0.0; color: Theme.alpha(Theme.accent, 0.06) }
                GradientStop { position: 0.4; color: Theme.alpha(Theme.bgBase, 0.0) }
                GradientStop { position: 1.0; color: Theme.alpha(Theme.accentViolet, 0.05) }
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ---- sidebar ----
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: Theme.sidebarWidth
            color: Theme.alpha(Theme.bgSurface, Appearance.transparency ? 0.7 : 1.0)
            Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: Theme.borderSubtle }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Theme.spaceMd
                spacing: Theme.spaceMd

                // brand
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceSm
                    AssistantOrb { orbState: window.orbState; implicitWidth: 32; implicitHeight: 32 }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Shadow")
                        color: Theme.textPrimary
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeBodyLarge
                        font.weight: Theme.weightSemibold
                        elide: Text.ElideRight
                    }
                }

                // Voice quick-controls: privacy mute, always-listening toggle,
                // and a listening indicator. Push-to-talk lives in the composer.
                Rectangle {
                    Layout.fillWidth: true
                    radius: Theme.radiusSm
                    color: Theme.alpha(Theme.bgRaised, 0.6)
                    border.color: Theme.borderSubtle
                    border.width: 1
                    implicitHeight: voiceRow.implicitHeight + Theme.spaceSm * 2
                    RowLayout {
                        id: voiceRow
                        anchors.fill: parent
                        anchors.margins: Theme.spaceSm
                        spacing: Theme.spaceXs
                        NxIconButton {
                            iconName: agent.config.micMuted ? "mic-off" : "mic"
                            icon.color: agent.config.micMuted ? Theme.danger : Theme.accent
                            checkable: true
                            checked: agent.config.micMuted
                            tooltip: agent.config.micMuted ? qsTr("Microphone muted — click to unmute")
                                                           : qsTr("Mute microphone (privacy)")
                            onClicked: { agent.config.micMuted = !agent.config.micMuted; if (agent.config.micMuted) voice.cancel() }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Text {
                                text: agent.config.micMuted ? qsTr("Muted")
                                    : window.orbState === "listening" ? qsTr("Listening…")
                                    : window.orbState === "wake" ? qsTr("Wake!")
                                    : agent.config.wakeEnabled ? qsTr("Say “Shadow”") : qsTr("Voice ready")
                                color: Theme.textSecondary
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fontSizeCaption
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                            Text {
                                visible: !voice.wakeAvailable && agent.config.wakeEnabled
                                text: qsTr("wake word needs engine")
                                color: Theme.warning
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fontSizeCaption
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                        }
                        NxIconButton {
                            iconName: "signal"
                            icon.color: agent.config.wakeEnabled ? Theme.accent : Theme.textMuted
                            checkable: true
                            checked: agent.config.wakeEnabled
                            enabled: !agent.config.micMuted
                            tooltip: agent.config.wakeEnabled ? qsTr("Always-listening on — click to stop")
                                                              : qsTr("Turn on always-listening for the wake word")
                            onClicked: agent.config.wakeEnabled = !agent.config.wakeEnabled
                        }
                    }
                }

                NxButton {
                    Layout.fillWidth: true
                    text: qsTr("New conversation")
                    kind: "primary"
                    iconName: "plus"
                    enabled: !agent.busy
                    onClicked: { agent.newConversation(); window.view = "chat" }
                }

                // conversation list
                ListView {
                    id: convList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: Theme.spaceXxs
                    model: agent.conversations
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar {}

                    delegate: Rectangle {
                        id: convItem
                        required property string id
                        required property string title
                        required property string subtitle
                        width: ListView.view.width
                        height: Theme.rowHeight + Theme.spaceXs
                        radius: Theme.radiusSm
                        readonly property bool current: agent.activeId === id
                        color: current ? Theme.alpha(Theme.accent, 0.14)
                             : hover.hovered ? Theme.bgHover : "transparent"
                        border.color: current ? Theme.alpha(Theme.accent, 0.4) : "transparent"
                        border.width: 1

                        HoverHandler { id: hover }
                        TapHandler {
                            onTapped: { agent.selectConversation(convItem.id); window.view = "chat" }
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.spaceSm
                            anchors.rightMargin: Theme.spaceXs
                            spacing: Theme.spaceXs
                            NxIcon { name: "file-text"; size: Theme.iconSmall
                                     color: convItem.current ? Theme.accent : Theme.textMuted }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0
                                Text {
                                    Layout.fillWidth: true
                                    text: convItem.title
                                    color: Theme.textPrimary
                                    font.family: Theme.fontUi
                                    font.pixelSize: Theme.fontSizeSmall
                                    font.weight: convItem.current ? Theme.weightMedium : Theme.weightRegular
                                    elide: Text.ElideRight
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: convItem.subtitle
                                    color: Theme.textMuted
                                    font.family: Theme.fontUi
                                    font.pixelSize: Theme.fontSizeCaption
                                    elide: Text.ElideRight
                                }
                            }
                            NxIconButton {
                                visible: hover.hovered || convItem.current
                                iconName: "edit"; iconSize: Theme.iconSmall
                                tooltip: qsTr("Rename")
                                onClicked: { renameDialog.convId = convItem.id; renameField.text = convItem.title; renameDialog.open() }
                            }
                            NxIconButton {
                                visible: hover.hovered || convItem.current
                                iconName: "trash"; iconSize: Theme.iconSmall
                                tooltip: qsTr("Delete")
                                onClicked: { deleteDialog.convId = convItem.id; deleteDialog.open() }
                            }
                        }
                    }

                    // empty state
                    NxEmptyState {
                        anchors.centerIn: parent
                        width: parent.width - Theme.spaceMd
                        visible: agent.conversations.count === 0
                        icon: "file-text"
                        title: qsTr("No conversations")
                        message: qsTr("Start a new one above.")
                    }
                }

                NxDivider { Layout.fillWidth: true }

                // footer nav — history
                Rectangle {
                    Layout.fillWidth: true
                    height: Theme.rowHeight
                    radius: Theme.radiusSm
                    color: window.view === "history" ? Theme.alpha(Theme.accent, 0.14)
                         : historyHover.hovered ? Theme.bgHover : "transparent"
                    HoverHandler { id: historyHover }
                    TapHandler { onTapped: window.view = "history" }
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spaceSm
                        spacing: Theme.spaceSm
                        NxIcon { name: "undo"; size: Theme.iconSmall
                                 color: window.view === "history" ? Theme.accent : Theme.textSecondary }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("History & undo")
                            color: Theme.textPrimary
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeBody
                        }
                    }
                }

                // footer nav — settings
                Rectangle {
                    Layout.fillWidth: true
                    height: Theme.rowHeight
                    radius: Theme.radiusSm
                    color: window.view === "settings" ? Theme.alpha(Theme.accent, 0.14)
                         : settingsHover.hovered ? Theme.bgHover : "transparent"
                    HoverHandler { id: settingsHover }
                    TapHandler { onTapped: window.view = "settings" }
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spaceSm
                        spacing: Theme.spaceSm
                        NxIcon { name: "settings"; size: Theme.iconSmall
                                 color: window.view === "settings" ? Theme.accent : Theme.textSecondary }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Settings")
                            color: Theme.textPrimary
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeBody
                        }
                        NxBadge {
                            visible: !agent.config.apiKeyConfigured
                            text: qsTr("setup")
                            kind: "warning"
                        }
                    }
                }
            }
        }

        // ---- content ----
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            SettingsPage {
                anchors.fill: parent
                visible: window.view === "settings"
                controller: agent
                voice: voice
            }
            ActionsPage {
                anchors.fill: parent
                visible: window.view === "history"
                controller: agent
            }
            SetupPage {
                anchors.fill: parent
                visible: window.view !== "settings" && window.view !== "history" && !agent.config.apiKeyConfigured
                controller: agent
            }
            ChatPage {
                anchors.fill: parent
                visible: window.view !== "settings" && window.view !== "history" && agent.config.apiKeyConfigured
                controller: agent
                voice: voice
                orbState: window.orbState
                onRequestListen: window.toggleListen()
            }
        }
    }

    // ---- dialogs ----
    Dialog {
        id: renameDialog
        property string convId: ""
        anchors.centerIn: parent
        modal: true
        title: qsTr("Rename conversation")
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: if (renameField.text.trim() !== "") agent.renameConversation(convId, renameField.text.trim())
        TextField {
            id: renameField
            implicitWidth: 360
            placeholderText: qsTr("Conversation name")
            onAccepted: renameDialog.accept()
        }
    }

    NxConfirmDialog {
        id: deleteDialog
        property string convId: ""
        title: qsTr("Delete conversation?")
        message: qsTr("This permanently removes the conversation and its history.")
        confirmText: qsTr("Delete")
        destructive: true
        onConfirmed: agent.deleteConversation(deleteDialog.convId)
    }

    Connections {
        target: agent
        function onErrorMessage(text) {
            window.toaster.show(text.split("\n")[0], "danger")
        }
    }
}
