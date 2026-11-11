// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui

// Confirmation for the actions that destroy data: format, delete partition,
// new partition table, restore image. It names the device, lists exactly
// what will be lost and only enables the action once the device name has
// been typed.
Dialog {
    id: root

    // "format" | "delete" | "table" | "restore"
    property string action: ""
    property string device: ""              // e.g. /dev/sdb1 — also the phrase to type
    property string deviceDescription: ""   // e.g. "SanDisk Ultra — 32 GB — USB"
    property var losses: []                 // lines describing what will be destroyed
    property var formatTypes: []
    property var imageInfo: ({})            // restore: info about the typed image file
    property double deviceSize: 0

    readonly property string fsType: typeBox.currentText
    readonly property string label: labelField.text
    readonly property string tableType: tableBox.currentIndex === 0 ? "gpt" : "dos"
    readonly property string imagePath: imageField.text

    signal confirmed()
    signal imagePathEdited(string path)

    function present() {
        phrase.text = ""
        labelField.text = ""
        imageField.text = ""
        tableBox.currentIndex = 0
        typeBox.currentIndex = Math.max(0, root.formatTypes.indexOf("ext4"))
        open()
        if (action === "format") typeBox.forceActiveFocus()
        else if (action === "restore") imageField.forceActiveFocus()
        else phrase.forceActiveFocus()
    }

    readonly property bool inputValid: action === "format" ? typeBox.currentIndex >= 0
        : action === "restore" ? (imageInfo.exists === true && imageInfo.readable === true && imageInfo.size > 0 && imageInfo.size <= deviceSize)
        : true

    title: action === "format" ? qsTr("Format %1").arg(device)
         : action === "delete" ? qsTr("Delete %1").arg(device)
         : action === "table" ? qsTr("New partition table on %1").arg(device)
         : qsTr("Restore an image to %1").arg(device)
    anchors.centerIn: parent
    width: Math.min(560, parent ? parent.width - Theme.spaceXxl : 560)
    modal: true
    closePolicy: Popup.CloseOnEscape
    footer: null

    contentItem: ColumnLayout {
        spacing: Theme.spaceMd

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textPrimary
            font.weight: Theme.weightSemibold
            text: root.device + (root.deviceDescription !== "" ? " — " + root.deviceDescription : "")
        }

        // ---- action-specific input ---------------------------------------------
        GridLayout {
            Layout.fillWidth: true
            visible: root.action === "format"
            columns: 2
            columnSpacing: Theme.spaceMd
            rowSpacing: Theme.spaceSm
            Label { text: qsTr("File system"); color: Theme.textSecondary }
            ComboBox {
                id: typeBox
                Layout.fillWidth: true
                model: root.formatTypes
                Accessible.name: qsTr("File system type")
            }
            Label { text: qsTr("Label"); color: Theme.textSecondary }
            TextField {
                id: labelField
                Layout.fillWidth: true
                maximumLength: 32
                placeholderText: qsTr("Optional name of the volume")
                Accessible.name: qsTr("Volume label")
            }
        }
        RowLayout {
            Layout.fillWidth: true
            visible: root.action === "table"
            spacing: Theme.spaceMd
            Label { text: qsTr("Partitioning"); color: Theme.textSecondary }
            NxSegmented {
                id: tableBox
                model: [qsTr("GPT"), qsTr("MBR (DOS)")]
                onActivated: (index) => currentIndex = index
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textMuted
                font.pixelSize: Theme.fontSizeSmall
                text: tableBox.currentIndex === 0 ? qsTr("For current computers (UEFI) and disks of any size.")
                                                  : qsTr("For old BIOS computers and devices; up to 2 TiB.")
            }
        }
        ColumnLayout {
            Layout.fillWidth: true
            visible: root.action === "restore"
            spacing: Theme.spaceXs
            Label { text: qsTr("Image file to write onto the device"); color: Theme.textSecondary }
            TextField {
                id: imageField
                Layout.fillWidth: true
                font.family: Theme.fontMono
                placeholderText: qsTr("/path/to/disk.img")
                Accessible.name: qsTr("Image file")
                onTextChanged: root.imagePathEdited(text)
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontSizeSmall
                color: imageField.text.trim() === "" ? Theme.textMuted : root.inputValid ? Theme.success : Theme.danger
                text: imageField.text.trim() === "" ? qsTr("Type the full path of the image.")
                    : root.imageInfo.exists !== true ? qsTr("This file does not exist.")
                    : root.imageInfo.readable !== true ? qsTr("This file cannot be read.")
                    : root.imageInfo.size === 0 ? qsTr("This file is empty.")
                    : root.imageInfo.size > root.deviceSize ? qsTr("The image (%1) is larger than the device (%2).").arg(Theme.formatBytes(root.imageInfo.size)).arg(Theme.formatBytes(root.deviceSize))
                    : qsTr("Image of %1; the device holds %2.").arg(Theme.formatBytes(root.imageInfo.size)).arg(Theme.formatBytes(root.deviceSize))
            }
        }

        // ---- what will be lost -------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: lossColumn.implicitHeight + Theme.spaceMd * 2
            radius: Theme.radiusMd
            color: Theme.alpha(Theme.danger, 0.10)
            border.width: Theme.borderWidth
            border.color: Theme.alpha(Theme.danger, 0.40)
            ColumnLayout {
                id: lossColumn
                anchors.fill: parent
                anchors.margins: Theme.spaceMd
                spacing: Theme.spaceXs
                RowLayout {
                    spacing: Theme.spaceSm
                    NxIcon { name: "alert-triangle"; color: Theme.danger; size: Theme.icon }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("This will be permanently destroyed:")
                        color: Theme.textPrimary
                        font.weight: Theme.weightSemibold
                        wrapMode: Text.WordWrap
                    }
                }
                Repeater {
                    model: root.losses
                    Label {
                        required property string modelData
                        Layout.fillWidth: true
                        Layout.leftMargin: Theme.icon + Theme.spaceSm
                        text: "• " + modelData
                        color: Theme.textPrimary
                        wrapMode: Text.WordWrap
                    }
                }
                Label {
                    Layout.fillWidth: true
                    Layout.leftMargin: Theme.icon + Theme.spaceSm
                    text: qsTr("This cannot be undone.")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSizeSmall
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceXs
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSizeSmall
                text: qsTr("Type the device name “%1” to confirm:").arg(root.device)
            }
            TextField {
                id: phrase
                Layout.fillWidth: true
                font.family: Theme.fontMono
                Accessible.name: qsTr("Device name, to confirm")
                onAccepted: if (confirm.enabled) confirm.clicked()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Theme.spaceXs
            spacing: Theme.spaceSm
            Item { Layout.fillWidth: true }
            NxButton { text: qsTr("Cancel"); onClicked: root.reject() }
            NxButton {
                id: confirm
                kind: "danger"
                text: root.action === "format" ? qsTr("Format")
                    : root.action === "delete" ? qsTr("Delete partition")
                    : root.action === "table" ? qsTr("Create partition table")
                    : qsTr("Overwrite with image")
                enabled: root.device !== "" && phrase.text === root.device && root.inputValid
                onClicked: { root.confirmed(); root.accept() }
            }
        }
    }
}
