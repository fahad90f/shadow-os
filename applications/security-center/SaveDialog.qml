// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui

// "Save report…": format and destination of the exported report.
Dialog {
    id: root

    required property var center
    readonly property string format: formatChoice.currentIndex === 0 ? "text" : "json"
    property string problem: ""
    signal saved(string path)

    function save() {
        problem = center.saveReport(pathField.text, format)
        if (problem !== "") return
        const path = pathField.text
        root.accept()
        root.saved(path)
    }

    title: qsTr("Save the security report")
    modal: true
    width: Math.min(560, parent ? parent.width - Theme.spaceXxl : 560)
    closePolicy: Popup.CloseOnEscape
    footer: null
    onOpened: {
        problem = ""
        pathField.text = center.defaultSavePath(format)
        pathField.forceActiveFocus()
    }

    contentItem: ColumnLayout {
        spacing: Theme.spaceMd

        Text {
            Layout.fillWidth: true
            text: qsTr("The report lists listening services, login activity and system settings. "
                       + "The file is readable only by you.")
            color: Theme.textSecondary
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeBody
            wrapMode: Text.WordWrap
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceMd
            Text {
                text: qsTr("Format")
                color: Theme.textMuted
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
            }
            NxSegmented {
                id: formatChoice
                model: [qsTr("Text"), qsTr("JSON")]
                onActivated: (index) => {
                    currentIndex = index
                    // Keep a name the user typed; only swap the extension.
                    pathField.text = pathField.text.replace(/\.(txt|json)$/, index === 0 ? ".txt" : ".json")
                    root.problem = ""
                }
            }
            Item { Layout.fillWidth: true }
        }
        TextField {
            id: pathField
            Layout.fillWidth: true
            font.family: Theme.fontMono
            placeholderText: qsTr("File name")
            Accessible.name: qsTr("File name")
            onTextEdited: root.problem = ""
            onAccepted: root.save()
        }
        Text {
            Layout.fillWidth: true
            visible: text !== ""
            text: root.problem !== "" ? root.problem
                : root.visible && root.center.fileExists(pathField.text) ? qsTr("This file exists and will be replaced.") : ""
            color: root.problem !== "" ? Theme.danger : Theme.warning
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
            wrapMode: Text.WordWrap
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Theme.spaceXs
            spacing: Theme.spaceSm
            Item { Layout.fillWidth: true }
            NxButton { text: qsTr("Cancel"); onClicked: root.reject() }
            NxButton {
                text: qsTr("Save")
                kind: "primary"
                iconName: "save"
                enabled: pathField.text.trim() !== ""
                onClicked: root.save()
            }
        }
    }
}
