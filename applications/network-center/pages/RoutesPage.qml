// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.networkcenter

NxPage {
    id: page
    title: qsTr("Routing table")
    subtitle: qsTr("IPv4 and IPv6 routes of all kernel routing tables, and the neighbours (ARP / NDP) next hops resolve to.")
    maxContentWidth: 1400

    property int familyIndex: 0
    property bool showLocal: false

    actions: [
        NxIconButton {
            iconName: "refresh"
            tooltip: qsTr("Refresh")
            onClicked: NetState.refresh()
        }
    ]

    RecordModel {
        id: routes
        roles: ["order", "family", "destination", "gateway", "device", "metric", "protocol", "scope", "table", "type", "source", "flags"]
        records: page.showLocal ? NetState.routes : NetState.routes.filter(r => r.table !== "local")
        filterText: search.text
        filterRoles: ["family", "destination", "gateway", "device", "protocol", "scope", "table", "type", "source", "flags"]
        criteria: page.familyIndex === 1 ? ({ family: "IPv4" }) : page.familyIndex === 2 ? ({ family: "IPv6" }) : ({})
    }

    RecordModel {
        id: neighbours
        roles: ["address", "family", "device", "mac", "state", "router"]
        records: NetState.neighbours
        filterText: search.text
        criteria: page.familyIndex === 1 ? ({ family: "IPv4" }) : page.familyIndex === 2 ? ({ family: "IPv6" }) : ({})
    }

    NxBanner {
        visible: NetState.error !== ""
        kind: "danger"
        title: qsTr("The routing table cannot be read")
        message: NetState.error
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spaceMd

        NxSearchField {
            id: search
            Layout.fillWidth: true
            Layout.maximumWidth: 360
            placeholderText: qsTr("Filter by destination, gateway, device, protocol…")
        }
        NxSegmented {
            model: [qsTr("All"), "IPv4", "IPv6"]
            currentIndex: page.familyIndex
            onActivated: (index) => page.familyIndex = index
        }
        Switch {
            text: qsTr("Kernel “local” table")
            checked: page.showLocal
            onToggled: page.showLocal = checked
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Also show the kernel-maintained routes for this computer's own and broadcast addresses")
        }
        Item { Layout.fillWidth: true }
        Label {
            text: qsTr("%1 of %2 routes").arg(routes.count).arg(NetState.routes.length)
            color: Theme.textMuted
            font.pixelSize: Theme.fontSizeSmall
        }
    }

    NxCard {
        Layout.fillWidth: true
        implicitHeight: Math.max(300, page.height - 430)
        padding: Theme.spaceSm

        NxDataTable {
            id: table
            anchors.fill: parent
            model: routes
            emptyText: !NetState.ready ? qsTr("Reading the routing tables…")
                : NetState.routes.length === 0 ? qsTr("The kernel has no routes")
                : qsTr("No route matches the filter")
            columns: [
                { title: qsTr("Family"), role: "family", width: 62 },
                { title: qsTr("Destination"), role: "destination", fill: true, mono: true,
                  color: (v) => v === "default" ? Theme.accent : Theme.textPrimary },
                { title: qsTr("Gateway"), role: "gateway", fill: true, mono: true, format: (v) => v === "" ? qsTr("on-link") : v,
                  color: (v) => v === "" ? Theme.textMuted : Theme.textSecondary },
                { title: qsTr("Device"), role: "device", width: 96, mono: true },
                { title: qsTr("Metric"), role: "metric", width: 70, align: Qt.AlignRight, mono: true },
                { title: qsTr("Protocol"), role: "protocol", width: 86, format: (v) => v === "" ? "—" : v },
                { title: qsTr("Scope"), role: "scope", width: 70 },
                { title: qsTr("Table"), role: "table", width: 70 },
                { title: qsTr("Type"), role: "type", width: 86 },
            ]
            onSortRequested: (role, ascending) => routes.sortBy(role, ascending)
        }
    }

    NxSection {
        title: qsTr("Neighbours")
        description: qsTr("Link-layer addresses the kernel has resolved for hosts on directly connected networks (ip neigh).")

        Item {
            Layout.fillWidth: true
            implicitHeight: Math.min(260, 40 + Math.max(1, neighbours.count) * 34) + Theme.spaceSm * 2
            NxDataTable {
                anchors.fill: parent
                anchors.margins: Theme.spaceSm
                model: neighbours
                emptyText: NetState.neighbours.length === 0 ? qsTr("The neighbour table is empty") : qsTr("No neighbour matches the filter")
                columns: [
                    { title: qsTr("Address"), role: "address", fill: true, mono: true },
                    { title: qsTr("Hardware address"), role: "mac", width: 170, mono: true, format: (v) => v === "" ? "—" : v },
                    { title: qsTr("Device"), role: "device", width: 110, mono: true },
                    { title: qsTr("State"), role: "state", width: 130,
                      color: (v) => v.indexOf("FAILED") >= 0 || v.indexOf("INCOMPLETE") >= 0 ? Theme.warning : Theme.textSecondary },
                ]
                onSortRequested: (role, ascending) => neighbours.sortBy(role, ascending)
            }
        }
    }
}
