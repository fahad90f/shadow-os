// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.networkcenter

NxPage {
    id: page
    title: qsTr("Interfaces")
    subtitle: qsTr("Every network interface the kernel knows, with its link, addresses, counters and live rates.")
    maxContentWidth: 1040

    actions: [
        NxIconButton {
            iconName: "refresh"
            tooltip: qsTr("Refresh")
            onClicked: { NetState.refresh(); Nm.refresh() }
        }
    ]

    RecordModel {
        id: interfaces
        roles: ["name", "typeLabel", "category", "status", "operstate", "flags", "mac", "mtu", "speedMbps", "duplex",
                "driver", "bus", "busDevice", "kind", "master", "peer", "addresses", "wireless", "physical"]
        records: NetState.interfaces
    }

    NxBanner {
        visible: NetState.error !== ""
        kind: "danger"
        title: qsTr("The network state cannot be read")
        message: NetState.error
    }
    NxBanner {
        visible: Nm.status !== "running" && Nm.status !== "unknown"
        kind: "info"
        message: qsTr("%1 Interfaces are shown as the kernel reports them; connecting and disconnecting them needs NetworkManager.").arg(Nm.statusText)
    }

    BusyIndicator {
        Layout.alignment: Qt.AlignHCenter
        visible: !NetState.ready && NetState.error === ""
        running: visible
    }

    Repeater {
        model: interfaces
        InterfaceCard { }
    }

    NxEmptyState {
        Layout.fillWidth: true
        visible: NetState.ready && interfaces.count === 0 && NetState.error === ""
        icon: "ethernet"
        title: qsTr("No network interfaces")
        message: qsTr("The kernel reports no network interface at all, not even loopback.")
    }
}
