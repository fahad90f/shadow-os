// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.monitor

NxPage {
    id: page

    required property SystemMonitor monitor

    readonly property var interfaces: monitor.interfaces
    readonly property color secondaryColor: Theme.accentName === "violet" ? Theme.accentCyan : Theme.accentViolet
    readonly property string spanText: {
        const seconds = monitor.historySize * monitor.interval / 1000
        return seconds < 120 ? qsTr("Last %1 seconds").arg(seconds) : qsTr("Last %1 minutes").arg(Math.round(seconds / 60))
    }
    readonly property var kindLabels: ({
        "ethernet": qsTr("Ethernet"), "wifi": qsTr("Wi-Fi"), "loopback": qsTr("Loopback"),
        "virtual": qsTr("Virtual interface"), "other": qsTr("Network device"),
    })
    readonly property var kindIcons: ({
        "ethernet": "ethernet", "wifi": "wifi", "loopback": "refresh", "virtual": "layers", "other": "network",
    })

    title: qsTr("Network")
    subtitle: qsTr("Download and upload rates of every network interface.")
    maxContentWidth: 1080

    HistoryCard {
        id: totalCard
        Layout.fillWidth: true
        title: qsTr("Throughput")
        capacity: page.monitor.historySize
        minScale: 10240
        chartHeight: 130
        series: [{ label: qsTr("Download"), value: Theme.formatRate(page.monitor.networkRxRate),
                   values: page.monitor.networkRxHistory, color: Theme.accent },
                 { label: qsTr("Upload"), value: Theme.formatRate(page.monitor.networkTxRate),
                   values: page.monitor.networkTxHistory, color: page.secondaryColor }]
        caption: qsTr("%1 · hardware interfaces together").arg(page.spanText)
        scaleCaption: qsTr("Scale %1").arg(Theme.formatRate(totalCard.valueScale))
    }

    NxSection {
        title: qsTr("Interfaces")

        Repeater {
            model: page.interfaces.length
            ListRow {
                id: row
                required property int index
                readonly property var iface: page.interfaces[index]
                    ?? ({ name: "", kind: "other", state: "", up: false, mac: "", mtu: 0, speedMbps: null, addresses: [],
                          rxBytes: 0, txBytes: 0, rxBytesPerSecond: 0, txBytesPerSecond: 0, rxHistory: [], txHistory: [] })

                first: index === 0
                icon: page.kindIcons[iface.kind] ?? "network"
                iconColor: iface.up ? Theme.accent : Theme.textMuted
                title: iface.name
                subtitle: {
                    const parts = [page.kindLabels[iface.kind] ?? iface.kind]
                    if (iface.speedMbps !== null && iface.speedMbps !== undefined)
                        parts.push(iface.speedMbps >= 1000 ? qsTr("%1 Gbit/s").arg(iface.speedMbps / 1000)
                                                           : qsTr("%1 Mbit/s").arg(iface.speedMbps))
                    if (iface.addresses.length > 0)
                        parts.push(iface.addresses.join(", "))
                    else if (iface.kind !== "loopback")
                        parts.push(qsTr("no address"))
                    if (iface.mac !== "")
                        parts.push(iface.mac)
                    return parts.join(" · ")
                }

                NxBadge {
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.iface.up ? qsTr("Up") : row.iface.state === "down" ? qsTr("Down") : row.iface.state
                    kind: row.iface.up ? "success" : "neutral"
                }
                SeriesChart {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: page.width >= 940      // the figures matter more in a narrow window
                    width: 120
                    height: 34
                    gridLines: 0
                    minScale: 10240
                    capacity: page.monitor.historySize
                    series: [{ values: row.iface.rxHistory, color: Theme.accent },
                             { values: row.iface.txHistory, color: page.secondaryColor }]
                }
                Figure {
                    anchors.verticalCenter: parent.verticalCenter
                    label: qsTr("Download")
                    value: page.monitor.ratesReady ? Theme.formatRate(row.iface.rxBytesPerSecond) : "…"
                    valueColor: row.iface.rxBytesPerSecond > 0 ? Theme.textPrimary : Theme.textMuted
                }
                Figure {
                    anchors.verticalCenter: parent.verticalCenter
                    label: qsTr("Upload")
                    value: page.monitor.ratesReady ? Theme.formatRate(row.iface.txBytesPerSecond) : "…"
                    valueColor: row.iface.txBytesPerSecond > 0 ? Theme.textPrimary : Theme.textMuted
                }
                Figure {
                    anchors.verticalCenter: parent.verticalCenter
                    label: qsTr("Received")
                    value: Theme.formatBytes(row.iface.rxBytes)
                    valueColor: Theme.textSecondary
                }
                Figure {
                    anchors.verticalCenter: parent.verticalCenter
                    label: qsTr("Sent")
                    value: Theme.formatBytes(row.iface.txBytes)
                    valueColor: Theme.textSecondary
                }
            }
        }
        ListRow {
            first: true
            visible: page.interfaces.length === 0
            icon: "info"
            title: page.monitor.ready ? qsTr("No network interface found") : qsTr("Looking for network interfaces…")
            subtitle: page.monitor.ready ? qsTr("The kernel lists no interface in /proc/net/dev.") : ""
        }
    }
}
