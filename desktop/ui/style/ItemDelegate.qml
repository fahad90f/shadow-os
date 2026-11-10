// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.impl
import QtQuick.Templates as T
import org.nexus.ui

T.ItemDelegate {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding,
                             implicitIndicatorHeight + topPadding + bottomPadding)

    leftPadding: Theme.spaceMd
    rightPadding: Theme.spaceMd
    verticalPadding: 0
    spacing: Theme.spaceSm

    font.family: Theme.fontUi
    font.pixelSize: Theme.fontSizeBody

    icon.width: Theme.icon
    icon.height: Theme.icon
    icon.color: control.highlighted ? Theme.accent : Theme.textSecondary

    contentItem: IconLabel {
        spacing: control.spacing
        mirrored: control.mirrored
        display: control.display
        alignment: control.display === IconLabel.IconOnly || control.display === IconLabel.TextUnderIcon ? Qt.AlignCenter : Qt.AlignLeft
        icon: control.icon
        text: control.text
        font: control.font
        color: !control.enabled ? Theme.textDisabled : Theme.textPrimary
    }

    background: Rectangle {
        implicitWidth: 100
        implicitHeight: 34
        radius: Theme.radiusSm
        color: control.down ? Theme.pressedOverlay
            : control.highlighted ? Theme.accentFill
            : control.hovered ? Theme.hoverOverlay : "transparent"
    }
}
