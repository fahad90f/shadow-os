// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Templates as T
import org.shadow.ui

T.CheckBox {
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
        implicitWidth: 18
        implicitHeight: 18
        x: control.text ? (control.mirrored ? control.width - width - control.rightPadding : control.leftPadding)
                        : control.leftPadding + (control.availableWidth - width) / 2
        y: control.topPadding + (control.availableHeight - height) / 2
        radius: Theme.radiusXs
        opacity: control.enabled ? 1 : Theme.opacityDisabled
        color: control.checkState !== Qt.Unchecked ? Theme.accent : Theme.bgInput
        border.width: control.visualFocus ? Theme.focusRing : Theme.borderWidth
        border.color: control.visualFocus || control.checkState !== Qt.Unchecked ? Theme.accent
            : control.hovered ? Theme.borderStrong : Theme.border
        Behavior on color { ColorAnimation { duration: Theme.motionFast } }

        NxIcon {
            anchors.centerIn: parent
            name: control.checkState === Qt.PartiallyChecked ? "minus" : "check"
            size: 14
            color: Theme.textOnAccent
            visible: control.checkState !== Qt.Unchecked
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
