// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui

// Asks for a secret (a passphrase, or a new password typed twice). The text
// is handed to the caller once through accepted(secret) and the fields are
// emptied; it is never kept in a property.
Dialog {
    id: root

    property string message: ""
    property string fieldLabel: qsTr("Passphrase")
    property string confirmText: qsTr("Continue")
    // Ask twice and compare (for a new password).
    property bool repeat: false
    property bool destructive: false
    // Returns why (first, second) cannot be used, or "" — set by the caller.
    property var validate: function(first, second) { return first === "" ? " " : "" }
    // Optional choice shown above the fields (user names); empty = hidden.
    property var choices: []
    property alias choiceIndex: choice.currentIndex

    signal secretAccepted(string secret)

    width: Math.min(480, parent ? parent.width - Theme.spaceXxl : 480)
    closePolicy: Popup.CloseOnEscape
    footer: null
    onOpened: { first.text = ""; second.text = ""; first.forceActiveFocus() }
    onClosed: { first.text = ""; second.text = "" }

    readonly property string problem: validate(first.text, repeat ? second.text : first.text)

    function submit() {
        if (problem !== "")
            return
        const secret = first.text
        first.text = ""
        second.text = ""
        close()
        secretAccepted(secret)
    }

    contentItem: ColumnLayout {
        spacing: Theme.spaceMd

        Text {
            Layout.fillWidth: true
            text: root.message
            color: Theme.textSecondary
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeBody
            wrapMode: Text.WordWrap
        }
        ComboBox {
            id: choice
            Layout.fillWidth: true
            visible: root.choices.length > 0
            model: root.choices
            Accessible.name: qsTr("User account")
        }
        NxPasswordField {
            id: first
            Layout.fillWidth: true
            placeholderText: root.fieldLabel
            Accessible.name: root.fieldLabel
            onAccepted: root.repeat ? second.forceActiveFocus() : root.submit()
        }
        NxPasswordField {
            id: second
            Layout.fillWidth: true
            visible: root.repeat
            placeholderText: qsTr("Repeat")
            Accessible.name: qsTr("Repeat the new password")
            onAccepted: root.submit()
        }
        Text {
            Layout.fillWidth: true
            visible: root.repeat && first.text !== "" && second.text !== "" && root.problem.trim() !== ""
            text: root.problem
            color: Theme.warning
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
            wrapMode: Text.WordWrap
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Theme.spaceXs
            spacing: Theme.spaceSm
            Item { Layout.fillWidth: true }
            NxButton { text: qsTr("Cancel"); onClicked: root.close() }
            NxButton {
                text: root.confirmText
                kind: root.destructive ? "danger" : "primary"
                enabled: root.problem === ""
                onClicked: root.submit()
            }
        }
    }
}
