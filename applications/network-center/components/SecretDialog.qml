// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui

// Asks for one secret (Wi-Fi passphrase, VPN password). The text is handed
// to the caller through submitted() and cleared as soon as the dialog closes.
Dialog {
    id: root

    property string message: ""
    property string placeholder: qsTr("Password")
    property string confirmText: qsTr("Connect")
    // Optional function(text) returning "" or the reason the secret is unusable.
    property var validator: null
    readonly property string problem: validator ? validator(field.text) : (field.text === "" ? " " : "")

    signal submitted(string secret)

    width: Math.min(460, parent ? parent.width - Theme.spaceXxl : 460)
    closePolicy: Popup.CloseOnEscape
    footer: null

    onOpened: { field.text = ""; field.revealed = false; field.forceActiveFocus() }
    onClosed: field.text = ""

    function submit() {
        if (root.problem !== "")
            return
        const secret = field.text
        root.accept()
        root.submitted(secret)
    }

    contentItem: ColumnLayout {
        spacing: Theme.spaceMd

        Label {
            Layout.fillWidth: true
            visible: root.message !== ""
            text: root.message
            color: Theme.textSecondary
            wrapMode: Text.WordWrap
        }
        NxPasswordField {
            id: field
            Layout.fillWidth: true
            placeholderText: root.placeholder
            Accessible.name: root.placeholder
            onAccepted: root.submit()
        }
        Label {
            Layout.fillWidth: true
            visible: field.text !== "" && root.problem.trim() !== ""
            text: root.problem
            color: Theme.danger
            font.pixelSize: Theme.fontSizeSmall
            wrapMode: Text.WordWrap
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Theme.spaceXs
            spacing: Theme.spaceSm
            Item { Layout.fillWidth: true }
            NxButton {
                text: qsTr("Cancel")
                onClicked: root.reject()
            }
            NxButton {
                text: root.confirmText
                kind: "primary"
                enabled: root.problem === ""
                onClicked: root.submit()
            }
        }
    }
}
