// SPDX-License-Identifier: GPL-3.0-or-later
// Button for the login screen: icon, text or both. Keyboard: Tab to reach,
// Return/Enter/Space to activate.
import QtQuick 2.15
import "../tokens.js" as T

Item {
    id: button

    property real u: 1
    property string icon: ""          // file name in ../icons without ".svg"
    property string trailingIcon: ""
    property string text: ""
    property string description: ""   // for assistive technology when there is no text
    property bool primary: false
    property bool danger: false
    property bool flat: true
    property real iconSize: 20 * u
    readonly property bool hovered: mouse.containsMouse
    signal clicked()

    implicitHeight: Math.round(T.controlHeight * u)
    implicitWidth: Math.max(implicitHeight, Math.round(row.implicitWidth + (text !== "" ? 2 * T.spaceMd * u : 0)))
    width: implicitWidth
    height: implicitHeight
    activeFocusOnTab: true
    opacity: enabled ? 1 : 0.42

    Accessible.role: Accessible.Button
    Accessible.name: text !== "" ? text : description
    Accessible.onPressAction: button.clicked()

    Keys.onReturnPressed: button.clicked()
    Keys.onEnterPressed: button.clicked()
    Keys.onSpacePressed: button.clicked()

    Rectangle {
        anchors.fill: parent
        radius: Math.round(T.radiusMd * button.u)
        color: button.primary ? (mouse.pressed ? Qt.darker(T.accent, 1.25) : button.hovered ? Qt.lighter(T.accent, 1.12) : T.accent)
             : button.danger ? (mouse.pressed ? Qt.darker(T.danger, 1.25) : button.hovered ? Qt.lighter(T.danger, 1.1) : T.danger)
             : mouse.pressed ? T.pressedFill
             : button.hovered ? T.hoverFill
             : button.flat ? "transparent" : T.glassField
        border.width: button.activeFocus ? Math.max(2, Math.round(T.focusRing * button.u)) : (!button.flat && !button.primary && !button.danger ? 1 : 0)
        border.color: button.activeFocus ? ((button.primary || button.danger) ? T.textPrimary : T.accent) : T.border
    }

    Row {
        id: row
        anchors.centerIn: parent
        spacing: Math.round(T.spaceSm * button.u)

        Image {
            visible: button.icon !== ""
            anchors.verticalCenter: parent.verticalCenter
            source: button.icon !== "" ? "../icons/" + button.icon + ".svg" : ""
            sourceSize.width: Math.round(button.iconSize)
            sourceSize.height: Math.round(button.iconSize)
        }
        Text {
            visible: button.text !== ""
            anchors.verticalCenter: parent.verticalCenter
            text: button.text
            color: (button.primary || button.danger) ? T.textOnAccent : T.textPrimary
            font.family: T.fontUi
            font.pixelSize: Math.round(T.fontBody * button.u)
            font.weight: (button.primary || button.danger) ? Font.DemiBold : Font.Medium
        }
        Image {
            visible: button.trailingIcon !== ""
            anchors.verticalCenter: parent.verticalCenter
            source: button.trailingIcon !== "" ? "../icons/" + button.trailingIcon + ".svg" : ""
            sourceSize.width: Math.round(16 * button.u)
            sourceSize.height: Math.round(16 * button.u)
            opacity: 0.7
        }
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: {
            button.forceActiveFocus()
            button.clicked()
        }
    }
}
