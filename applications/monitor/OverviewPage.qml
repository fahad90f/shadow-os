// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.monitor

NxPage {
    id: page

    required property SystemMonitor monitor

    // A second series colour that differs from the accent the user chose.
    readonly property color secondaryColor: Theme.accentName === "violet" ? Theme.accentCyan : Theme.accentViolet
    readonly property color tertiaryColor: Theme.accentName === "blue" ? Theme.accentCyan : Theme.accentBlue
    readonly property string spanText: {
        const seconds = monitor.historySize * monitor.interval / 1000
        return seconds < 120 ? qsTr("Last %1 seconds").arg(seconds) : qsTr("Last %1 minutes").arg(Math.round(seconds / 60))
    }
    readonly property var gpus: monitor.gpus

    function percent(fraction) { return Math.round(fraction * 100) + " %" }
    function frequency(mhz) { return mhz >= 1000 ? (mhz / 1000).toFixed(2) + " GHz" : Math.round(mhz) + " MHz" }
    function usageColor(fraction, normal) {
        return fraction >= 0.92 ? Theme.danger : fraction >= 0.80 ? Theme.warning : normal
    }

    title: qsTr("Overview")
    subtitle: monitor.cpuModel !== ""
        ? qsTr("%1 · %2 of memory").arg(monitor.cpuModel).arg(Theme.formatBytes(monitor.memoryTotal))
        : ""
    maxContentWidth: 1080

    // ---- gauges ------------------------------------------------------------------
    NxCard {
        Layout.fillWidth: true
        implicitHeight: gauges.implicitHeight + padding * 2

        RowLayout {
            id: gauges
            anchors.fill: parent
            spacing: Theme.spaceLg

            GaugeTile {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.alignment: Qt.AlignTop
                label: qsTr("CPU")
                value: page.monitor.cpuUsage
                text: page.monitor.ratesReady ? Math.round(page.monitor.cpuUsage * 100) + "%" : "…"
                caption: qsTr("Processor")
                detail: [page.monitor.cpuThreads === 1 ? qsTr("1 logical CPU")
                         : page.monitor.cpuThreads > 1 ? qsTr("%1 logical CPUs").arg(page.monitor.cpuThreads) : "",
                         page.monitor.cpuFrequencyAvailable ? page.frequency(page.monitor.cpuFrequencyMhz) : ""]
                        .filter(s => s !== "").join(" · ")
            }
            GaugeTile {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.alignment: Qt.AlignTop
                label: qsTr("Memory")
                color: page.usageColor(page.monitor.memoryFraction, page.secondaryColor)
                value: page.monitor.memoryFraction
                text: page.monitor.ready ? Math.round(page.monitor.memoryFraction * 100) + "%" : "…"
                caption: qsTr("Memory")
                detail: page.monitor.ready
                    ? qsTr("%1 of %2").arg(Theme.formatBytes(page.monitor.memoryUsed)).arg(Theme.formatBytes(page.monitor.memoryTotal))
                    : ""
            }
            GaugeTile {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.alignment: Qt.AlignTop
                label: qsTr("GPU")
                color: page.tertiaryColor
                available: page.monitor.gpuUsageAvailable
                value: page.monitor.gpuUsage
                text: Math.round(page.monitor.gpuUsage * 100) + "%"
                caption: qsTr("Graphics")
                detail: !page.monitor.ready ? ""
                    : page.monitor.gpuUsageAvailable ? page.monitor.gpuName
                    : page.monitor.gpuStatus
            }
            GaugeTile {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.alignment: Qt.AlignTop
                label: qsTr("Disk")
                color: page.usageColor(page.monitor.rootFraction, Theme.accentEmerald)
                available: page.monitor.rootAvailable || !page.monitor.filesystemsReady
                value: page.monitor.rootFraction
                text: page.monitor.rootAvailable ? Math.round(page.monitor.rootFraction * 100) + "%" : "…"
                caption: qsTr("Root file system")
                detail: page.monitor.rootAvailable
                    ? qsTr("%1 used · %2 free").arg(Theme.formatBytes(page.monitor.rootUsed)).arg(Theme.formatBytes(page.monitor.rootFree))
                    : page.monitor.filesystemsReady ? qsTr("Usage of / could not be read") : ""
            }
        }
    }

    // ---- key figures -------------------------------------------------------------
    GridLayout {
        Layout.fillWidth: true
        columns: page.width >= 900 ? 4 : 2
        columnSpacing: Theme.spaceLg
        rowSpacing: Theme.spaceLg

        NxStatTile {
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            label: qsTr("Load average")
            icon: "activity"
            loading: !page.monitor.ready
            value: page.monitor.loadAverage[0].toFixed(2)
            detail: qsTr("%1 · %2 over 5 and 15 min").arg(page.monitor.loadAverage[1].toFixed(2)).arg(page.monitor.loadAverage[2].toFixed(2))
            tone: page.monitor.cpuThreads > 0 && page.monitor.loadAverage[0] > page.monitor.cpuThreads ? "warning" : "neutral"
        }
        NxStatTile {
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            label: qsTr("Processor temperature")
            icon: "thermometer"
            loading: !page.monitor.ready
            value: page.monitor.cpuTemperatureAvailable ? Math.round(page.monitor.cpuTemperature) + " °C" : "—"
            detail: page.monitor.cpuTemperatureAvailable ? page.monitor.cpuTemperatureSource : qsTr("No sensor available")
        }
        NxStatTile {
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            label: qsTr("Processes")
            icon: "list"
            loading: !page.monitor.ready
            value: String(page.monitor.processCount)
            detail: qsTr("%1 threads · %2 runnable").arg(page.monitor.tasksTotal).arg(page.monitor.tasksRunnable)
        }
        NxStatTile {
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            label: qsTr("Uptime")
            icon: "clock"
            loading: !page.monitor.ready
            value: Theme.formatDuration(page.monitor.uptimeSeconds)
            detail: page.monitor.ready
                ? qsTr("Since %1").arg(new Date(Date.now() - page.monitor.uptimeSeconds * 1000).toLocaleString(Qt.locale(), Locale.ShortFormat))
                : ""
        }
    }

    // ---- histories ---------------------------------------------------------------
    GridLayout {
        Layout.fillWidth: true
        columns: 2
        columnSpacing: Theme.spaceLg
        rowSpacing: Theme.spaceLg

        HistoryCard {
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            title: qsTr("Processor")
            capacity: page.monitor.historySize
            maxValue: 1
            series: [{ label: "", value: page.monitor.ratesReady ? page.percent(page.monitor.cpuUsage) : "…",
                       values: page.monitor.cpuHistory, color: Theme.accent }]
            caption: page.spanText
            scaleCaption: qsTr("0 – 100 %")
        }
        HistoryCard {
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            title: qsTr("Memory")
            capacity: page.monitor.historySize
            maxValue: 1
            series: page.monitor.swapTotal > 0
                ? [{ label: qsTr("RAM"), value: page.percent(page.monitor.memoryFraction),
                     values: page.monitor.memoryHistory, color: page.secondaryColor },
                   { label: qsTr("Swap"), value: page.percent(page.monitor.swapFraction),
                     values: page.monitor.swapHistory, color: Theme.accentAmber }]
                : [{ label: "", value: page.monitor.ready ? page.percent(page.monitor.memoryFraction) : "…",
                     values: page.monitor.memoryHistory, color: page.secondaryColor }]
            caption: page.monitor.ready
                ? qsTr("%1 available · %2 cached · %3").arg(Theme.formatBytes(page.monitor.memoryAvailable))
                      .arg(Theme.formatBytes(page.monitor.memoryCached))
                      .arg(page.monitor.swapTotal > 0
                           ? qsTr("swap %1 of %2").arg(Theme.formatBytes(page.monitor.swapUsed)).arg(Theme.formatBytes(page.monitor.swapTotal))
                           : qsTr("no swap"))
                : ""
            scaleCaption: qsTr("0 – 100 %")
        }
        HistoryCard {
            id: networkCard
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            title: qsTr("Network")
            capacity: page.monitor.historySize
            minScale: 10240
            series: [{ label: qsTr("Down"), value: Theme.formatRate(page.monitor.networkRxRate),
                       values: page.monitor.networkRxHistory, color: Theme.accent },
                     { label: qsTr("Up"), value: Theme.formatRate(page.monitor.networkTxRate),
                       values: page.monitor.networkTxHistory, color: page.secondaryColor }]
            caption: page.spanText
            scaleCaption: qsTr("Scale %1").arg(Theme.formatRate(networkCard.valueScale))
        }
        HistoryCard {
            id: diskCard
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            title: qsTr("Disk activity")
            capacity: page.monitor.historySize
            minScale: 10240
            series: [{ label: qsTr("Read"), value: Theme.formatRate(page.monitor.diskReadRate),
                       values: page.monitor.diskReadHistory, color: Theme.accentEmerald },
                     { label: qsTr("Write"), value: Theme.formatRate(page.monitor.diskWriteRate),
                       values: page.monitor.diskWriteHistory, color: Theme.accentAmber }]
            caption: page.spanText
            scaleCaption: qsTr("Scale %1").arg(Theme.formatRate(diskCard.valueScale))
        }
    }

    // ---- per-CPU utilisation -----------------------------------------------------
    NxSection {
        title: qsTr("Logical CPUs")
        description: page.monitor.cpuCores > 0
            ? qsTr("%1 cores, %2 threads").arg(page.monitor.cpuCores).arg(page.monitor.cpuThreads)
            : ""

        Item {
            Layout.fillWidth: true
            implicitHeight: coreGrid.implicitHeight + Theme.spaceLg * 2

            GridLayout {
                id: coreGrid
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.margins: Theme.spaceLg
                columns: Math.max(1, Math.min(4, Math.floor(width / 190)))
                columnSpacing: Theme.spaceXl
                rowSpacing: Theme.spaceMd

                Repeater {
                    model: page.monitor.coreIds.length
                    NxMeter {
                        required property int index
                        readonly property real usage: page.monitor.coreUsage[index] ?? 0
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        toned: false
                        label: qsTr("CPU %1").arg(page.monitor.coreIds[index] ?? index)
                        valueText: page.monitor.ratesReady ? page.percent(usage) : "…"
                        value: usage
                    }
                }
            }
        }
    }

    // ---- hardware summary --------------------------------------------------------
    NxSection {
        title: qsTr("Hardware")
        description: page.monitor.systemInformationAvailable
            ? qsTr("System Information has the full details of this computer.")
            : qsTr("NEXUS System Information (nexus-sysinfo) is not installed, so the detailed view cannot be opened.")
        headerActions: NxButton {
            text: qsTr("System Information")
            iconName: "external-link"
            enabled: page.monitor.systemInformationAvailable
            onClicked: {
                if (!page.monitor.openSystemInformation())
                    ApplicationWindow.window.toaster.show(qsTr("System Information could not be started"), "danger")
            }
        }

        NxRow {
            icon: "cpu"
            title: qsTr("Processor")
            subtitle: page.monitor.cpuCores > 0 ? qsTr("%1 cores, %2 threads").arg(page.monitor.cpuCores).arg(page.monitor.cpuThreads) : ""
            value: page.monitor.cpuModel !== "" ? page.monitor.cpuModel : (page.monitor.ready ? qsTr("Not reported") : "…")
        }
        NxRow {
            icon: "memory"
            title: qsTr("Memory")
            subtitle: page.monitor.swapTotal > 0 ? qsTr("%1 of swap space").arg(Theme.formatBytes(page.monitor.swapTotal))
                                                 : (page.monitor.ready ? qsTr("No swap space configured") : "")
            value: page.monitor.ready ? Theme.formatBytes(page.monitor.memoryTotal) : "…"
        }
        Repeater {
            model: page.gpus.length
            NxRow {
                required property int index
                readonly property var gpu: page.gpus[index] ?? ({ name: "", driver: "" })
                icon: "monitor"
                title: qsTr("Graphics")
                subtitle: gpu.driver !== "" ? qsTr("%1 driver").arg(gpu.driver) : ""
                value: gpu.name
            }
        }
        NxRow {
            visible: page.gpus.length === 0
            icon: "monitor"
            title: qsTr("Graphics")
            value: page.monitor.ready ? qsTr("No graphics device detected") : "…"
        }
    }
}
