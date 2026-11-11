// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls.impl
import QtQuick.Templates as T
import org.shadow.ui

// Variants: default (secondary), highlighted (primary), flat (ghost).
// NxButton adds "danger" through its 'kind' property, read here by duck typing.
T.Button {
    id: control

    readonly property string nxKind: control.kind !== undefined ? control.kind
        : control.highlighted ? "primary" : control.flat ? "ghost" : "secondary"
    readonly property color nxFill: nxKind === "primary" ? Theme.accent
        : nxKind === "danger" ? Theme.danger
        : nxKind === "ghost" ? "transparent" : Theme.bgRaised
    readonly property color nxText: !control.enabled ? Theme.textDisabled
        : nxKind === "primary" ? Theme.textOnAccent
        : nxKind === "danger" ? (Theme.dark ? "#1A0308" : "#FFFFFF")
        : Theme.textPrimary

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    verticalPadding: 0
    horizontalPadding: Theme.spaceLg
    spacing: Theme.spaceSm

    font.family: Theme.fontUi
    font.pixelSize: Theme.fontSizeBody
    font.weight: Theme.weightMedium

    icon.width: Theme.iconSmall
    icon.height: Theme.iconSmall
    icon.color: nxText

    contentItem: IconLabel {
        spacing: control.spacing
        mirrored: control.mirrored
        display: control.display
        icon: control.icon
        text: control.text
        font: control.font
        color: control.nxText
    }

    background: Rectangle {
        implicitWidth: 72
        implicitHeight: Theme.controlHeight
        radius: Theme.radiusSm
        opacity: control.enabled ? 1 : Theme.opacityDisabled
        color: control.nxFill
        border.width: control.nxKind === "secondary" ? Theme.borderWidth : 0
        border.color: control.hovered ? Theme.borderStrong : Theme.border

        // hover / press wash
        Rectangle {
            anchors.fill: parent
            radius: parent.radius
            color: control.nxKind === "primary" || control.nxKind === "danger" ? "#FFFFFF" : Theme.textPrimary
            opacity: control.down ? 0.16 : control.hovered ? 0.08 : control.checked ? 0.12 : 0
            Behavior on opacity { NumberAnimation { duration: Theme.motionFast } }
        }
        // keyboard focus ring
        Rectangle {
            anchors.fill: parent
            anchors.margins: -3
            radius: parent.radius + 3
            color: "transparent"
            border.width: Theme.focusRing
            border.color: Theme.accent
            visible: control.visualFocus
        }
    }
}
