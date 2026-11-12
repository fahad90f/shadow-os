// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui

// API key status, model mapping, tool enablement, and autonomy level.
NxPage {
    id: settings
    property var controller
    property var voice
    readonly property var config: controller ? controller.config : null
    title: qsTr("Settings")
    subtitle: qsTr("Shadow Assistant · NVIDIA Nemotron")
    maxContentWidth: 820

    actions: [
        NxButton {
            text: qsTr("Reload config")
            iconName: "refresh"
            onClicked: {
                settings.controller.reloadConfig()
                ApplicationWindow.window.toaster.show(qsTr("Configuration reloaded"), "info")
            }
        }
    ]

    NxSection {
        title: qsTr("API key")
        description: qsTr("The key is read from the environment or %1 and is never stored by this application.")
                     .arg(settings.config ? settings.config.configPath : "")
        NxRow {
            icon: settings.config && settings.config.apiKeyConfigured ? "shield-check" : "shield-alert"
            iconColor: settings.config && settings.config.apiKeyConfigured ? Theme.success : Theme.warning
            title: qsTr("Status")
            NxBadge {
                text: settings.config && settings.config.apiKeyConfigured ? qsTr("Configured") : qsTr("Not configured")
                kind: settings.config && settings.config.apiKeyConfigured ? "success" : "warning"
            }
        }
        NxRow {
            visible: settings.config && settings.config.apiKeyConfigured
            icon: "key"; title: qsTr("Source")
            value: settings.config ? settings.config.apiKeySource : ""
        }
        NxRow {
            icon: "globe"; title: qsTr("Endpoint")
            value: settings.config ? settings.config.baseUrl : ""
            monoValue: true
            subtitle: settings.config && settings.config.baseUrlOverridden
                      ? qsTr("overridden by SHADOW_ASSISTANT_BASE_URL") : ""
        }
    }

    NxSection {
        title: qsTr("Models")
        description: qsTr("Edit these in the configuration file to match the model IDs on your NVIDIA account. Normal chat uses brain; “Think hard” uses planner.")
        NxRow { icon: "cpu"; title: qsTr("Brain (chat, tools, vision)"); value: settings.config ? settings.config.brainModel : ""; monoValue: true }
        NxRow { icon: "lab"; title: qsTr("Planner (deep reasoning)"); value: settings.config ? settings.config.plannerModel : ""; monoValue: true }
        NxRow { icon: "zap"; title: qsTr("Fast (background helpers)"); value: settings.config ? settings.config.fastModel : ""; monoValue: true }
        NxRow { icon: "mic"; title: qsTr("Omni (voice & image)"); value: settings.config ? settings.config.omniModel : ""; monoValue: true }
    }

    NxSection {
        title: qsTr("Mode")
        description: qsTr("Switch the assistant's persona and available tools.")
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceMd
            NxSegmented {
                model: [qsTr("General"), qsTr("Pentest")]
                currentIndex: settings.config && settings.config.role === "pentest" ? 1 : 0
                onActivated: (index) => settings.config.role = index === 1 ? "pentest" : "general"
            }
            Item { Layout.fillWidth: true }
        }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: settings.config && settings.config.role === "pentest"
                  ? qsTr("Penetration Testing mode. Activates the offensive security system prompt and seven pentest tools (nmap profiles, web enumeration, exploit search, SQLi, hash cracking, service brute-force, packet capture). Use only on systems you own or are contracted in writing to test.")
                  : qsTr("Standard assistant with full productivity tools.")
            color: settings.config && settings.config.role === "pentest" ? Theme.warning : Theme.textMuted
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
        }
    }

    NxSection {
        title: qsTr("Autonomy")
        description: qsTr("How much the assistant asks before acting.")
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceMd
            NxSegmented {
                model: [qsTr("Ask every time"), qsTr("Ask for risky only"), qsTr("Autopilot")]
                currentIndex: !settings.config ? 0
                              : settings.config.autonomy === "autopilot" ? 2
                              : settings.config.autonomy === "ask_risky" ? 1 : 0
                onActivated: (index) => settings.config.autonomy =
                    index === 2 ? "autopilot" : index === 1 ? "ask_risky" : "ask_every"
            }
            Item { Layout.fillWidth: true }
        }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: !settings.config ? ""
                  : settings.config.autonomy === "autopilot"
                  ? qsTr("Runs every command and action with no confirmation, however risky. Only a few hard safety rails remain (it will not delete the system root). Use with care.")
                  : settings.config.autonomy === "ask_risky"
                  ? qsTr("Read-only tools (read a file, search, system info) run automatically; anything that changes the system still asks.")
                  : qsTr("Every tool call waits for your explicit approval.")
            color: settings.config && settings.config.autonomy === "autopilot" ? Theme.danger : Theme.textMuted
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
        }
    }

    NxSection {
        title: qsTr("Voice")
        description: qsTr("Hands-free “Shadow”. Say the wake word, speak a command, and hear the reply. Nothing is captured or sent until after the wake word, and never while muted.")

        NxRow {
            icon: settings.voice && settings.voice.anyEngine ? "sound" : "volume-off"
            iconColor: settings.voice && settings.voice.anyEngine ? Theme.success : Theme.warning
            title: qsTr("Status")
            subtitle: settings.voice ? settings.voice.statusText : ""
        }

        NxRow {
            icon: "mic-off"
            title: qsTr("Privacy mute")
            subtitle: qsTr("When on, the microphone is never opened.")
            Switch {
                checked: settings.config ? settings.config.micMuted : false
                onToggled: settings.config.micMuted = checked
            }
        }
        NxRow {
            icon: "signal"
            title: qsTr("Always-listening wake word")
            subtitle: settings.voice && !settings.voice.wakeAvailable
                      ? qsTr("Needs pocketsphinx + an en-us model and a capture tool. Off until available.")
                      : qsTr("Opt-in. Listens locally for “%1”.").arg(settings.config ? settings.config.wakePhrase : "shadow")
            Switch {
                enabled: settings.config && !settings.config.micMuted && settings.voice && settings.voice.wakeAvailable
                checked: settings.config ? settings.config.wakeEnabled : false
                onToggled: settings.config.wakeEnabled = checked
            }
        }
        NxRow {
            icon: "volume-high"
            title: qsTr("Speak replies aloud")
            Switch {
                checked: settings.config ? settings.config.speakReplies : false
                onToggled: settings.config.speakReplies = checked
            }
        }

        NxRow {
            icon: "sound"
            title: qsTr("Speech engine")
            subtitle: settings.voice
                ? qsTr("Resolved: %1").arg(settings.voice.resolvedTtsEngine === "" ? qsTr("none installed") : settings.voice.resolvedTtsEngine)
                : ""
            NxSegmented {
                model: ["Auto", "Piper", "Speech-dispatcher", "eSpeak"]
                currentIndex: {
                    if (!settings.config) return 0
                    switch (settings.config.ttsEngine) {
                    case "piper": return 1
                    case "speechd": return 2
                    case "espeak": return 3
                    default: return 0
                    }
                }
                onActivated: (index) => settings.config.ttsEngine =
                    ["auto", "piper", "speechd", "espeak"][index]
            }
        }

        NxRow {
            icon: "info"
            title: qsTr("Engines detected")
            subtitle: qsTr("Speech-to-text (pocketsphinx), text-to-speech, wake word, and keyboard typing.")
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spaceLg
            spacing: Theme.spaceXs
            NxBadge { text: qsTr("TTS"); kind: settings.voice && settings.voice.ttsAvailable ? "success" : "neutral" }
            NxBadge { text: qsTr("STT"); kind: settings.voice && settings.voice.sttAvailable ? "success" : "neutral" }
            NxBadge { text: qsTr("Wake"); kind: settings.voice && settings.voice.wakeAvailable ? "success" : "neutral" }
            NxBadge { text: qsTr("Capture: %1").arg(settings.voice && settings.voice.resolvedCaptureTool !== "" ? settings.voice.resolvedCaptureTool : qsTr("none"));
                      kind: settings.voice && settings.voice.captureAvailable ? "success" : "neutral" }
            NxBadge { text: qsTr("Type"); kind: settings.voice && settings.voice.typeAvailable ? "success" : "neutral" }
            Item { Layout.fillWidth: true }
        }
        Text {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spaceLg
            wrapMode: Text.Wrap
            text: qsTr("Engine paths, the piper voice model, the wake phrase/threshold and the capture device are set in the [voice] section of the configuration file.")
            color: Theme.textMuted
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
        }
    }

    NxSection {
        title: qsTr("Tools")
        description: qsTr("Which capabilities the assistant may call. A disabled tool is never offered to the model.")
        Repeater {
            model: [
                { name: "run_shell_command", title: qsTr("Run shell commands"), icon: "terminal", risky: true },
                { name: "read_file", title: qsTr("Read files"), icon: "file-text", risky: false },
                { name: "write_file", title: qsTr("Write files"), icon: "edit", risky: false },
                { name: "search_files", title: qsTr("Search files"), icon: "file-search", risky: false },
                { name: "open_application", title: qsTr("Open applications"), icon: "apps", risky: false },
                { name: "get_system_info", title: qsTr("Read system info"), icon: "info", risky: false },
                { name: "open_url", title: qsTr("Open URLs / browser"), icon: "globe", risky: false },
                { name: "web_search", title: qsTr("Web search"), icon: "search", risky: false },
                { name: "make_directory", title: qsTr("Create folders"), icon: "folder", risky: false },
                { name: "move_path", title: qsTr("Move / rename"), icon: "folder-open", risky: true },
                { name: "copy_path", title: qsTr("Copy files / folders"), icon: "copy", risky: false },
                { name: "delete_path", title: qsTr("Delete files / folders"), icon: "trash", risky: true },
                { name: "write_code", title: qsTr("Write code"), icon: "file-text", risky: false },
                { name: "run_in_terminal", title: qsTr("Run in terminal"), icon: "terminal", risky: true },
                { name: "type_text", title: qsTr("Type into focused window"), icon: "edit", risky: true },
                { name: "take_screenshot", title: qsTr("Take screenshots"), icon: "camera", risky: false },
            ]
            delegate: NxRow {
                required property var modelData
                icon: modelData.icon
                title: modelData.title
                subtitle: modelData.risky ? qsTr("Always requires confirmation") : ""
                Switch {
                    checked: settings.config ? settings.config.toolEnabled(modelData.name) : false
                    onToggled: settings.config.setToolEnabled(modelData.name, checked)
                }
            }
        }
    }

    NxSection {
        title: qsTr("Activity log")
        description: qsTr("Every tool call this session, most recent first.")
        NxEmptyState {
            visible: !settings.controller || settings.controller.toolLog.length === 0
            Layout.fillWidth: true
            icon: "list"
            title: qsTr("No tool activity yet")
            message: qsTr("Actions the assistant takes will be logged here.")
        }
        Repeater {
            model: settings.controller ? settings.controller.toolLog : []
            delegate: NxRow {
                required property var modelData
                icon: modelData.status === "done" ? "check-circle"
                    : modelData.status === "error" ? "x-circle"
                    : modelData.status === "denied" ? "shield-off" : "activity"
                iconColor: modelData.status === "done" ? Theme.success
                    : modelData.status === "error" ? Theme.danger : Theme.textSecondary
                title: modelData.name
                subtitle: modelData.preview
                value: modelData.time
                monoValue: true
            }
        }
    }
}
