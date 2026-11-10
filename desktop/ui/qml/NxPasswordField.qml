// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls

TextField {
    id: root

    property bool revealed: false

    echoMode: revealed ? TextInput.Normal : TextInput.Password
    passwordCharacter: "•"
    rightPadding: toggle.width + Theme.spaceXs
    inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText | Qt.ImhSensitiveData
    selectByMouse: true

    NxIconButton {
        id: toggle
        anchors.right: parent.right
        anchors.rightMargin: 2
        anchors.verticalCenter: parent.verticalCenter
        width: root.height - 4
        height: root.height - 4
        focusPolicy: Qt.NoFocus
        iconName: root.revealed ? "eye-off" : "eye"
        iconSize: Theme.iconSmall
        tooltip: root.revealed ? qsTr("Hide") : qsTr("Show")
        onClicked: root.revealed = !root.revealed
    }
}
