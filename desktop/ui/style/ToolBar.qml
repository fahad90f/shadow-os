// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Templates as T
import org.nexus.ui

T.ToolBar {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)
    horizontalPadding: Theme.spaceMd
    spacing: Theme.spaceSm

    background: Rectangle {
        implicitHeight: Theme.headerHeight
        color: Theme.bgSurface
        Rectangle {
            width: parent.width
            height: 1
            y: control.position === T.ToolBar.Header ? parent.height - 1 : 0
            color: Theme.borderSubtle
        }
    }
}
