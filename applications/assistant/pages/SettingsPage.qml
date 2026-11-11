// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui

// API key status, model mapping, tool enablement, and autonomy level.
NxPage {
    id: settings
    property var controller
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
        title: qsTr("Autonomy")
        description: qsTr("Actions that change the system or reach the network are always confirmed, whatever this is set to.")
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceMd
            NxSegmented {
                model: [qsTr("Ask every time"), qsTr("Ask for risky only")]
                currentIndex: settings.config && settings.config.autonomy === "ask_risky" ? 1 : 0
                onActivated: (index) => settings.config.autonomy = index === 1 ? "ask_risky" : "ask_every"
            }
            Item { Layout.fillWidth: true }
        }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            text: settings.config && settings.config.autonomy === "ask_risky"
                  ? qsTr("Read-only tools (read a file, search, system info) run automatically; anything that changes the system still asks.")
                  : qsTr("Every tool call waits for your explicit approval.")
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
        title: qsTr("Voice")
        NxRow {
            icon: "mic-off"
            iconColor: Theme.textMuted
            title: qsTr("Push-to-talk & speech")
            subtitle: qsTr("Scaffolded with the omni model, but disabled: no tested speech backend (e.g. speech-dispatcher) in this build.")
            NxBadge { text: qsTr("Unavailable"); kind: "neutral" }
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
