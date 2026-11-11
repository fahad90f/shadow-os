// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.impl
import QtQuick.Templates as T
import org.shadow.ui

T.MenuItem {
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

    icon.width: Theme.iconSmall
    icon.height: Theme.iconSmall
    icon.color: !control.enabled ? Theme.textDisabled : control.highlighted ? Theme.accent : Theme.textSecondary

    contentItem: IconLabel {
        readonly property real arrowPadding: control.subMenu && control.arrow ? control.arrow.width + control.spacing : 0
        readonly property real indicatorPadding: control.checkable && control.indicator ? control.indicator.width + control.spacing : 0
        leftPadding: !control.mirrored ? indicatorPadding : arrowPadding
        rightPadding: control.mirrored ? indicatorPadding : arrowPadding

        spacing: control.spacing
        mirrored: control.mirrored
        display: control.display
        alignment: Qt.AlignLeft

        icon: control.icon
        text: control.text
        font: control.font
        color: !control.enabled ? Theme.textDisabled : Theme.textPrimary
    }

    indicator: NxIcon {
        x: control.mirrored ? control.width - width - control.rightPadding : control.leftPadding
        y: control.topPadding + (control.availableHeight - height) / 2
        visible: control.checked
        name: "check"
        size: Theme.iconSmall
        color: Theme.accent
    }

    arrow: NxIcon {
        x: control.mirrored ? control.leftPadding : control.width - width - control.rightPadding
        y: control.topPadding + (control.availableHeight - height) / 2
        visible: control.subMenu
        name: "chevron-right"
        size: Theme.iconSmall
        color: Theme.textMuted
    }

    background: Rectangle {
        implicitWidth: 200
        implicitHeight: 32
        radius: Theme.radiusSm
        color: control.down ? Theme.pressedOverlay : control.highlighted ? Theme.hoverOverlay : "transparent"
    }
}
