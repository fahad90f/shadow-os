// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.monitor

NxPage {
    id: page

    required property SystemMonitor monitor

    readonly property var filesystems: monitor.filesystems
    readonly property var disks: monitor.disks
    readonly property string spanText: {
        const seconds = monitor.historySize * monitor.interval / 1000
        return seconds < 120 ? qsTr("Last %1 seconds").arg(seconds) : qsTr("Last %1 minutes").arg(Math.round(seconds / 60))
    }
    readonly property var kindLabels: ({
        "nvme": qsTr("NVMe solid-state drive"), "ssd": qsTr("Solid-state drive"), "hdd": qsTr("Hard disk"),
        "removable": qsTr("Removable drive"), "mmc": qsTr("SD / eMMC storage"), "virtual": qsTr("Virtual disk"),
        "optical": qsTr("Optical drive"),
    })

    title: qsTr("Disks")
    subtitle: qsTr("Space used on the mounted file systems, and what the drives are reading and writing.")
    maxContentWidth: 1080

    // ---- file systems ------------------------------------------------------------
    NxSection {
        title: qsTr("File systems")
        description: qsTr("Mounted file systems that hold data. Virtual ones such as tmpfs and proc are not listed.")

        Repeater {
            model: page.filesystems.length
            Item {
                id: fsRow
                required property int index
                readonly property var fs: page.filesystems[index]
                    ?? ({ mountPoint: "", device: "", type: "", readOnly: false, total: 0, used: 0, available: 0, usedPercent: 0 })

                Layout.fillWidth: true
                implicitHeight: Math.max(Theme.rowHeightLarge + Theme.spaceSm, fsLayout.implicitHeight + Theme.spaceMd * 2)
                Accessible.role: Accessible.StaticText
                Accessible.name: qsTr("%1: %2 of %3 used").arg(fs.mountPoint).arg(Theme.formatBytes(fs.used)).arg(Theme.formatBytes(fs.total))

                Rectangle {
                    visible: fsRow.index > 0
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: Theme.spaceLg
                    height: 1
                    color: Theme.borderSubtle
                }
                RowLayout {
                    id: fsLayout
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: Theme.spaceLg
                    anchors.rightMargin: Theme.spaceLg
                    spacing: Theme.spaceLg

                    NxIcon { name: "harddrive"; size: Theme.icon; color: Theme.textSecondary }
                    ColumnLayout {
                        Layout.preferredWidth: 250
                        Layout.maximumWidth: 250
                        spacing: 2
                        Text {
                            Layout.fillWidth: true
                            text: fsRow.fs.mountPoint
                            color: Theme.textPrimary
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeBody
                            font.weight: Theme.weightMedium
                            elide: Text.ElideMiddle
                        }
                        Text {
                            Layout.fillWidth: true
                            text: [fsRow.fs.device, fsRow.fs.type, fsRow.fs.readOnly ? qsTr("read-only") : ""]
                                .filter(s => s !== "").join(" · ")
                            color: Theme.textMuted
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeSmall
                            elide: Text.ElideMiddle
                        }
                    }
                    NxMeter {
                        Layout.fillWidth: true
                        // The share of the usable space that is taken, as df(1) reports it.
                        label: qsTr("%1 used").arg(Theme.formatBytes(fsRow.fs.used))
                        valueText: Math.round(fsRow.fs.usedPercent) + " %"
                        value: fsRow.fs.usedPercent / 100
                        // A read-only medium is full by nature; that is not a warning.
                        toned: !fsRow.fs.readOnly
                    }
                    ColumnLayout {
                        Layout.preferredWidth: 150
                        Layout.maximumWidth: 150
                        spacing: 2
                        Text {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignRight
                            text: qsTr("%1 free").arg(Theme.formatBytes(fsRow.fs.available))
                            color: Theme.textPrimary
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeBody
                            font.weight: Theme.weightMedium
                        }
                        Text {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignRight
                            text: qsTr("%1 in total").arg(Theme.formatBytes(fsRow.fs.total))
                            color: Theme.textMuted
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeSmall
                        }
                    }
                }
            }
        }
        ListRow {
            first: true
            visible: page.filesystems.length === 0
            icon: "info"
            title: page.monitor.filesystemsReady ? qsTr("No file system with stored data is mounted")
                                                 : qsTr("Reading the mounted file systems…")
            subtitle: page.monitor.filesystemsReady
                ? qsTr("Only virtual file systems are mounted in this environment.") : ""
        }
    }

    // ---- activity ----------------------------------------------------------------
    HistoryCard {
        id: activityCard
        Layout.fillWidth: true
        title: qsTr("Disk activity")
        capacity: page.monitor.historySize
        minScale: 10240
        chartHeight: 120
        series: [{ label: qsTr("Read"), value: Theme.formatRate(page.monitor.diskReadRate),
                   values: page.monitor.diskReadHistory, color: Theme.accentEmerald },
                 { label: qsTr("Write"), value: Theme.formatRate(page.monitor.diskWriteRate),
                   values: page.monitor.diskWriteHistory, color: Theme.accentAmber }]
        caption: qsTr("%1 · all drives together").arg(page.spanText)
        scaleCaption: qsTr("Scale %1").arg(Theme.formatRate(activityCard.valueScale))
        unavailableText: page.monitor.ready && page.disks.length === 0
            ? qsTr("No drive is visible to this system, so there is no activity to chart.") : ""
    }

    NxSection {
        title: qsTr("Drives")
        description: qsTr("Physical drives. Partitions, loop devices and logical volumes are part of these figures.")

        Repeater {
            model: page.disks.length
            ListRow {
                id: diskRow
                required property int index
                first: index === 0
                readonly property var disk: page.disks[index]
                    ?? ({ name: "", model: "", kind: "", size: 0, readBytes: 0, writeBytes: 0,
                          readBytesPerSecond: 0, writeBytesPerSecond: 0, busyPercent: 0 })
                icon: "harddrive"
                title: disk.name
                subtitle: [disk.model, page.kindLabels[disk.kind] ?? "", Theme.formatBytes(disk.size)]
                    .filter(s => s !== "").join(" · ")

                Figure {
                    label: qsTr("Read")
                    value: page.monitor.ratesReady ? Theme.formatRate(diskRow.disk.readBytesPerSecond) : "…"
                    valueColor: diskRow.disk.readBytesPerSecond > 0 ? Theme.textPrimary : Theme.textMuted
                }
                Figure {
                    label: qsTr("Write")
                    value: page.monitor.ratesReady ? Theme.formatRate(diskRow.disk.writeBytesPerSecond) : "…"
                    valueColor: diskRow.disk.writeBytesPerSecond > 0 ? Theme.textPrimary : Theme.textMuted
                }
                Figure {
                    label: qsTr("Busy")
                    minimumWidth: 56
                    value: !page.monitor.ratesReady ? "…"
                        : diskRow.disk.busyPercent > 0 && diskRow.disk.busyPercent < 0.5 ? "<1 %"
                        : Math.round(diskRow.disk.busyPercent) + " %"
                    valueColor: diskRow.disk.busyPercent >= 80 ? Theme.warning
                        : diskRow.disk.busyPercent > 0 ? Theme.textPrimary : Theme.textMuted
                }
                Figure {
                    label: qsTr("Read since boot")
                    minimumWidth: 104
                    value: Theme.formatBytes(diskRow.disk.readBytes)
                    valueColor: Theme.textSecondary
                }
                Figure {
                    label: qsTr("Written since boot")
                    minimumWidth: 112
                    value: Theme.formatBytes(diskRow.disk.writeBytes)
                    valueColor: Theme.textSecondary
                }
            }
        }
        ListRow {
            first: true
            visible: page.disks.length === 0
            icon: "info"
            title: page.monitor.ready ? qsTr("No physical drive found") : qsTr("Looking for drives…")
            subtitle: page.monitor.ready
                ? qsTr("The kernel lists no block device backed by hardware (/sys/block has only virtual devices).") : ""
        }
    }
}
