// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.impl
import QtQuick.Templates as T
import org.shadow.ui

T.TabButton {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    horizontalPadding: Theme.spaceLg
    verticalPadding: 0
    spacing: Theme.spaceSm

    font.family: Theme.fontUi
    font.pixelSize: Theme.fontSizeBody
    font.weight: control.checked ? Theme.weightSemibold : Theme.weightMedium

    icon.width: Theme.iconSmall
    icon.height: Theme.iconSmall
    icon.color: control.checked ? Theme.accent : Theme.textSecondary

    contentItem: IconLabel {
        spacing: control.spacing
        mirrored: control.mirrored
        display: control.display
        icon: control.icon
        text: control.text
        font: control.font
        color: !control.enabled ? Theme.textDisabled : control.checked ? Theme.textPrimary
            : control.hovered ? Theme.textPrimary : Theme.textSecondary
    }

    background: Item {
        implicitHeight: 38
        Rectangle {
            anchors.fill: parent
            anchors.bottomMargin: 4
            radius: Theme.radiusSm
            color: control.hovered && !control.checked ? Theme.hoverOverlay : "transparent"
        }
        Rectangle {
            anchors.bottom: parent.bottom
            anchors.horizontalCenter: parent.horizontalCenter
            width: control.checked ? parent.width - Theme.spaceLg : 0
            height: 2
            radius: 1
            color: Theme.accent
            Behavior on width { NumberAnimation { duration: Theme.motionNormal; easing.type: Easing.OutCubic } }
        }
    }
}
