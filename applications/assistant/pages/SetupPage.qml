// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import "../components"

// Shown when no NVIDIA API key is configured. Explains, honestly and without
// blocking, exactly which file to create and what to put in it. Never crashes
// or pretends to work.
NxPage {
    id: setup
    property var controller
    title: qsTr("Welcome to Shadow Assistant")
    subtitle: qsTr("One step before we begin: add your NVIDIA API key")
    maxContentWidth: 760

    NxCard {
        Layout.fillWidth: true
        padding: Theme.spaceLg
        ColumnLayout {
            width: parent.width
            spacing: Theme.spaceMd
            AssistantOrb {
                Layout.alignment: Qt.AlignHCenter
                orbState: "idle"
                implicitWidth: 120; implicitHeight: 120
            }
            Text {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: qsTr("Shadow Assistant talks to NVIDIA's Nemotron models over their OpenAI-compatible API. Your key is read at run time and is never stored in the application.")
                color: Theme.textSecondary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
            }
        }
    }

    NxSection {
        title: qsTr("1 · Create the configuration file")
        description: qsTr("Create this file with owner-only permissions (chmod 600). You can also set the SHADOW_ASSISTANT_API_KEY environment variable instead.")
        NxKeyValue {
            label: qsTr("Path")
            value: setup.controller ? setup.controller.config.configPath : ""
            mono: true
        }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            NxButton {
                text: qsTr("Copy path")
                iconName: "copy"
                onClicked: setup.controller.copyToClipboard(setup.controller.config.configPath)
            }
        }
    }

    NxSection {
        title: qsTr("2 · Paste this template and add your key")
        headerActions: [
            NxButton {
                text: qsTr("Copy template")
                iconName: "copy"
                onClicked: {
                    setup.controller.copyToClipboard(setup.controller.config.configTemplate)
                    ApplicationWindow.window.toaster.show(qsTr("Template copied to the clipboard"), "success")
                }
            }
        ]
        Rectangle {
            Layout.fillWidth: true
            color: Theme.bgInput
            radius: Theme.radiusSm
            border.color: Theme.borderSubtle
            border.width: 1
            implicitHeight: tmpl.implicitHeight + Theme.spaceMd * 2
            TextEdit {
                id: tmpl
                anchors.fill: parent
                anchors.margins: Theme.spaceMd
                text: setup.controller ? setup.controller.config.configTemplate : ""
                readOnly: true
                selectByMouse: true
                wrapMode: Text.NoWrap
                textFormat: TextEdit.PlainText
                color: Theme.textPrimary
                font.family: Theme.fontMono
                font.pixelSize: Theme.fontSizeSmall
            }
        }
    }

    NxSection {
        title: qsTr("3 · Reload")
        NxBanner {
            Layout.fillWidth: true
            kind: "info"
            title: qsTr("Already added your key?")
            message: qsTr("Click reload to pick up the new configuration.")
            actionText: qsTr("Reload configuration")
            onActionTriggered: {
                setup.controller.reloadConfig()
                if (setup.controller.config.apiKeyConfigured)
                    ApplicationWindow.window.toaster.show(qsTr("API key found — you're ready to go"), "success")
                else
                    ApplicationWindow.window.toaster.show(qsTr("Still no key found at the expected location"), "warning")
            }
        }
    }
}
