// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.nexus.ui

// One application in the dock.
Item {
    id: root

    required property int index
    required property string name
    required property string icon
    required property bool running
    required property bool active
    required property bool attention
    required property bool pinned
    required property int windowCount
    property real iconSize: 44

    signal activated()
    signal menuRequested()
    signal middleClicked()

    readonly property bool hovered: hover.hovered

    width: iconSize + Theme.spaceSm * 2
    height: iconSize + Theme.spaceSm * 2 + 6
    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: name + (running ? qsTr(", running") : "")
    Keys.onReturnPressed: activated()
    Keys.onSpacePressed: activated()
    Keys.onMenuPressed: menuRequested()

    Rectangle {
        anchors.fill: appIcon
        anchors.margins: -5
        radius: Theme.radiusMd
        color: root.active ? Theme.accentFill : hover.hovered || root.activeFocus ? Theme.hoverOverlay : "transparent"
        border.width: root.active ? 1 : 0
        border.color: Theme.alpha(Theme.accent, 0.45)
        Behavior on color { ColorAnimation { duration: Theme.motionFast } }
    }

    NxAppIcon {
        id: appIcon
        anchors.horizontalCenter: parent.horizontalCenter
        y: Theme.spaceSm
        name: root.icon
        size: root.iconSize
        scale: tap.pressed ? 0.92 : hover.hovered ? 1.1 : 1.0
        transformOrigin: Item.Bottom
        Behavior on scale { NumberAnimation { duration: Theme.motionFast; easing.type: Easing.OutCubic } }

        // Attention: gentle pulse until the window is activated.
        SequentialAnimation on opacity {
            running: root.attention && !root.active
            loops: Animation.Infinite
            alwaysRunToEnd: true
            NumberAnimation { to: 0.45; duration: 520 }
            NumberAnimation { to: 1.0; duration: 520 }
        }
    }

    // Running indicator: one dot per window (up to three); a bar when active.
    Row {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 2
        spacing: 3
        visible: root.running
        Repeater {
            model: root.active ? 1 : Math.min(3, root.windowCount)
            Rectangle {
                width: root.active ? 14 : 4
                height: 4
                radius: 2
                color: root.active ? Theme.accent : Theme.textSecondary
            }
        }
    }

    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler { id: tap; acceptedButtons: Qt.LeftButton; onTapped: root.activated() }
    TapHandler { acceptedButtons: Qt.RightButton; onTapped: root.menuRequested() }
    TapHandler { acceptedButtons: Qt.MiddleButton; onTapped: root.middleClicked() }
}
