// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Templates as T
import org.shadow.ui

T.SplitView {
    id: control
    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, implicitContentHeight + topPadding + bottomPadding)

    handle: Rectangle {
        implicitWidth: control.orientation === Qt.Horizontal ? 5 : control.width
        implicitHeight: control.orientation === Qt.Horizontal ? control.height : 5
        color: "transparent"
        Rectangle {
            anchors.centerIn: parent
            width: control.orientation === Qt.Horizontal ? 1 : parent.width
            height: control.orientation === Qt.Horizontal ? parent.height : 1
            color: T.SplitHandle.pressed ? Theme.accent : T.SplitHandle.hovered ? Theme.borderStrong : Theme.borderSubtle
        }
    }
}
