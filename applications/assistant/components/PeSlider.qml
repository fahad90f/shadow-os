// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Templates as T
import org.shadow.assistant

T.Slider {
    id: sl
    property real mark: 1.0             // a tick (the default value)
    implicitWidth: 240
    implicitHeight: 28
    padding: 0
    hoverEnabled: true
    focusPolicy: Qt.StrongFocus
    Accessible.role: Accessible.Slider
    background: Item {
        x: sl.leftPadding
        width: sl.availableWidth
        height: sl.height
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width; height: 4; radius: 2
            color: Pe.line
        }
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: sl.visualPosition * parent.width; height: 4; radius: 2
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0; color: Pe.cyan }
                GradientStop { position: 1.0; color: Pe.violet }
            }
        }
        Rectangle {
            readonly property real p: (sl.mark - sl.from) / (sl.to - sl.from)
            visible: p > 0 && p < 1
            x: p * parent.width - 1
            anchors.verticalCenter: parent.verticalCenter
            width: 2; height: 12; radius: 1
            color: Pe.inkFaint
        }
    }
    handle: Rectangle {
        x: sl.leftPadding + sl.visualPosition * (sl.availableWidth - width)
        y: (sl.height - height) / 2
        width: 20; height: 20; radius: 10
        color: Pe.dark ? "#FFFFFF" : Pe.ink
        border.width: sl.visualFocus ? 3 : 0
        border.color: Pe.cyan
        scale: sl.pressed ? 1.12 : sl.hovered ? 1.05 : 1
        Behavior on scale { NumberAnimation { duration: Pe.tQuick } }
    }
}
