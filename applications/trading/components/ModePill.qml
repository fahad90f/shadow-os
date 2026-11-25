// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui

// The trading mode. There is one mode in this version, paper trading; the
// menu says so and says why the other is not there.
Rectangle {
    id: root

    implicitWidth: row.implicitWidth + Theme.spaceLg * 2
    implicitHeight: 44
    radius: height / 2
    color: hover.hovered ? Theme.bgHover : Theme.alpha(Theme.bgRaised, 0.9)
    border.width: activeFocus ? 2 : Theme.borderWidth
    border.color: activeFocus ? Theme.textPrimary : Theme.alpha(Theme.accent, 0.6)
    activeFocusOnTab: true
    Accessible.role: Accessible.ButtonMenu
    Accessible.name: qsTr("Trading mode: paper trading")
    Keys.onReturnPressed: menu.open()
    Keys.onSpacePressed: menu.open()

    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler { onTapped: menu.open() }

    RowLayout {
        id: row
        anchors.centerIn: parent
        spacing: Theme.spaceSm
        Rectangle { width: 10; height: 10; radius: 5; color: Theme.success }
        Label {
            text: qsTr("Paper Trading")
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSizeBody
            font.weight: Theme.weightMedium
        }
        NxIcon { name: "chevron-down"; size: Theme.iconSmall; color: Theme.textSecondary }
    }

    Menu {
        id: menu
        y: root.height + Theme.spaceXs
        MenuItem {
            text: qsTr("Paper Trading")
            checkable: true
            checked: true
            // The only mode: choosing it again changes nothing.
            onTriggered: checked = true
        }
        MenuItem {
            text: qsTr("Live Trading (not available in this version)")
            enabled: false
        }
        MenuSeparator {}
        Label {
            width: 300
            leftPadding: Theme.spaceMd
            rightPadding: Theme.spaceMd
            bottomPadding: Theme.spaceSm
            text: qsTr("Orders are simulated against live prices. No order leaves this computer and no real money is involved.")
            color: Theme.textMuted
            font.pixelSize: Theme.fontSizeCaption
            wrapMode: Text.WordWrap
        }
    }
}
