// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui

// Input for the actions that do not destroy data:
//   "unlock"  passphrase of an encrypted volume
//   "label"   new file-system label
//   "resize"  new partition size
//   "create"  new partition in free space
//   "image"   file to save a disk image to
Dialog {
    id: root

    property string mode: ""
    property string device: ""
    property var formatTypes: []
    property string currentLabel: ""
    property double currentSize: 0
    property double minSize: 0
    property double maxSize: 0
    property string defaultPath: ""
    property var imageInfo: ({})
    property string note: ""

    readonly property double mib: 1048576
    readonly property string passphrase: passField.text
    readonly property string label: mode === "label" ? labelField.text : createLabel.text
    readonly property double sizeBytes: mode === "resize" ? resizeBox.value * mib
                                      : (createBox.value >= createBox.to ? 0 : createBox.value * mib)
    readonly property string fsType: typeBox.currentIndex <= 0 ? "" : typeBox.currentText
    readonly property string imagePath: pathField.text

    signal confirmed()
    signal imagePathEdited(string path)

    function present() {
        passField.text = ""
        labelField.text = currentLabel
        createLabel.text = ""
        resizeBox.value = Math.round(currentSize / mib)
        createBox.value = createBox.to
        typeBox.currentIndex = Math.max(0, typeBox.find("ext4"))
        pathField.text = defaultPath
        open()
        if (mode === "unlock") passField.forceActiveFocus()
        else if (mode === "label") { labelField.forceActiveFocus(); labelField.selectAll() }
        else if (mode === "image") pathField.forceActiveFocus()
    }

    readonly property bool inputValid: mode === "unlock" ? passField.text !== ""
        : mode === "resize" ? Math.round(currentSize / mib) !== resizeBox.value
        : mode === "image" ? (pathField.text.trim() !== "" && imageInfo.exists !== true && imageInfo.isDirectory !== true && imageInfo.directoryWritable === true)
        : true

    title: mode === "unlock" ? qsTr("Unlock %1").arg(device)
         : mode === "label" ? qsTr("Change the label of %1").arg(device)
         : mode === "resize" ? qsTr("Resize %1").arg(device)
         : mode === "create" ? qsTr("Create a partition on %1").arg(device)
         : qsTr("Create an image of %1").arg(device)
    anchors.centerIn: parent
    width: Math.min(520, parent ? parent.width - Theme.spaceXxl : 520)
    modal: true
    closePolicy: Popup.CloseOnEscape
    footer: null

    contentItem: ColumnLayout {
        spacing: Theme.spaceMd

        Label {
            Layout.fillWidth: true
            visible: root.note !== ""
            wrapMode: Text.WordWrap
            color: Theme.textSecondary
            text: root.note
        }

        // unlock
        NxPasswordField {
            id: passField
            Layout.fillWidth: true
            visible: root.mode === "unlock"
            placeholderText: qsTr("Passphrase")
            Accessible.name: qsTr("Passphrase of the encrypted volume")
            onAccepted: if (confirm.enabled) confirm.clicked()
        }

        // label
        TextField {
            id: labelField
            Layout.fillWidth: true
            visible: root.mode === "label"
            maximumLength: 32
            placeholderText: qsTr("New label (empty removes it)")
            Accessible.name: qsTr("New label")
            onAccepted: if (confirm.enabled) confirm.clicked()
        }

        // resize
        GridLayout {
            Layout.fillWidth: true
            visible: root.mode === "resize"
            columns: 2
            columnSpacing: Theme.spaceMd
            rowSpacing: Theme.spaceSm
            Label { text: qsTr("Current size"); color: Theme.textSecondary }
            Label { text: Theme.formatBytes(root.currentSize); color: Theme.textPrimary }
            Label { text: qsTr("New size (MiB)"); color: Theme.textSecondary }
            SpinBox {
                id: resizeBox
                Layout.preferredWidth: 180
                editable: true
                from: Math.max(1, Math.ceil(root.minSize / root.mib))
                to: Math.max(from, Math.floor(root.maxSize / root.mib))
                stepSize: 16
                Accessible.name: qsTr("New size in mebibytes")
            }
            Label { text: qsTr("Possible"); color: Theme.textSecondary }
            Label {
                text: qsTr("%1 – %2 MiB").arg(resizeBox.from).arg(resizeBox.to)
                color: Theme.textMuted
            }
        }

        // create
        GridLayout {
            Layout.fillWidth: true
            visible: root.mode === "create"
            columns: 2
            columnSpacing: Theme.spaceMd
            rowSpacing: Theme.spaceSm
            Label { text: qsTr("Size (MiB)"); color: Theme.textSecondary }
            RowLayout {
                spacing: Theme.spaceSm
                SpinBox {
                    id: createBox
                    Layout.preferredWidth: 180
                    editable: true
                    from: 1
                    to: Math.max(1, Math.floor(root.maxSize / root.mib))
                    stepSize: 16
                    Accessible.name: qsTr("Partition size in mebibytes")
                }
                Label {
                    text: createBox.value >= createBox.to ? qsTr("all of the free space") : qsTr("of %1 MiB free").arg(createBox.to)
                    color: Theme.textMuted
                }
            }
            Label { text: qsTr("File system"); color: Theme.textSecondary }
            ComboBox {
                id: typeBox
                Layout.fillWidth: true
                model: [qsTr("None (empty partition)")].concat(root.formatTypes)
                Accessible.name: qsTr("File system type")
            }
            Label { text: qsTr("Label"); color: Theme.textSecondary; visible: typeBox.currentIndex > 0 }
            TextField {
                id: createLabel
                Layout.fillWidth: true
                visible: typeBox.currentIndex > 0
                maximumLength: 32
                placeholderText: qsTr("Optional name of the volume")
                Accessible.name: qsTr("Volume label")
            }
        }

        // image
        ColumnLayout {
            Layout.fillWidth: true
            visible: root.mode === "image"
            spacing: Theme.spaceXs
            TextField {
                id: pathField
                Layout.fillWidth: true
                font.family: Theme.fontMono
                Accessible.name: qsTr("Image file to create")
                onTextChanged: root.imagePathEdited(text)
                onAccepted: if (confirm.enabled) confirm.clicked()
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontSizeSmall
                color: root.inputValid ? Theme.textMuted : Theme.danger
                text: pathField.text.trim() === "" ? qsTr("Type the file to create.")
                    : root.imageInfo.isDirectory === true ? qsTr("This is a folder; add a file name.")
                    : root.imageInfo.exists === true ? qsTr("This file already exists; choose another name.")
                    : root.imageInfo.directoryWritable !== true ? qsTr("The folder does not exist or is not writable.")
                    : qsTr("The image needs up to %1 of space (empty areas are stored as holes).").arg(Theme.formatBytes(root.currentSize))
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
                kind: "primary"
                text: root.mode === "unlock" ? qsTr("Unlock")
                    : root.mode === "label" ? qsTr("Change label")
                    : root.mode === "resize" ? qsTr("Resize")
                    : root.mode === "create" ? qsTr("Create")
                    : qsTr("Create image")
                enabled: root.inputValid
                onClicked: { root.confirmed(); root.accept() }
            }
        }
    }
}
