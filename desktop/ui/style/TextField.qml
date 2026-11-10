// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Templates as T
import org.nexus.ui

T.TextField {
    id: control

    implicitWidth: implicitBackgroundWidth + leftInset + rightInset
                   || Math.max(contentWidth, placeholder.implicitWidth) + leftPadding + rightPadding
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding,
                             placeholder.implicitHeight + topPadding + bottomPadding)

    leftPadding: Theme.spaceMd
    rightPadding: Theme.spaceMd
    topPadding: 0
    bottomPadding: 0

    color: enabled ? Theme.textPrimary : Theme.textDisabled
    selectionColor: Theme.accentFillStrong
    selectedTextColor: Theme.textPrimary
    placeholderTextColor: Theme.textMuted
    verticalAlignment: TextInput.AlignVCenter
    font.family: Theme.fontUi
    font.pixelSize: Theme.fontSizeBody

    Text {
        id: placeholder
        x: control.leftPadding
        y: control.topPadding
        width: control.width - (control.leftPadding + control.rightPadding)
        height: control.height - (control.topPadding + control.bottomPadding)
        text: control.placeholderText
        font: control.font
        color: control.placeholderTextColor
        verticalAlignment: control.verticalAlignment
        visible: !control.length && !control.preeditText && (!control.activeFocus || control.horizontalAlignment !== Qt.AlignHCenter)
        elide: Text.ElideRight
        renderType: control.renderType
    }

    background: Rectangle {
        implicitWidth: 200
        implicitHeight: Theme.controlHeight
        radius: Theme.radiusSm
        color: Theme.bgInput
        opacity: control.enabled ? 1 : Theme.opacityDisabled
        border.width: control.activeFocus ? Theme.focusRing : Theme.borderWidth
        border.color: control.activeFocus ? Theme.accent : control.hovered ? Theme.borderStrong : Theme.border
        Behavior on border.color { ColorAnimation { duration: Theme.motionFast } }
    }
}
