// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.shadow.ui

// Context menu of a dock entry, shown above it.
OverlaySurface {
    id: root

    scope: "shadow-dock-menu"

    property int row: -1
    property real centerX: 0
    property real bottomOffset: 96
    property var entry: ({})
    property var windowList: []

    // Fills the menu for a dock entry; the shell then presents it.
    function prepare(index, x) {
        row = index
        centerX = x
        const i = dock.index(index, 0)
        entry = {
            name: dock.data(i, 259), desktopId: dock.data(i, 258), pinned: dock.data(i, 261),
            running: dock.data(i, 262)
        }
        windowList = dock.windowsOf(index)
    }

    component MenuRow: Rectangle {
        id: menuRow
        property string icon: ""
        property string text: ""
        property bool danger: false
        signal triggered()
        Layout.fillWidth: true
        implicitHeight: 34
        radius: Theme.radiusSm
        color: rowTap.pressed ? Theme.pressedOverlay : rowHover.hovered || activeFocus ? Theme.hoverOverlay : "transparent"
        activeFocusOnTab: true
        Accessible.role: Accessible.MenuItem
        Accessible.name: text
        Keys.onReturnPressed: triggered()
        Keys.onSpacePressed: triggered()
        Keys.onDownPressed: nextItemInFocusChain(true).forceActiveFocus()
        Keys.onUpPressed: nextItemInFocusChain(false).forceActiveFocus()
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.spaceMd
            anchors.rightMargin: Theme.spaceMd
            spacing: Theme.spaceSm
            NxIcon { name: menuRow.icon; size: Theme.iconSmall; color: menuRow.danger ? Theme.danger : Theme.textSecondary; visible: menuRow.icon !== "" }
            Text {
                Layout.fillWidth: true
                text: menuRow.text
                color: menuRow.danger ? Theme.danger : Theme.textPrimary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
                elide: Text.ElideRight
            }
        }
        HoverHandler { id: rowHover; cursorShape: Qt.PointingHandCursor }
        TapHandler { id: rowTap; onTapped: menuRow.triggered() }
    }

    Rectangle {
        x: Math.max(Theme.spaceSm, Math.min(root.width - width - Theme.spaceSm, root.centerX - width / 2))
        y: root.height - root.bottomOffset - height
        width: 260
        height: menuColumn.implicitHeight + Theme.spaceXs * 2
        radius: Theme.radiusMd
        color: Theme.bgOverlay
        border.width: Theme.borderWidth
        border.color: Theme.border
        TapHandler { }

        ColumnLayout {
            id: menuColumn
            anchors.fill: parent
            anchors.margins: Theme.spaceXs
            spacing: 1

            Text {
                Layout.fillWidth: true
                Layout.margins: Theme.spaceSm
                text: root.entry.name || ""
                color: Theme.textMuted
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeCaption
                font.weight: Theme.weightSemibold
                font.capitalization: Font.AllUppercase
                font.letterSpacing: 0.8
                elide: Text.ElideRight
            }

            Repeater {
                model: root.windowList
                MenuRow {
                    required property var modelData
                    icon: modelData.active ? "check" : "window"
                    text: modelData.title !== "" ? modelData.title : qsTr("Untitled window")
                    onTriggered: { windows.activate(modelData.uuid); root.hide() }
                }
            }
            NxDivider { Layout.fillWidth: true; Layout.margins: Theme.spaceXs; visible: root.windowList.length > 0 }

            MenuRow {
                visible: (root.entry.desktopId || "") !== ""
                icon: "plus"
                text: root.entry.running ? qsTr("New window") : qsTr("Open")
                onTriggered: { dock.launchNew(root.row, root); root.hide() }
            }
            MenuRow {
                visible: (root.entry.desktopId || "") !== ""
                icon: "pin"
                text: root.entry.pinned ? qsTr("Unpin from dock") : qsTr("Pin to dock")
                onTriggered: { dock.setPinned(root.row, !root.entry.pinned); root.hide() }
            }
            MenuRow {
                visible: root.windowList.length > 0
                icon: "close"
                danger: true
                text: root.windowList.length > 1 ? qsTr("Close all windows") : qsTr("Close window")
                onTriggered: { dock.closeAll(root.row); root.hide() }
            }
        }
    }
}
