// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Templates as T
import org.nexus.ui

T.DialogButtonBox {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            (control.count === 1 ? implicitContentWidth * 2 : implicitContentWidth) + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)
    contentWidth: (contentItem as ListView)?.contentWidth

    spacing: Theme.spaceSm
    padding: Theme.spaceXl
    topPadding: Theme.spaceSm
    alignment: Qt.AlignRight

    delegate: Button {
        highlighted: control.buttonRole(this) === T.DialogButtonBox.AcceptRole
            || control.buttonRole(this) === T.DialogButtonBox.YesRole
            || control.buttonRole(this) === T.DialogButtonBox.ApplyRole
    }

    contentItem: ListView {
        implicitWidth: contentWidth
        model: control.contentModel
        spacing: control.spacing
        orientation: ListView.Horizontal
        boundsBehavior: Flickable.StopAtBounds
        snapMode: ListView.SnapToItem
    }

    background: Item { implicitHeight: Theme.controlHeight }
}
