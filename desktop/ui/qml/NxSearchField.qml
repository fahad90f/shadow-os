// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls

TextField {
    id: root

    placeholderText: qsTr("Search")
    leftPadding: Theme.spaceMd + Theme.iconSmall + Theme.spaceSm
    rightPadding: clear.visible ? clear.width + Theme.spaceXs : Theme.spaceMd
    selectByMouse: true
    Accessible.role: Accessible.EditableText
    Accessible.name: placeholderText

    NxIcon {
        x: Theme.spaceMd
        anchors.verticalCenter: parent.verticalCenter
        name: "search"
        size: Theme.iconSmall
        color: root.activeFocus ? Theme.accent : Theme.textMuted
    }

    NxIconButton {
        id: clear
        anchors.right: parent.right
        anchors.rightMargin: 2
        anchors.verticalCenter: parent.verticalCenter
        width: root.height - 4
        height: root.height - 4
        visible: root.text.length > 0
        iconName: "close"
        iconSize: 14
        tooltip: qsTr("Clear")
        onClicked: { root.clear(); root.forceActiveFocus() }
    }

    Keys.onEscapePressed: (event) => {
        if (text.length > 0) clear.clicked()
        else event.accepted = false
    }
}
