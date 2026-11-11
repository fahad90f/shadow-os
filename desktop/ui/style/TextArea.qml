// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Templates as T
import org.shadow.ui

T.TextArea {
    id: control

    implicitWidth: Math.max(contentWidth + leftPadding + rightPadding,
                            implicitBackgroundWidth + leftInset + rightInset,
                            placeholder.implicitWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(contentHeight + topPadding + bottomPadding,
                             implicitBackgroundHeight + topInset + bottomInset,
                             placeholder.implicitHeight + topPadding + bottomPadding)

    padding: Theme.spaceMd

    color: enabled ? Theme.textPrimary : Theme.textDisabled
    selectionColor: Theme.accentFillStrong
    selectedTextColor: Theme.textPrimary
    placeholderTextColor: Theme.textMuted
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
        visible: !control.length && !control.preeditText
        elide: Text.ElideRight
        renderType: control.renderType
    }

    background: Rectangle {
        implicitWidth: 200
        implicitHeight: 80
        radius: Theme.radiusSm
        color: Theme.bgInput
        border.width: control.activeFocus ? Theme.focusRing : Theme.borderWidth
        border.color: control.activeFocus ? Theme.accent : Theme.border
    }
}
