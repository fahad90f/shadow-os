// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Templates as T
import org.shadow.assistant

// A switch whose "on" track glows with the brand light.
T.Switch {
    id: s
    implicitWidth: 40
    implicitHeight: 24
    padding: 0
    hoverEnabled: true
    // Tab reaches it; a click leaves the keyboard where it was
    focusPolicy: Qt.TabFocus
    Accessible.role: Accessible.CheckBox
    Accessible.name: text

    indicator: Rectangle {
        width: 40; height: 24; radius: 12
        opacity: s.enabled ? 1 : 0.4
        color: s.hovered ? Pe.surfaceHi : Pe.sunken
        border.width: s.checked ? 0 : 1
        border.color: Pe.lineStrong
        Rectangle {
            anchors.fill: parent
            radius: parent.radius
            visible: s.checked
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0; color: Pe.dark ? "#1FC4DE" : "#0B8FAE" }
                GradientStop { position: 1.0; color: Pe.dark ? "#8C63F2" : "#6D3FE0" }
            }
        }
        Rectangle {
            width: 18; height: 18; radius: 9
            y: 3
            x: s.checked ? parent.width - width - 3 : 3
            color: s.checked ? "#FFFFFF" : Pe.inkMute
            Behavior on x { NumberAnimation { duration: Pe.tQuick; easing.type: Easing.OutCubic } }
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: -3
            radius: height / 2
            color: "transparent"
            border.width: 2
            border.color: Pe.cyan
            visible: s.visualFocus
        }
    }
    contentItem: Item {}
}
