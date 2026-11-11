// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui

// A vertically scrolling column for step content that can exceed the window.
Flickable {
    id: root

    default property alias content: column.data
    property real spacing: Theme.spaceLg

    Layout.fillWidth: true
    Layout.fillHeight: true
    contentWidth: width
    contentHeight: column.implicitHeight
    boundsBehavior: Flickable.StopAtBounds
    clip: true
    ScrollBar.vertical: ScrollBar { id: bar; policy: root.contentHeight > root.height ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff }

    // Keep the item that has keyboard focus visible.
    function reveal(item) {
        if (!item)
            return
        const p = item.mapToItem(column, 0, 0)
        if (p.y < contentY)
            contentY = Math.max(0, p.y - Theme.spaceMd)
        else if (p.y + item.height > contentY + height)
            contentY = Math.min(contentHeight - height, p.y + item.height - height + Theme.spaceMd)
    }
    Connections {
        target: root.Window.window
        function onActiveFocusItemChanged() {
            let item = root.Window.window.activeFocusItem
            for (let p = item; p; p = p.parent) {
                if (p === column) { root.reveal(item); break }
            }
        }
    }

    ColumnLayout {
        id: column
        width: root.width - (bar.policy === ScrollBar.AlwaysOn ? bar.width + Theme.spaceXs : 0)
        spacing: root.spacing
    }
}
