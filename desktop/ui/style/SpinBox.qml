// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Templates as T
import org.shadow.ui

T.SpinBox {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentItem.implicitWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding,
                             up.implicitIndicatorHeight, down.implicitIndicatorHeight)

    leftPadding: Theme.spaceMd
    rightPadding: 2 * 26 + Theme.spaceXs
    font.family: Theme.fontUi
    font.pixelSize: Theme.fontSizeBody

    validator: IntValidator {
        locale: control.locale.name
        bottom: Math.min(control.from, control.to)
        top: Math.max(control.from, control.to)
    }

    contentItem: TextInput {
        z: 2
        text: control.displayText
        font: control.font
        color: control.enabled ? Theme.textPrimary : Theme.textDisabled
        selectionColor: Theme.accentFillStrong
        selectedTextColor: Theme.textPrimary
        horizontalAlignment: Qt.AlignLeft
        verticalAlignment: Qt.AlignVCenter
        readOnly: !control.editable
        validator: control.validator
        inputMethodHints: control.inputMethodHints
        clip: width < implicitWidth
    }

    up.indicator: Rectangle {
        x: control.width - width - 2
        y: 2
        height: control.height - 4
        implicitWidth: 26
        implicitHeight: Theme.controlHeight - 4
        radius: Theme.radiusXs
        color: control.up.pressed ? Theme.pressedOverlay : control.up.hovered ? Theme.hoverOverlay : "transparent"
        NxIcon { anchors.centerIn: parent; name: "plus"; size: 14; color: enabled ? Theme.textSecondary : Theme.textDisabled }
    }

    down.indicator: Rectangle {
        x: control.width - 2 * width - 2
        y: 2
        height: control.height - 4
        implicitWidth: 26
        implicitHeight: Theme.controlHeight - 4
        radius: Theme.radiusXs
        color: control.down.pressed ? Theme.pressedOverlay : control.down.hovered ? Theme.hoverOverlay : "transparent"
        NxIcon { anchors.centerIn: parent; name: "minus"; size: 14; color: enabled ? Theme.textSecondary : Theme.textDisabled }
    }

    background: Rectangle {
        implicitWidth: 120
        implicitHeight: Theme.controlHeight
        radius: Theme.radiusSm
        color: Theme.bgInput
        opacity: control.enabled ? 1 : Theme.opacityDisabled
        border.width: control.activeFocus ? Theme.focusRing : Theme.borderWidth
        border.color: control.activeFocus ? Theme.accent : Theme.border
    }
}
