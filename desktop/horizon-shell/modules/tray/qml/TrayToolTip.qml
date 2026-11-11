// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.nexus.ui

// The tool tip of a tray item, under its icon. A surface of its own: the
// panel is only as high as its buttons, a tool tip inside it has no room.
//
// A layer surface is placed by its anchors, not by coordinates: this one is
// anchored to the top right corner and made just wide enough for the bubble
// at its left end to sit under the icon.
ShellSurface {
    id: root

    required property var shell

    property var item: null         // TrayItem
    property int anchorX: 0
    readonly property int margin: Theme.spaceXs
    readonly property int screenWidth: screen ? screen.width : 1280

    screen: shell.primaryScreen
    layer: "overlay"
    scope: "nexus-tray-tooltip"
    anchorTop: true
    anchorRight: true
    keyboard: "none"
    width: Math.max(bubble.width + margin, Math.min(screenWidth, screenWidth - anchorX + Math.round(bubble.width / 2)))
    height: bubble.height + margin * 2
    visible: item !== null && bubble.hasText

    Connections {
        target: tray
        function onToolTipRequested(item, x) { root.anchorX = x; root.item = item }
        function onToolTipDismissed() { root.item = null }
        function onItemRemoved(item) { if (item === root.item) root.item = null }
        function onMenuRequested(item, x) { root.item = null }
    }

    Rectangle {
        id: bubble
        readonly property bool hasText: title.text !== "" || description.text !== ""
        x: 0
        y: root.margin
        width: Math.min(360, Math.max(title.implicitWidth, description.implicitWidth) + Theme.spaceMd * 2)
        height: column.implicitHeight + Theme.spaceSm * 2
        radius: Theme.radiusSm
        color: Theme.bgOverlay
        border.width: Theme.borderWidth
        border.color: Theme.borderStrong

        Column {
            id: column
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Theme.spaceSm
            anchors.leftMargin: Theme.spaceMd
            anchors.rightMargin: Theme.spaceMd
            spacing: Theme.spaceXxs
            Text {
                id: title
                width: parent.width
                visible: text !== ""
                text: root.item ? root.item.toolTipTitle : ""
                textFormat: Text.PlainText
                color: Theme.textPrimary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                font.weight: Theme.weightSemibold
                elide: Text.ElideRight
                maximumLineCount: 1
            }
            Text {
                id: description
                width: parent.width
                visible: text !== ""
                text: root.item ? root.item.toolTipText : ""
                textFormat: Text.PlainText
                color: Theme.textSecondary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.Wrap
                elide: Text.ElideRight
                maximumLineCount: 6
            }
        }
    }
}
