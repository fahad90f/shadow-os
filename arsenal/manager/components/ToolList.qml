// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.nexus.ui

// A card of ToolRows.
Rectangle {
    id: root

    property var tools: []
    property bool showCategory: false

    signal opened(string packageName)
    signal installRequested(string packageName)

    Layout.fillWidth: true
    implicitHeight: rows.implicitHeight
    color: Theme.bgRaised
    radius: Theme.radiusMd
    border.width: Theme.borderWidth
    border.color: Theme.borderSubtle
    clip: true

    ColumnLayout {
        id: rows
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 0

        Repeater {
            model: root.tools
            ToolRow {
                required property var modelData
                required property int index
                tool: modelData
                first: index === 0
                showCategory: root.showCategory
                onOpened: (name) => root.opened(name)
                onInstallRequested: (name) => root.installRequested(name)
            }
        }
    }
}
