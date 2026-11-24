// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui

// "View all →": a quiet link at the end of a panel's title row.
Item {
    id: root

    property string text: ""
    signal clicked()

    implicitWidth: row.implicitWidth + Theme.spaceXs * 2
    implicitHeight: 24
    activeFocusOnTab: true
    Accessible.role: Accessible.Link
    Accessible.name: text
    Keys.onReturnPressed: clicked()
    Keys.onSpacePressed: clicked()

    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusSm
        color: hover.hovered ? Theme.hoverOverlay : "transparent"
        border.width: root.activeFocus ? 2 : 0
        border.color: Theme.focusRing
    }
    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler { onTapped: root.clicked() }

    RowLayout {
        id: row
        anchors.centerIn: parent
        spacing: Theme.spaceXs
        Label {
            text: root.text
            color: Theme.accent
            font.pixelSize: Theme.fontSizeSmall
        }
        NxIcon { name: "arrow-right"; size: Theme.iconSmall; color: Theme.accent }
    }
}
