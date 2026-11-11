// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Templates as T
import org.shadow.ui

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
        // The role is an attached property of each button in the box.
        highlighted: T.DialogButtonBox.buttonRole === T.DialogButtonBox.AcceptRole
            || T.DialogButtonBox.buttonRole === T.DialogButtonBox.YesRole
            || T.DialogButtonBox.buttonRole === T.DialogButtonBox.ApplyRole
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
