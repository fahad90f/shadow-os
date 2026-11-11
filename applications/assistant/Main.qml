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

    // "chat" | "settings". The setup state replaces the chat page when no key.
    property string view: "chat"

    Component.onCompleted: {
        if (agent.conversations.count > 0)
            agent.selectConversation(agent.conversations.data(agent.conversations.index(0, 0), 257 /*IdRole*/))
        else
            agent.newConversation()
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
                    AssistantOrb { orbState: agent.orbState; implicitWidth: 32; implicitHeight: 32 }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Shadow Assistant")
                        color: Theme.textPrimary
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeBodyLarge
                        font.weight: Theme.weightSemibold
                        elide: Text.ElideRight
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

                // footer nav
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
            }
            SetupPage {
                anchors.fill: parent
                visible: window.view !== "settings" && !agent.config.apiKeyConfigured
                controller: agent
            }
            ChatPage {
                anchors.fill: parent
                visible: window.view !== "settings" && agent.config.apiKeyConfigured
                controller: agent
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
        function onErrorMessage(text) { window.toaster.show(text.split("\n")[0], "danger") }
    }
}
