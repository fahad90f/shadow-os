// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.nexus.ui
import "Fmt.js" as Fmt

// A table inside a card for a scrolling page: as tall as its rows up to
// maxVisibleRows, then the table scrolls. Sorting and filtering are local.
NxCard {
    id: root

    property var columns: []
    property var rows: []
    property int maxVisibleRows: 12
    property string emptyText: qsTr("Nothing to show")
    property string sortRole: ""
    property bool sortAscending: true
    property string filterText: ""
    property var filterRoles: []
    readonly property var shownRows: Fmt.sortRows(Fmt.filterRows(Fmt.list(rows), filterText, filterRoles), sortRole, sortAscending)

    padding: Theme.spaceSm
    Layout.fillWidth: true
    implicitHeight: 34 + Math.max(2, Math.min(shownRows.length, maxVisibleRows)) * table.rowHeight + padding * 2

    NxDataTable {
        id: table
        anchors.fill: parent
        columns: root.columns
        model: root.shownRows
        sortRole: root.sortRole
        sortAscending: root.sortAscending
        emptyText: root.filterText !== "" && Fmt.list(root.rows).length > 0 ? qsTr("Nothing matches “%1”").arg(root.filterText) : root.emptyText
        onSortRequested: (role, ascending) => { root.sortRole = role; root.sortAscending = ascending }
    }
}
