// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import "Format.js" as Format

// The trades of the paper account: open positions first (with their running
// profit), then closed trades, newest first.
Item {
    id: root

    property Labels labels
    property int maxRows: 0          // 0 = all
    property bool showClose: false   // a "Close" button on open positions
    property bool showReason: false  // why a trade closed, and who led it

    readonly property var rows: {
        var list = []
        var open = trading.positions
        for (var i = 0; i < open.length; i++)
            list.push({ open: true, data: open[i] })
        var closed = trading.trades
        for (var j = 0; j < closed.length; j++)
            list.push({ open: false, data: closed[j] })
        return maxRows > 0 ? list.slice(0, maxRows) : list
    }
    // The compact table (a row limit) shows the time of day only.
    readonly property bool compact: maxRows > 0
    readonly property var widths: showReason ? [0.13, 0.12, 0.07, 0.12, 0.12, 0.12, 0.10, 0.22]
                                             : [0.15, 0.19, 0.08, 0.16, 0.16, 0.12, 0.14]
    readonly property var titles: {
        var t = [qsTr("Time"), qsTr("Asset"), qsTr("Type"), qsTr("Entry"), qsTr("Exit"), qsTr("P&L"), qsTr("Status")]
        if (showReason)
            t.push(qsTr("Closed because"))
        return t
    }

    component Cell: Label {
        property int column: 0
        Layout.preferredWidth: root.widths[column] * (root.width - Theme.spaceSm * 2)
        elide: Text.ElideRight
        font.pixelSize: Theme.fontSizeSmall
        color: Theme.textSecondary
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spaceSm
            Layout.rightMargin: Theme.spaceSm
            spacing: 0
            Repeater {
                model: root.titles
                Cell {
                    required property string modelData
                    required property int index
                    column: index
                    text: modelData
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSizeCaption
                }
            }
        }
        NxDivider { Layout.fillWidth: true; Layout.topMargin: Theme.spaceXs }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: root.rows
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            delegate: Item {
                id: row
                required property var modelData
                required property int index
                readonly property var t: modelData.data
                readonly property bool open: modelData.open
                width: list.width
                height: Theme.rowHeight
                Accessible.name: qsTr("%1 %2 %3, entry %4, profit %5, %6").arg(Format.clock(t.openedAt)).arg(t.name)
                    .arg(root.labels.action(t.side)).arg(Format.price(t.entry)).arg(Format.signedMoney(t.pnl))
                    .arg(open ? qsTr("open") : qsTr("closed"))

                Rectangle {
                    anchors.fill: parent
                    color: rowHover.hovered ? Theme.hoverOverlay : "transparent"
                }
                HoverHandler { id: rowHover }
                ToolTip.visible: rowHover.hovered && (row.t.reasoning || "") !== ""
                ToolTip.delay: 600
                ToolTip.text: (row.t.lead ? row.t.lead + ": " : "") + (row.t.reasoning || "")

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spaceSm
                    anchors.rightMargin: Theme.spaceSm
                    spacing: 0
                    Cell {
                        column: 0
                        text: root.compact ? Format.clock(row.open ? row.t.openedAt : row.t.closedAt)
                                           : Format.stamp(row.open ? row.t.openedAt : row.t.closedAt)
                        font.family: Theme.fontMono
                    }
                    Cell { column: 1; text: row.t.name; color: Theme.textPrimary }
                    Cell {
                        column: 2
                        text: root.labels.action(row.t.side)
                        color: root.labels.actionColor(row.t.side)
                        font.weight: Theme.weightSemibold
                    }
                    Cell { column: 3; text: Format.price(row.t.entry) }
                    Cell { column: 4; text: row.open ? "—" : Format.price(row.t.exit) }
                    Cell {
                        column: 5
                        text: Format.signedMoney(row.t.pnl)
                        color: row.t.pnl > 0 ? Theme.success : row.t.pnl < 0 ? Theme.danger : Theme.textSecondary
                    }
                    Item {
                        Layout.preferredWidth: root.widths[6] * (root.width - Theme.spaceSm * 2)
                        Layout.fillHeight: true
                        NxBadge {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: !(root.showClose && row.open)
                            text: row.open ? qsTr("OPEN") : qsTr("CLOSED")
                            kind: row.open ? "success" : "neutral"
                            dot: false
                        }
                        NxButton {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: root.showClose && row.open
                            text: qsTr("Close")
                            onClicked: trading.closePosition(row.t.id)
                            Accessible.name: qsTr("Close the %1 position").arg(row.t.name)
                        }
                    }
                    Cell {
                        visible: root.showReason
                        column: 7
                        text: row.open ? qsTr("Stop %1 · target %2").arg(Format.price(row.t.stop)).arg(Format.price(row.t.takeProfit))
                                       : root.labels.closeReason(row.t.reason)
                    }
                }
            }
        }
    }

    readonly property string emptyMessage: trading.autoTrade
        ? qsTr("Trades appear here when a signal passes the risk rules.")
        : qsTr("Auto trade is off. Turn it on, or place a signal by hand, and the trades appear here.")

    // Under the column titles, in the space the rows would take. The small
    // table on the Live Trading page has no room for the icon.
    NxEmptyState {
        x: list.x + (list.width - width) / 2
        y: list.y + (list.height - height) / 2
        width: Math.min(list.width - Theme.spaceLg * 2, 420)
        visible: root.rows.length === 0 && !root.compact
        icon: "list"
        title: qsTr("No trades yet")
        message: root.emptyMessage
    }
    ColumnLayout {
        x: list.x + (list.width - width) / 2
        y: list.y + Math.max(0, (list.height - height) / 2)
        width: Math.min(list.width - Theme.spaceLg * 2, 420)
        visible: root.rows.length === 0 && root.compact
        spacing: Theme.spaceXs
        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("No trades yet")
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSizeBodyLarge
            font.weight: Theme.weightSemibold
        }
        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: root.emptyMessage
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSizeSmall
            wrapMode: Text.WordWrap
        }
    }
}
