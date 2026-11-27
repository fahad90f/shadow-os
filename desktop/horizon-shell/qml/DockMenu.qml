// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.shadow.ui

// Context menu of a dock entry, shown above it.
//
// How it opens (windows of the overlay's 'reveal' clock, 160 ms in all; see
// OverlaySurface): a short line rises from the icon to the card, the card's
// outline is traced from that point both ways round, and the rows appear
// one after the other, starting with the one nearest the icon.
OverlaySurface {
    id: root

    scope: "shadow-dock-menu"
    revealDuration: Theme.motionRevealMenu
    exitDuration: Theme.motionExitMenu

    property int row: -1
    property real centerX: 0
    property real bottomOffset: 96
    // The distance between the dock and the card (part of bottomOffset).
    readonly property real gap: Theme.spaceSm
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

    // 0..1 for a row whose lower edge is 'distance' above the lower end of
    // the list: the row nearest the icon comes first, at most seven steps.
    function rowAppear(distance) {
        const clock = root.stage        // read first: bindings follow the clock
        if (clock >= 1) return 1
        const start = Math.min(0.25 + 0.075 * Math.min(7, Math.round(distance / 35)), 0.6)
        return root.ph(start, start + 0.4)
    }

    component MenuRow: Rectangle {
        id: menuRow
        property string icon: ""
        property string text: ""
        property bool danger: false
        // 0..1 while the menu opens (DockMenu.rowAppear).
        property real appear: 1
        signal triggered()
        Layout.fillWidth: true
        implicitHeight: 34
        radius: Theme.radiusSm
        opacity: appear
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
            // What is drawn rises 4 px; the row keeps its place.
            transform: Translate { y: 4 * (1 - menuRow.appear) }
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

    FxTether {
        x: root.centerX
        y: root.height - root.bottomOffset + root.gap
        direction: "up"
        length: root.gap
        progress: root.ph(0, 0.25)
        color: Theme.mix(Theme.accent, Theme.borderStrong, root.settle)
    }

    Item {
        id: card
        x: Math.max(Theme.spaceSm, Math.min(root.width - width - Theme.spaceSm, root.centerX - width / 2))
        y: root.height - root.bottomOffset - height
        width: 260
        height: menuColumn.implicitHeight + Theme.spaceXs * 2

        Rectangle {
            anchors.fill: parent
            radius: Theme.radiusMd
            color: Theme.bgOverlay
            border.width: Theme.borderWidth
            border.color: Theme.border
            opacity: root.ph(0.1, 0.6)
        }
        TapHandler { }
        FxFrame {
            anchors.fill: parent
            radius: Theme.radiusMd
            origin: "bottom"
            originPosition: root.centerX - card.x
            progress: root.ph(0.2, 0.9)
            color: Theme.mix(Theme.accent, Theme.border, root.settle)
            glow: 1 - root.settle
        }

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
                opacity: root.rowAppear(menuColumn.height - y - height)
            }

            Repeater {
                model: root.windowList
                MenuRow {
                    required property var modelData
                    icon: modelData.active ? "check" : "window"
                    text: modelData.title !== "" ? modelData.title : qsTr("Untitled window")
                    appear: root.rowAppear(menuColumn.height - y - height)
                    onTriggered: { windows.activate(modelData.uuid); root.hide() }
                }
            }
            NxDivider { Layout.fillWidth: true; Layout.margins: Theme.spaceXs; visible: root.windowList.length > 0 }

            MenuRow {
                visible: (root.entry.desktopId || "") !== ""
                icon: "plus"
                text: root.entry.running ? qsTr("New window") : qsTr("Open")
                appear: root.rowAppear(menuColumn.height - y - height)
                onTriggered: { dock.launchNew(root.row, root); root.hide() }
            }
            MenuRow {
                visible: (root.entry.desktopId || "") !== ""
                icon: "pin"
                text: root.entry.pinned ? qsTr("Unpin from dock") : qsTr("Pin to dock")
                appear: root.rowAppear(menuColumn.height - y - height)
                onTriggered: { dock.setPinned(root.row, !root.entry.pinned); root.hide() }
            }
            MenuRow {
                visible: root.windowList.length > 0
                icon: "close"
                danger: true
                text: root.windowList.length > 1 ? qsTr("Close all windows") : qsTr("Close window")
                appear: root.rowAppear(menuColumn.height - y - height)
                onTriggered: { dock.closeAll(root.row); root.hide() }
            }
        }
    }
}
