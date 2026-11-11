// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.networkcenter

NxPage {
    id: page
    title: qsTr("Active connections")
    subtitle: qsTr("TCP and UDP sockets of this computer and the processes that own them. Listening sockets bound to more than loopback are highlighted.")
    maxContentWidth: 1400

    property int protocolIndex: 0
    property int groupIndex: 0
    property bool autoRefresh: true

    readonly property var criteria: {
        let c = {}
        if (protocolIndex === 1) c.protocol = "tcp"
        if (protocolIndex === 2) c.protocol = "udp"
        if (groupIndex === 1) c.group = "listening"
        if (groupIndex === 2) c.group = "connected"
        if (groupIndex === 3) c.group = "other"
        return c
    }

    actions: [
        NxButton {
            text: qsTr("Refresh")
            iconName: "refresh"
            busy: sockets.loading
            onClicked: sockets.refresh()
        }
    ]

    SocketList {
        id: sockets
        onElevationFailed: (message) => ApplicationWindow.window.toaster.show(message, "danger")
    }
    RecordModel {
        id: socketModel
        roles: ["order", "protocol", "family", "state", "group", "localAddress", "localInterface", "localPort",
                "peerAddress", "peerPort", "process", "pid", "pids", "processes", "hasProcess", "listening", "exposed"]
        records: sockets.sockets
        filterText: search.text
        filterRoles: ["protocol", "state", "localAddress", "localInterface", "localPort", "peerAddress", "peerPort", "processes"]
        criteria: page.criteria
    }
    Timer {
        interval: 5000
        // The administrator view is a snapshot: refreshing it would ask for
        // authentication again whenever the authorisation has expired.
        running: page.visible && page.autoRefresh && !sockets.administratorView
        repeat: true
        onTriggered: sockets.refresh()
    }
    onVisibleChanged: if (visible && !sockets.administratorView) sockets.refresh()
    Component.onCompleted: sockets.refresh()

    NxBanner {
        visible: sockets.error !== ""
        kind: "danger"
        title: qsTr("The socket list cannot be read")
        message: sockets.error
    }
    NxBanner {
        visible: sockets.administratorView
        kind: "info"
        title: qsTr("Administrator view")
        message: qsTr("Snapshot taken at %1 with administrator rights: processes of all users are shown. It does not refresh on its own.").arg(sockets.updatedAt)
        actionText: qsTr("Back to my view")
        onActionTriggered: sockets.showOwnView()
    }
    NxBanner {
        visible: !sockets.administratorView && !sockets.runningAsRoot && sockets.error === "" && sockets.withoutProcess > 0
        kind: "info"
        message: qsTr("%1 of %2 sockets show no process: the kernel only tells you which of your own processes own a socket. Seeing the processes of other users and of the system needs administrator rights.")
                     .arg(sockets.withoutProcess).arg(sockets.total)
        actionText: qsTr("Show all processes (administrator)")
        onActionTriggered: sockets.showAllProcesses()
    }
    NxBanner {
        visible: !sockets.administratorView && sockets.runningAsRoot && sockets.error === "" && sockets.withoutProcess > 0
        kind: "info"
        message: sockets.withoutProcess === 1
            ? qsTr("One socket has no process even for root: it belongs to the kernel or to a process in another PID namespace.")
            : qsTr("%1 sockets have no process even for root: they belong to the kernel or to a process in another PID namespace.").arg(sockets.withoutProcess)
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spaceMd

        NxSearchField {
            id: search
            Layout.fillWidth: true
            Layout.maximumWidth: 320
            placeholderText: qsTr("Filter by address, port or process…")
        }
        NxSegmented {
            model: [qsTr("All"), "TCP", "UDP"]
            currentIndex: page.protocolIndex
            onActivated: (index) => page.protocolIndex = index
        }
        NxSegmented {
            model: [qsTr("Any state"), qsTr("Listening"), qsTr("Connected"), qsTr("Other")]
            currentIndex: page.groupIndex
            onActivated: (index) => page.groupIndex = index
        }
        Item { Layout.fillWidth: true }
        Switch {
            text: qsTr("Auto-refresh")
            enabled: !sockets.administratorView
            checked: page.autoRefresh
            onToggled: page.autoRefresh = checked
        }
    }

    NxCard {
        Layout.fillWidth: true
        implicitHeight: Math.max(320, page.height - 330)
        padding: Theme.spaceSm

        NxDataTable {
            anchors.fill: parent
            model: socketModel
            emptyText: !sockets.ready ? qsTr("Reading the socket list…")
                : sockets.total === 0 ? qsTr("No TCP or UDP sockets")
                : qsTr("No socket matches the filter")
            columns: [
                { title: qsTr("Protocol"), role: "protocol", width: 76, format: (v) => v.toUpperCase() },
                { title: qsTr("State"), role: "state", width: 96,
                  color: (v, row) => row.listening ? Theme.accent : Theme.textSecondary },
                { title: qsTr("Local address"), role: "localAddress", fill: true, mono: true,
                  format: (v, row) => row.localInterface !== "" ? v + "%" + row.localInterface : v,
                  color: (v, row) => row.exposed ? Theme.warning : Theme.textPrimary },
                { title: qsTr("Port"), role: "localPort", width: 70, align: Qt.AlignRight, mono: true, format: (v) => v < 0 ? "*" : String(v),
                  color: (v, row) => row.exposed ? Theme.warning : Theme.textSecondary },
                { title: qsTr("Peer address"), role: "peerAddress", fill: true, mono: true },
                { title: qsTr("Port"), role: "peerPort", width: 70, align: Qt.AlignRight, mono: true, format: (v) => v < 0 ? "*" : String(v) },
                { title: qsTr("Process"), role: "process", width: 170, format: (v) => v === "" ? "—" : v,
                  color: (v) => v === "" ? Theme.textMuted : Theme.textPrimary },
                { title: qsTr("PID"), role: "pids", width: 90, align: Qt.AlignRight, mono: true, format: (v) => v === "" ? "—" : v },
            ]
            onSortRequested: (role, ascending) => socketModel.sortBy(role, ascending)
        }
    }

    Label {
        Layout.fillWidth: true
        text: sockets.ready && sockets.error === ""
            ? qsTr("%1 of %2 sockets shown · %3 listening (%4 beyond loopback) · %5 established TCP · updated %6")
                  .arg(socketModel.count).arg(sockets.total).arg(sockets.listening).arg(sockets.exposed).arg(sockets.established).arg(sockets.updatedAt)
            : ""
        color: Theme.textMuted
        font.pixelSize: Theme.fontSizeSmall
        wrapMode: Text.WordWrap
    }
}
