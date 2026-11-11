// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui

// Asks for one name: new folder, new file, bookmark label.
Dialog {
    id: dialog

    property string label: ""
    property string confirmText: qsTr("Create")
    // function(name) → "" if acceptable, otherwise the reason shown under the field
    property var validate: null
    // function(name), called with the trimmed name on confirm
    property var handler: null
    // Select only the part before the extension (file names).
    property bool selectBaseName: false

    readonly property string problem: validate ? validate(field.text) : ""

    function ask(title, label, initial, confirmText, validate, handler, selectBaseName) {
        dialog.title = title
        dialog.label = label
        dialog.confirmText = confirmText
        dialog.validate = validate
        dialog.handler = handler
        dialog.selectBaseName = selectBaseName === true
        field.text = initial
        open()
    }

    function confirm() {
        if (field.text.trim() === "" || dialog.problem !== "") return
        const name = field.text.trim()
        const fn = dialog.handler
        close()
        if (fn) fn(name)
    }

    width: Math.min(420, parent ? parent.width - Theme.spaceXxl : 420)
    closePolicy: Popup.CloseOnEscape
    footer: null

    onOpened: {
        field.forceActiveFocus()
        const dot = dialog.selectBaseName ? field.text.lastIndexOf(".") : -1
        if (dot > 0) field.select(0, dot)
        else field.selectAll()
    }

    contentItem: ColumnLayout {
        spacing: Theme.spaceSm

        Text {
            Layout.fillWidth: true
            visible: dialog.label !== ""
            text: dialog.label
            color: Theme.textSecondary
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeBody
            wrapMode: Text.WordWrap
        }
        TextField {
            id: field
            Layout.fillWidth: true
            selectByMouse: true
            Accessible.name: dialog.label
            onAccepted: dialog.confirm()
        }
        Text {
            Layout.fillWidth: true
            // Keeps its height so the dialog does not jump while typing.
            opacity: dialog.problem !== "" && field.text.trim() !== "" ? 1 : 0
            text: dialog.problem !== "" ? dialog.problem : " "
            color: Theme.danger
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
            wrapMode: Text.WordWrap
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceSm
            Item { Layout.fillWidth: true }
            NxButton {
                text: qsTr("Cancel")
                onClicked: dialog.close()
            }
            NxButton {
                text: dialog.confirmText
                kind: "primary"
                enabled: field.text.trim() !== "" && dialog.problem === ""
                onClicked: dialog.confirm()
            }
        }
    }
}
