// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Sortable table for process lists, connections, packages and similar data.
//
//   NxDataTable {
//       columns: [
//           { title: "Name", role: "name", fill: true },
//           { title: "CPU",  role: "cpu",  width: 80, align: Qt.AlignRight,
//             format: (v) => v.toFixed(1) + " %", mono: true },
//       ]
//       model: processModel                 // QAbstractItemModel or JS array of objects
//       sortRole: "cpu"; sortAscending: false
//       onSortRequested: (role, ascending) => processModel.sortBy(role, ascending)
//       onRowActivated: (row, data) => ...
//   }
//
// Sorting is delegated to the model owner through sortRequested(); the table
// only renders the indicator.
Item {
    id: root

    property var columns: []
    property var model: null
    property string sortRole: ""
    property bool sortAscending: true
    property int currentRow: -1
    property int rowHeight: 34
    property string emptyText: qsTr("Nothing to show")
    property alias count: list.count
    property alias view: list

    signal sortRequested(string role, bool ascending)
    signal rowActivated(int row, var data)
    signal rowSelected(int row, var data)
    signal contextMenuRequested(int row, var data)

    implicitWidth: 600
    implicitHeight: 300

    function columnWidth(index) {
        var c = columns[index]
        if (!c.fill) return c.width || 100
        var fixed = 0, fills = 0
        for (var i = 0; i < columns.length; i++) {
            if (columns[i].fill) fills++
            else fixed += (columns[i].width || 100)
        }
        return Math.max(120, (width - fixed - Theme.spaceMd * 2) / Math.max(1, fills))
    }

    Rectangle {
        id: header
        width: parent.width
        height: 32
        color: Theme.bgSurface
        radius: Theme.radiusSm

        Row {
            anchors.fill: parent
            anchors.leftMargin: Theme.spaceMd
            anchors.rightMargin: Theme.spaceMd
            Repeater {
                model: root.columns
                Item {
                    id: headCell
                    required property var modelData
                    required property int index
                    width: root.columnWidth(index)
                    height: header.height
                    readonly property bool sortable: modelData.sortable !== false && (modelData.role || "") !== ""
                    readonly property bool sorted: root.sortRole !== "" && root.sortRole === modelData.role

                    Row {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: headCell.modelData.align === Qt.AlignRight ? undefined : parent.left
                        anchors.right: headCell.modelData.align === Qt.AlignRight ? parent.right : undefined
                        anchors.rightMargin: Theme.spaceSm
                        spacing: Theme.spaceXs
                        Text {
                            text: headCell.modelData.title || ""
                            color: headCell.sorted ? Theme.textPrimary : Theme.textMuted
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeSmall
                            font.weight: Theme.weightSemibold
                        }
                        NxIcon {
                            visible: headCell.sorted
                            anchors.verticalCenter: parent.verticalCenter
                            name: root.sortAscending ? "chevron-up" : "chevron-down"
                            size: 12
                            color: Theme.accent
                        }
                    }
                    HoverHandler { enabled: headCell.sortable; cursorShape: Qt.PointingHandCursor }
                    TapHandler {
                        enabled: headCell.sortable
                        onTapped: {
                            var asc = headCell.sorted ? !root.sortAscending : (headCell.modelData.align !== Qt.AlignRight)
                            root.sortRole = headCell.modelData.role
                            root.sortAscending = asc
                            root.sortRequested(headCell.modelData.role, asc)
                        }
                    }
                }
            }
        }
    }

    ListView {
        id: list
        anchors.top: header.bottom
        anchors.topMargin: 2
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        clip: true
        model: root.model
        boundsBehavior: Flickable.StopAtBounds
        currentIndex: root.currentRow
        highlightMoveDuration: 0
        reuseItems: true
        activeFocusOnTab: true
        keyNavigationEnabled: true
        ScrollBar.vertical: ScrollBar { }

        onCurrentIndexChanged: if (currentIndex !== root.currentRow) root.currentRow = currentIndex
        Keys.onReturnPressed: if (currentItem) root.rowActivated(currentIndex, currentItem.rowData)

        delegate: Rectangle {
            id: rowItem
            required property int index
            required property var model
            // JS-array models expose the row object as model.modelData.
            readonly property var rowData: (model.modelData !== undefined && model.modelData !== null
                                            && typeof model.modelData === "object") ? model.modelData : model
            readonly property bool selected: index === root.currentRow

            width: list.width
            height: root.rowHeight
            radius: Theme.radiusXs
            color: selected ? Theme.accentFill : hover.hovered ? Theme.hoverOverlay : "transparent"

            HoverHandler { id: hover }
            TapHandler {
                acceptedButtons: Qt.LeftButton
                onTapped: { root.currentRow = rowItem.index; list.forceActiveFocus(); root.rowSelected(rowItem.index, rowItem.rowData) }
                onDoubleTapped: root.rowActivated(rowItem.index, rowItem.rowData)
            }
            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: { root.currentRow = rowItem.index; root.contextMenuRequested(rowItem.index, rowItem.rowData) }
            }

            Row {
                anchors.fill: parent
                anchors.leftMargin: Theme.spaceMd
                anchors.rightMargin: Theme.spaceMd
                Repeater {
                    model: root.columns
                    Item {
                        id: cell
                        required property var modelData
                        required property int index
                        readonly property var raw: rowItem.rowData ? rowItem.rowData[modelData.role] : undefined
                        width: root.columnWidth(index)
                        height: rowItem.height
                        Text {
                            anchors.fill: parent
                            anchors.rightMargin: Theme.spaceSm
                            verticalAlignment: Text.AlignVCenter
                            horizontalAlignment: cell.modelData.align === Qt.AlignRight ? Text.AlignRight : Text.AlignLeft
                            text: cell.raw === undefined || cell.raw === null ? ""
                                : cell.modelData.format ? cell.modelData.format(cell.raw, rowItem.rowData) : String(cell.raw)
                            color: cell.modelData.color ? cell.modelData.color(cell.raw, rowItem.rowData)
                                : cell.index === 0 ? Theme.textPrimary : Theme.textSecondary
                            font.family: cell.modelData.mono ? Theme.fontMono : Theme.fontUi
                            font.pixelSize: cell.modelData.mono ? Theme.fontSizeSmall : Theme.fontSizeBody
                            elide: cell.modelData.elide !== undefined ? cell.modelData.elide : Text.ElideRight
                        }
                    }
                }
            }
        }

        Text {
            anchors.centerIn: parent
            visible: list.count === 0
            text: root.emptyText
            color: Theme.textMuted
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeBody
        }
    }
}
