// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui

// A command line shown exactly as it has to be typed, selectable, with a
// button that copies it to the clipboard.
Rectangle {
    id: root

    property string command: ""
    signal copied()

    Layout.fillWidth: true
    implicitHeight: row.implicitHeight + Theme.spaceSm * 2
    radius: Theme.radiusSm
    color: Theme.bgInput
    border.width: Theme.borderWidth
    border.color: Theme.borderSubtle

    RowLayout {
        id: row
        anchors.fill: parent
        anchors.leftMargin: Theme.spaceMd
        anchors.rightMargin: Theme.spaceXs
        anchors.topMargin: Theme.spaceSm
        anchors.bottomMargin: Theme.spaceSm
        spacing: Theme.spaceSm

        TextEdit {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            text: root.command
            readOnly: true
            selectByMouse: true
            wrapMode: TextEdit.WrapAnywhere
            color: Theme.textPrimary
            selectionColor: Theme.accentFill
            selectedTextColor: Theme.textOnAccent
            font.family: Theme.fontMono
            font.pixelSize: Theme.fontSizeSmall
            Accessible.name: qsTr("Command")
            Accessible.description: root.command
        }
        NxIconButton {
            Layout.alignment: Qt.AlignTop
            iconName: "copy"
            tooltip: qsTr("Copy the command")
            onClicked: {
                arsenal.copyText(root.command)
                root.copied()
            }
        }
    }
}
