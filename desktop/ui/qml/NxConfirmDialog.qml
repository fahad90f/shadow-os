// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Confirmation for consequential actions. Destructive confirmations use the
// danger colour and can require typing a phrase (e.g. the disk name) first.
Dialog {
    id: root

    property string message: ""
    property string confirmText: qsTr("Confirm")
    property string cancelText: qsTr("Cancel")
    property bool destructive: false
    property string requiredPhrase: ""      // non-empty → must be typed to enable confirm

    signal confirmed()

    width: Math.min(460, parent ? parent.width - Theme.spaceXxl : 460)
    closePolicy: Popup.CloseOnEscape
    footer: null

    onOpened: { phrase.text = ""; if (root.requiredPhrase !== "") phrase.forceActiveFocus() }

    contentItem: ColumnLayout {
        spacing: Theme.spaceLg

        Text {
            Layout.fillWidth: true
            text: root.message
            color: Theme.textSecondary
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeBody
            wrapMode: Text.WordWrap
        }
        ColumnLayout {
            Layout.fillWidth: true
            visible: root.requiredPhrase !== ""
            spacing: Theme.spaceXs
            Text {
                Layout.fillWidth: true
                text: qsTr("Type “%1” to confirm:").arg(root.requiredPhrase)
                color: Theme.textMuted
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WordWrap
            }
            TextField {
                id: phrase
                Layout.fillWidth: true
                font.family: Theme.fontMono
                onAccepted: if (confirm.enabled) confirm.clicked()
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Theme.spaceXs
            spacing: Theme.spaceSm
            Item { Layout.fillWidth: true }
            NxButton {
                text: root.cancelText
                onClicked: root.reject()
            }
            NxButton {
                id: confirm
                text: root.confirmText
                kind: root.destructive ? "danger" : "primary"
                enabled: root.requiredPhrase === "" || phrase.text === root.requiredPhrase
                onClicked: { root.confirmed(); root.accept() }
            }
        }
    }
}
