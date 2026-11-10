// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Templates as T
import org.nexus.ui

T.ToolTip {
    id: control

    x: parent ? (parent.width - implicitWidth) / 2 : 0
    y: -implicitHeight - 6

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)

    margins: Theme.spaceSm
    horizontalPadding: Theme.spaceSm + 2
    verticalPadding: Theme.spaceXs + 2
    delay: 500
    timeout: 8000

    closePolicy: T.Popup.CloseOnEscape | T.Popup.CloseOnPressOutsideParent | T.Popup.CloseOnReleaseOutsideParent

    font.family: Theme.fontUi
    font.pixelSize: Theme.fontSizeSmall

    contentItem: Text {
        text: control.text
        font: control.font
        wrapMode: Text.Wrap
        color: Theme.textPrimary
    }

    background: Rectangle {
        color: Theme.bgOverlay
        radius: Theme.radiusSm
        border.width: Theme.borderWidth
        border.color: Theme.borderStrong
    }
}
