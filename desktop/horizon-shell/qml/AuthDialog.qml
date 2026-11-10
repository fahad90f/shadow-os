// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui

// Authentication prompt of the session's polkit agent. It is shown on the
// overlay layer with exclusive keyboard focus, so no application window can
// cover it or read what is typed into it.
OverlaySurface {
    id: root

    scope: "nexus-authentication"
    scrimOpacity: 0.72
    modal: true
    open: polkitAgent.active

    onOpenChanged: if (open) { secret.text = ""; secret.forceActiveFocus() }

    function submit() {
        if (polkitAgent.busy) return
        polkitAgent.respond(secret.text)
        secret.text = ""
    }

    Connections {
        target: polkitAgent
        function onPromptChanged() { secret.text = ""; secret.forceActiveFocus() }
    }

    Rectangle {
        anchors.centerIn: parent
        width: 480
        height: column.implicitHeight + Theme.spaceXl * 2
        radius: Theme.radiusLg
        color: Theme.bgOverlay
        border.width: Theme.borderWidth
        border.color: Theme.border

        ColumnLayout {
            id: column
            anchors.fill: parent
            anchors.margins: Theme.spaceXl
            spacing: Theme.spaceLg

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceLg
                Rectangle {
                    implicitWidth: 48; implicitHeight: 48; radius: 24
                    color: Theme.accentFill
                    NxIcon { anchors.centerIn: parent; name: "shield"; size: 24; color: Theme.accent }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Authentication required")
                        color: Theme.textPrimary
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeTitle
                        font.weight: Theme.weightSemibold
                    }
                    Text {
                        Layout.fillWidth: true
                        text: polkitAgent.message
                        color: Theme.textSecondary
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeBody
                        wrapMode: Text.WordWrap
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceXs
                Text {
                    text: polkitAgent.identities.length > 1 ? qsTr("Authenticate as") : qsTr("Authenticating as")
                    color: Theme.textMuted
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                }
                ComboBox {
                    Layout.fillWidth: true
                    visible: polkitAgent.identities.length > 1
                    model: polkitAgent.identities
                    textRole: "fullName"
                    currentIndex: polkitAgent.identityIndex
                    onActivated: (index) => polkitAgent.selectIdentity(index)
                }
                Text {
                    visible: polkitAgent.identities.length === 1
                    text: polkitAgent.identities.length === 1
                        ? polkitAgent.identities[0].fullName + " (" + polkitAgent.identities[0].name + ")" : ""
                    color: Theme.textPrimary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBody
                    font.weight: Theme.weightMedium
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceXs
                Text {
                    text: polkitAgent.prompt !== "" ? polkitAgent.prompt : qsTr("Password:")
                    color: Theme.textMuted
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                }
                NxPasswordField {
                    id: secret
                    Layout.fillWidth: true
                    enabled: !polkitAgent.busy
                    revealed: polkitAgent.echo
                    onAccepted: root.submit()
                    Keys.onEscapePressed: polkitAgent.cancel()
                }
                Text {
                    Layout.fillWidth: true
                    visible: polkitAgent.errorText !== ""
                    text: polkitAgent.errorText
                    color: Theme.danger
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                    wrapMode: Text.WordWrap
                }
                Text {
                    Layout.fillWidth: true
                    visible: polkitAgent.infoText !== ""
                    text: polkitAgent.infoText
                    color: Theme.textSecondary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                    wrapMode: Text.WordWrap
                }
            }

            Text {
                Layout.fillWidth: true
                text: qsTr("Action: %1").arg(polkitAgent.actionId)
                color: Theme.textMuted
                font.family: Theme.fontMono
                font.pixelSize: Theme.fontSizeCaption
                elide: Text.ElideMiddle
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceSm
                Item { Layout.fillWidth: true }
                NxButton {
                    text: qsTr("Cancel")
                    onClicked: polkitAgent.cancel()
                }
                NxButton {
                    text: qsTr("Authenticate")
                    kind: "primary"
                    busy: polkitAgent.busy
                    onClicked: root.submit()
                }
            }
        }
    }
}
