// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.impl
import QtQuick.Templates as T
import org.nexus.ui

T.ToolButton {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    padding: Theme.spaceSm
    spacing: Theme.spaceSm

    font.family: Theme.fontUi
    font.pixelSize: Theme.fontSizeBody
    font.weight: Theme.weightMedium

    icon.width: Theme.icon
    icon.height: Theme.icon
    icon.color: !control.enabled ? Theme.textDisabled
        : control.checked ? Theme.accent
        : control.hovered || control.down ? Theme.textPrimary : Theme.textSecondary

    contentItem: IconLabel {
        spacing: control.spacing
        mirrored: control.mirrored
        display: control.display
        icon: control.icon
        text: control.text
        font: control.font
        color: control.icon.color
    }

    background: Rectangle {
        implicitWidth: Theme.controlHeight
        implicitHeight: Theme.controlHeight
        radius: Theme.radiusSm
        color: control.checked ? Theme.accentFill
            : control.down ? Theme.pressedOverlay
            : control.hovered ? Theme.hoverOverlay : "transparent"
        Behavior on color { ColorAnimation { duration: Theme.motionFast } }
        Rectangle {
            anchors.fill: parent
            anchors.margins: -2
            radius: parent.radius + 2
            color: "transparent"
            border.width: Theme.focusRing
            border.color: Theme.accent
            visible: control.visualFocus
        }
    }
}
