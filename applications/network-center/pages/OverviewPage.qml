// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.networkcenter

NxPage {
    id: page
    title: qsTr("Overview")
    subtitle: qsTr("Connectivity, addressing and exposure of this computer at a glance.")
    maxContentWidth: 1040

    readonly property var connectivity: NetUtil.connectivitySummary(Nm.running, Nm.state, Nm.connectivity, NetState.linkState)
    readonly property string primary: NetState.primaryInterface
    readonly property var primaryInfo: NetState.interfaces.find(i => i.name === primary) || ({})
    readonly property var live: NetState.liveStats[primary] || ({})
    readonly property var route4: NetState.defaultRoute4
    readonly property var route6: NetState.defaultRoute6
    readonly property var dnsServers: NetState.dns.servers || []
    readonly property string dnsSourceShort: NetState.dns.source === "systemd-resolved" ? qsTr("systemd-resolved") : qsTr("resolv.conf")
    readonly property string dnsSource: NetState.dns.source === "systemd-resolved" ? qsTr("systemd-resolved")
        : qsTr("/etc/resolv.conf, written by %1").arg((NetState.dns.resolvConf || {}).managedBy || "")

    function routeText(route) {
        if (route.destination === undefined)
            return ""
        return route.gateway !== "" ? qsTr("%1 via %2").arg(route.gateway).arg(route.device)
                                    : qsTr("directly on %1").arg(route.device)
    }
    function routeDetail(route) {
        if (route.destination === undefined)
            return ""
        let parts = [qsTr("metric %1").arg(route.metric)]
        if (route.protocol !== "") parts.push(qsTr("protocol %1").arg(route.protocol))
        if (route.source !== "") parts.push(qsTr("source %1").arg(route.source))
        if (route.table !== "main") parts.push(qsTr("table %1").arg(route.table))
        return parts.join(" · ")
    }

    actions: [
        NxIconButton {
            iconName: "refresh"
            tooltip: qsTr("Refresh")
            onClicked: { NetState.refresh(); Nm.refresh(); sockets.refresh() }
        }
    ]

    SocketList { id: sockets }
    Timer {
        interval: 10000
        running: page.visible
        repeat: true
        triggeredOnStart: true
        onTriggered: sockets.refresh()
    }

    NxBanner {
        visible: NetState.error !== ""
        kind: "danger"
        title: qsTr("The network state cannot be read")
        message: NetState.error
    }
    NxBanner {
        visible: page.connectivity.derived === true && Nm.status !== "unknown" && NetState.ready
        kind: "info"
        message: qsTr("%1 The connectivity figure below is derived from the kernel's link and routing state only; nothing has checked that the Internet is actually reachable. Diagnostics can.").arg(Nm.statusText)
        actionText: qsTr("Diagnostics")
        onActionTriggered: ApplicationWindow.window.openPage("diagnostics")
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spaceLg

        NxStatTile {
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            label: qsTr("Connectivity")
            icon: "globe"
            loading: !NetState.ready || Nm.status === "unknown"
            value: page.connectivity.label
            detail: page.connectivity.detail
            tone: page.connectivity.tone
        }
        NxStatTile {
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            label: qsTr("Default gateway")
            icon: "network"
            loading: !NetState.ready
            value: page.route4.gateway || page.route6.gateway
                || (page.route4.device || page.route6.device ? qsTr("On-link") : qsTr("None"))
            detail: page.route4.device ? qsTr("IPv4 via %1").arg(page.route4.device) + (page.route6.device ? qsTr(" · IPv6 too") : "")
                : page.route6.device ? qsTr("IPv6 via %1").arg(page.route6.device)
                : qsTr("No default route")
            tone: page.route4.device || page.route6.device ? "accent" : "danger"
        }
        NxStatTile {
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            label: qsTr("Primary address")
            icon: page.primaryInfo.category === "wifi" ? "wifi" : "ethernet"
            loading: !NetState.ready
            value: NetState.primaryAddress4 || NetState.primaryAddress6 || qsTr("None")
            detail: NetState.primaryAddress4 !== "" && NetState.primaryAddress6 !== "" ? NetState.primaryAddress6
                : page.primary !== "" ? qsTr("on %1").arg(page.primary) : qsTr("No interface with an address")
            tone: NetState.primaryAddress4 !== "" || NetState.primaryAddress6 !== "" ? "accent" : "danger"
        }
        NxStatTile {
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            label: qsTr("DNS")
            icon: "server"
            loading: !NetState.ready
            value: page.dnsServers.length > 0 ? page.dnsServers[0] : qsTr("None")
            detail: page.dnsServers.length > 1 ? qsTr("+%1 more · %2").arg(page.dnsServers.length - 1).arg(page.dnsSourceShort)
                : page.dnsServers.length === 1 ? page.dnsSourceShort : qsTr("No DNS server configured")
            tone: page.dnsServers.length > 0 ? "accent" : "danger"
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spaceLg

        NxStatTile {
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            label: qsTr("Exposed listeners")
            icon: "radar"
            loading: !sockets.ready
            value: sockets.error !== "" ? "—" : String(sockets.exposed)
            detail: sockets.error !== "" ? qsTr("Socket list unavailable")
                : sockets.exposed > 0 ? qsTr("of %1 listening sockets").arg(sockets.listening)
                : qsTr("Nothing beyond loopback")
            tone: sockets.error !== "" ? "neutral" : sockets.exposed > 0 ? "warning" : "success"
        }
        NxStatTile {
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            label: qsTr("Open connections")
            icon: "plug"
            loading: !sockets.ready
            value: sockets.error !== "" ? "—" : String(sockets.established)
            detail: sockets.error !== "" ? qsTr("Socket list unavailable")
                : qsTr("established TCP · %1 sockets").arg(sockets.total)
            tone: "neutral"
        }
        NxStatTile {
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            label: qsTr("Interfaces")
            icon: "ethernet"
            loading: !NetState.ready
            value: qsTr("%1 of %2 up").arg(NetState.interfacesUp).arg(NetState.interfacesTotal)
            detail: page.primary !== "" ? qsTr("default route on %1").arg(page.primary)
                : qsTr("No interface is connected")
            tone: NetState.interfacesUp > 0 ? "neutral" : "danger"
        }
        NxStatTile {
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            label: qsTr("VPN")
            icon: Nm.activeVpnCount > 0 ? "shield-check" : "shield"
            loading: Nm.status === "unknown"
            value: !Nm.running ? "—" : Nm.activeVpnCount > 0 ? qsTr("Active")
                : Nm.vpnConnections.length > 0 ? qsTr("Off") : qsTr("None")
            detail: !Nm.running ? qsTr("Needs NetworkManager")
                : Nm.activeVpnCount > 0 ? qsTr("%1 tunnel(s) up").arg(Nm.activeVpnCount)
                : Nm.vpnConnections.length > 0 ? qsTr("%1 configured, none active").arg(Nm.vpnConnections.length)
                : qsTr("No VPN configured")
            tone: Nm.running && Nm.activeVpnCount > 0 ? "success" : "neutral"
        }
    }

    NxCard {
        Layout.fillWidth: true
        implicitHeight: 196

        ColumnLayout {
            anchors.fill: parent
            spacing: Theme.spaceSm

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceMd
                Label {
                    Layout.fillWidth: true
                    text: page.primary !== "" ? qsTr("Throughput on %1").arg(page.primary) : qsTr("Throughput")
                    font.weight: Theme.weightSemibold
                    font.pixelSize: Theme.fontSizeBodyLarge
                    elide: Text.ElideRight
                }
                Label {
                    visible: page.primary !== ""
                    text: qsTr("last %1 s, one sample per second").arg(NetState.historyCapacity)
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSizeSmall
                }
            }

            Label {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: page.primary === ""
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                wrapMode: Text.WordWrap
                color: Theme.textMuted
                text: qsTr("No interface carries a default route or a routable address, so there is no primary interface to chart. The Interfaces page shows the rates of every interface.")
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: page.primary !== ""
                spacing: Theme.spaceXl

                Repeater {
                    model: [
                        { label: qsTr("Download"), icon: "arrow-down", rate: "rxRate", history: "rxHistory", total: "rxBytes", violet: false },
                        { label: qsTr("Upload"), icon: "arrow-up", rate: "txRate", history: "txHistory", total: "txBytes", violet: true },
                    ]
                    ColumnLayout {
                        id: lane
                        required property var modelData
                        readonly property color tint: modelData.violet ? Theme.accentViolet : Theme.accent
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.preferredWidth: 1
                        spacing: Theme.spaceXs

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spaceSm
                            NxIcon { name: lane.modelData.icon; size: Theme.iconSmall; color: lane.tint }
                            Label { text: lane.modelData.label; color: Theme.textSecondary; font.pixelSize: Theme.fontSizeSmall }
                            Label {
                                Layout.fillWidth: true
                                text: Theme.formatRate(page.live[lane.modelData.rate] || 0)
                                font.family: Theme.fontMono
                                font.weight: Theme.weightSemibold
                            }
                            Label {
                                text: qsTr("%1 since boot").arg(Theme.formatBytes(page.live[lane.modelData.total] || 0))
                                color: Theme.textMuted
                                font.pixelSize: Theme.fontSizeSmall
                            }
                        }
                        NxSparkline {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            values: page.live[lane.modelData.history] || []
                            capacity: NetState.historyCapacity
                            minScale: 1024
                            color: lane.tint
                            Accessible.name: lane.modelData.label
                        }
                    }
                }
            }
        }
    }

    NxSection {
        title: qsTr("Default route")
        NxRow {
            icon: "arrow-right"
            title: qsTr("IPv4")
            subtitle: page.routeDetail(page.route4)
            value: page.routeText(page.route4) || qsTr("No IPv4 default route")
            monoValue: page.route4.destination !== undefined
        }
        NxRow {
            icon: "arrow-right"
            title: qsTr("IPv6")
            subtitle: page.routeDetail(page.route6)
            value: page.routeText(page.route6) || qsTr("No IPv6 default route")
            monoValue: page.route6.destination !== undefined
        }
        NxRow {
            icon: "network"
            title: qsTr("Routing table")
            subtitle: qsTr("%1 routes in all tables").arg(NetState.routes.length)
            chevron: true
            onClicked: ApplicationWindow.window.openPage("routes")
        }
    }

    NxSection {
        title: qsTr("Addresses")
        Repeater {
            model: NetState.interfaces.filter(i => i.category !== "loopback" && i.addresses.length > 0)
            NxRow {
                required property var modelData
                icon: modelData.category === "wifi" ? "wifi" : modelData.category === "tunnel" ? "shield" : "ethernet"
                title: modelData.name
                subtitle: modelData.typeLabel
                value: modelData.addresses.map(a => a.cidr).join("   ")
                monoValue: true
            }
        }
        NxRow {
            visible: NetState.ready && NetState.interfaces.filter(i => i.category !== "loopback" && i.addresses.length > 0).length === 0
            icon: "info"
            title: qsTr("No interface has an IP address")
        }
        NxRow {
            icon: "ethernet"
            title: qsTr("All interfaces")
            subtitle: qsTr("State, counters and live rates of every interface")
            chevron: true
            onClicked: ApplicationWindow.window.openPage("interfaces")
        }
    }

    NxSection {
        title: qsTr("Name resolution")
        NxRow {
            icon: "server"
            title: qsTr("DNS servers")
            subtitle: qsTr("Source: %1").arg(page.dnsSource)
            value: page.dnsServers.length > 0 ? page.dnsServers.join("   ") : qsTr("None configured")
            monoValue: page.dnsServers.length > 0
        }
        NxRow {
            visible: (NetState.dns.search || []).length > 0
            icon: "search"
            title: qsTr("Search domains")
            value: (NetState.dns.search || []).join("   ")
            monoValue: true
        }
        NxRow {
            icon: "globe"
            title: qsTr("DNS configuration")
            subtitle: qsTr("Resolvers per link, DNS over TLS, change servers")
            chevron: true
            onClicked: ApplicationWindow.window.openPage("dns")
        }
    }
}
