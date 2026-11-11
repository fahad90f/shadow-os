// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.nexus.ui
import "Fmt.js" as Fmt

// Sections "listening" and "connections": what this computer offers to the
// network and what it is connected to.
SectionPage {
    id: page

    signal openPage(string key)

    readonly property var ls: ready ? section : ({})
    readonly property var sockets: Fmt.list(ls.sockets)
    readonly property var conn: center.report.connections
    readonly property bool connReady: Fmt.has(conn) && conn.available !== false
    readonly property var fw: center.report.firewall
    readonly property bool fwKnown: Fmt.has(fw) && fw.available !== false
    readonly property int allInterfaces: sockets.filter(s => s.scope === "all-interfaces").length
    property bool exposedOnly: false

    function scopeText(scope) {
        return scope === "loopback" ? qsTr("This computer only")
            : scope === "all-interfaces" ? qsTr("All interfaces")
            : scope === "specific-address" ? qsTr("One address") : scope
    }
    function endpoint(address, port) {
        return (String(address).indexOf(":") >= 0 ? "[" + address + "]" : address) + ":" + port
    }

    sectionKey: "listening"
    title: qsTr("Network exposure")
    sectionTitle: qsTr("The listening sockets")
    subtitle: qsTr("Programs that accept connections, and the connections that are open right now.")

    NxBanner {
        visible: page.ready && page.ls.exposed_count > 0 && page.fwKnown
        kind: page.fw && page.fw.active ? "info" : "danger"
        title: page.fw && page.fw.active
            ? qsTr("%1 socket(s) listen on addresses other computers can reach — the firewall decides which are reachable").arg(page.ls.exposed_count)
            : qsTr("%1 socket(s) listen on addresses other computers can reach, and the firewall is not active").arg(page.ls.exposed_count)
        message: page.fw && page.fw.active
            ? qsTr("Only the ports opened in the firewall accept inbound connections: %1.")
                  .arg(Fmt.list(page.fw.allowed).length > 0 ? Fmt.list(page.fw.allowed).map(r => r.port + "/" + r.protocol).join(", ") : qsTr("none"))
            : qsTr("Every one of them accepts connections from the network.")
        actionText: qsTr("Open Firewall")
        onActionTriggered: page.openPage("firewall")
    }

    GridLayout {
        Layout.fillWidth: true
        visible: page.ready
        columns: width >= 700 ? 4 : 2
        columnSpacing: Theme.spaceMd
        rowSpacing: Theme.spaceMd
        NxStatTile {
            Layout.fillWidth: true
            label: qsTr("Listening sockets")
            value: String(page.ls.count || 0)
            detail: qsTr("TCP and UDP")
            icon: "server"
        }
        NxStatTile {
            Layout.fillWidth: true
            label: qsTr("On all interfaces")
            value: String(page.allInterfaces)
            detail: qsTr("bound to 0.0.0.0, :: or *")
            icon: "globe"
            tone: page.allInterfaces > 0 ? "warning" : "neutral"
        }
        NxStatTile {
            Layout.fillWidth: true
            label: qsTr("Not loopback")
            value: String(page.ls.exposed_count || 0)
            detail: qsTr("any non-loopback address")
            icon: "network"
        }
        NxStatTile {
            Layout.fillWidth: true
            label: qsTr("Connections")
            value: page.connReady ? String(page.conn.count) : qsTr("—")
            detail: page.connReady ? qsTr("connected sockets now") : qsTr("not determined")
            icon: "activity"
        }
    }

    NxSection {
        visible: page.ready
        title: qsTr("Listening ports")
        description: qsTr("From “ss -tulpn”. Sockets on all interfaces are highlighted: they accept connections on every network this computer joins.")
        headerActions: [
            NxSegmented {
                model: [qsTr("All"), qsTr("Reachable from the network")]
                currentIndex: page.exposedOnly ? 1 : 0
                onActivated: (index) => page.exposedOnly = index === 1
            }
        ]
        TableCard {
            Layout.margins: Theme.spaceXs
            color: "transparent"
            border.width: 0
            maxVisibleRows: 14
            rows: (page.exposedOnly ? page.sockets.filter(s => s.scope !== "loopback") : page.sockets)
                .map(s => Object.assign({}, s, { processText: Fmt.has(s.process) ? s.process : qsTr("not identified") }))
            sortRole: "scope"
            emptyText: page.exposedOnly ? qsTr("Nothing listens on a network address") : qsTr("Nothing is listening")
            columns: [
                { title: qsTr("Protocol"), role: "protocol", width: 90, format: (v) => String(v).toUpperCase() },
                { title: qsTr("Bind address"), role: "local_address", fill: true, mono: true,
                  color: (v, row) => row.scope === "all-interfaces" ? Theme.warning : Theme.textSecondary },
                { title: qsTr("Port"), role: "local_port", width: 90, align: Qt.AlignRight, mono: true },
                { title: qsTr("Process"), role: "processText", fill: true,
                  color: (v, row) => Fmt.has(row.process) ? Theme.textPrimary : Theme.textMuted },
                { title: qsTr("PID"), role: "pid", width: 90, align: Qt.AlignRight, mono: true },
                { title: qsTr("Reachable from"), role: "scope", width: 180,
                  format: (v) => page.scopeText(v),
                  color: (v) => v === "all-interfaces" ? Theme.warning : v === "loopback" ? Theme.textMuted : Theme.textSecondary }
            ]
        }
    }
    Note {
        visible: page.ready && page.sockets.some(s => !Fmt.has(s.process))
        text: qsTr("“not identified”: the kernel did not tell the tool which process owns the socket (a kernel socket, or a process in another namespace).")
    }

    NxSection {
        visible: page.ready
        title: qsTr("Active connections")
        description: page.connReady
            ? (page.conn.count > Fmt.list(page.conn.connections).length
                ? qsTr("The first %1 of %2 connections, from “ss -tunp state connected”.").arg(Fmt.list(page.conn.connections).length).arg(page.conn.count)
                : qsTr("From “ss -tunp state connected”."))
            : ""
        NxRow {
            visible: !page.connReady
            icon: "alert-triangle"
            iconColor: Theme.warning
            title: qsTr("The connections could not be determined")
            subtitle: Fmt.has(page.conn) ? (page.conn.reason || "") : qsTr("The section is not part of the report yet.")
        }
        TableCard {
            visible: page.connReady
            Layout.margins: Theme.spaceXs
            color: "transparent"
            border.width: 0
            maxVisibleRows: 14
            sortRole: "process"
            emptyText: qsTr("No connection is open")
            columns: [
                { title: qsTr("Protocol"), role: "protocol", width: 90, format: (v) => String(v).toUpperCase() },
                { title: qsTr("State"), role: "state", width: 110, mono: true },
                { title: qsTr("Local"), role: "local", fill: true, mono: true },
                { title: qsTr("Remote"), role: "peer", fill: true, mono: true },
                { title: qsTr("Process"), role: "processText", fill: true,
                  color: (v, row) => Fmt.has(row.process) ? Theme.textPrimary : Theme.textMuted },
                { title: qsTr("PID"), role: "pid", width: 90, align: Qt.AlignRight, mono: true }
            ]
            rows: page.connReady ? Fmt.list(page.conn.connections).map(c => Object.assign({}, c, {
                      local: page.endpoint(c.local_address, c.local_port),
                      peer: page.endpoint(c.peer_address, c.peer_port),
                      processText: Fmt.has(c.process) ? c.process : qsTr("not identified") })) : []
        }
    }
}
