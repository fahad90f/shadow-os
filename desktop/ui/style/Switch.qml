// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Templates as T
import org.nexus.ui

T.Switch {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding,
                             implicitIndicatorHeight + topPadding + bottomPadding)

    padding: Theme.spaceXs
    spacing: Theme.spaceSm
    font.family: Theme.fontUi
    font.pixelSize: Theme.fontSizeBody

    indicator: Rectangle {
        implicitWidth: 40
        implicitHeight: 22
        x: control.text ? (control.mirrored ? control.width - width - control.rightPadding : control.leftPadding)
                        : control.leftPadding + (control.availableWidth - width) / 2
        y: control.topPadding + (control.availableHeight - height) / 2
        radius: height / 2
        opacity: control.enabled ? 1 : Theme.opacityDisabled
        color: control.checked ? Theme.accent : Theme.bgPressed
        border.width: control.visualFocus ? Theme.focusRing : Theme.borderWidth
        border.color: control.visualFocus ? Theme.accent : control.checked ? Theme.accent : Theme.border
        Behavior on color { ColorAnimation { duration: Theme.motionNormal } }

        Rectangle {
            width: 16
            height: 16
            radius: 8
            y: 3
            x: control.checked ? parent.width - width - 3 : 3
            color: control.checked ? Theme.textOnAccent : Theme.textSecondary
            Behavior on x { NumberAnimation { duration: Theme.motionNormal; easing.type: Easing.OutCubic } }
        }
    }

    contentItem: Text {
        leftPadding: control.indicator && !control.mirrored ? control.indicator.width + control.spacing : 0
        rightPadding: control.indicator && control.mirrored ? control.indicator.width + control.spacing : 0
        text: control.text
        font: control.font
        color: control.enabled ? Theme.textPrimary : Theme.textDisabled
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
}
