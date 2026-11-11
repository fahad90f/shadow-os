// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.files

// "Compress…": archive name and format for a selection. The archive is
// created by bsdtar in the folder of the selection.
Dialog {
    id: dialog

    property var urls: []
    property url destination
    // FolderModel of the destination, to check for an existing name.
    property var folder: null

    readonly property string suffix: Archiver.formats[formatBox.currentIndex].suffix
    readonly property string fileName: nameField.text.trim() + suffix
    readonly property string problem: {
        const base = nameField.text.trim()
        if (base === "") return ""
        const invalid = FileOps.validateName(fileName)
        if (invalid !== "") return invalid
        if (folder && folder.nameExists(fileName)) return qsTr("“%1” already exists in this folder.").arg(fileName)
        return ""
    }

    function show(urls, destination, folder) {
        dialog.urls = urls
        dialog.destination = destination
        dialog.folder = folder
        nameField.text = Archiver.suggestedName(urls)
        open()
    }

    function confirm() {
        if (nameField.text.trim() === "" || dialog.problem !== "") return
        Archiver.compress(dialog.urls, dialog.destination, nameField.text.trim(), dialog.suffix)
        close()
    }

    title: dialog.urls.length === 1 ? qsTr("Compress 1 item") : qsTr("Compress %1 items").arg(dialog.urls.length)
    width: Math.min(460, parent ? parent.width - Theme.spaceXxl : 460)
    closePolicy: Popup.CloseOnEscape
    footer: null
    onOpened: { nameField.forceActiveFocus(); nameField.selectAll() }

    contentItem: ColumnLayout {
        spacing: Theme.spaceMd

        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: Theme.spaceMd
            rowSpacing: Theme.spaceSm

            Label { text: qsTr("Archive name"); color: Theme.textMuted }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceXs
                TextField {
                    id: nameField
                    Layout.fillWidth: true
                    selectByMouse: true
                    Accessible.name: qsTr("Archive name")
                    onAccepted: dialog.confirm()
                }
                Text {
                    text: dialog.suffix
                    color: Theme.textSecondary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fontSizeSmall
                }
            }

            Label { text: qsTr("Format"); color: Theme.textMuted }
            ComboBox {
                id: formatBox
                Layout.fillWidth: true
                model: Archiver.formats.map(f => f.label)
                Accessible.name: qsTr("Format")
            }

            Label { text: qsTr("Save in"); color: Theme.textMuted }
            Text {
                Layout.fillWidth: true
                text: FileOps.displayPath(dialog.destination)
                color: Theme.textPrimary
                font.family: Theme.fontMono
                font.pixelSize: Theme.fontSizeSmall
                elide: Text.ElideMiddle
            }
        }

        Text {
            Layout.fillWidth: true
            opacity: dialog.problem !== "" ? 1 : 0
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
                text: qsTr("Compress")
                kind: "primary"
                iconName: "archive"
                enabled: nameField.text.trim() !== "" && dialog.problem === ""
                onClicked: dialog.confirm()
            }
        }
    }
}
