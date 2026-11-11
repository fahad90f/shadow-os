// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.monitor

NxPage {
    id: page

    required property SystemMonitor monitor

    readonly property var temperatures: monitor.temperatures
    readonly property var fans: monitor.fans
    readonly property var gpus: monitor.gpus
    readonly property var batteries: monitor.batteries
    readonly property string spanText: {
        const seconds = monitor.historySize * monitor.interval / 1000
        return seconds < 120 ? qsTr("Last %1 seconds").arg(seconds) : qsTr("Last %1 minutes").arg(Math.round(seconds / 60))
    }
    readonly property var batteryStates: ({
        "Charging": qsTr("Charging"), "Discharging": qsTr("Discharging"), "Full": qsTr("Fully charged"),
        "Not charging": qsTr("Not charging"), "Unknown": qsTr("State unknown"),
    })

    function celsius(value) { return Math.round(value) + " °C" }
    function levelColor(level) {
        return level === "critical" ? Theme.danger : level === "high" ? Theme.warning : Theme.textPrimary
    }
    function has(value) { return value !== null && value !== undefined }

    title: qsTr("Sensors")
    subtitle: qsTr("Temperatures, fans, graphics devices and batteries, as far as this machine reports them.")
    maxContentWidth: 1080

    // ---- processor temperature history -------------------------------------------
    HistoryCard {
        id: temperatureCard
        Layout.fillWidth: true
        visible: page.monitor.cpuTemperatureAvailable
        title: qsTr("Processor temperature")
        capacity: page.monitor.historySize
        minScale: 100
        chartHeight: 110
        series: [{ label: "", value: page.celsius(page.monitor.cpuTemperature),
                   values: page.monitor.temperatureHistory, color: Theme.accentAmber }]
        caption: qsTr("%1 · %2").arg(page.spanText).arg(page.monitor.cpuTemperatureSource)
        scaleCaption: qsTr("0 – %1").arg(page.celsius(temperatureCard.valueScale))
    }

    // ---- temperatures -------------------------------------------------------------
    NxSection {
        title: qsTr("Temperatures")

        Repeater {
            model: page.temperatures.length
            ListRow {
                id: tempRow
                required property int index
                readonly property var reading: page.temperatures[index]
                    ?? ({ chip: "", device: "", label: "", celsius: 0, high: null, critical: null, level: "normal" })

                first: index === 0
                icon: "thermometer"
                iconColor: reading.level === "normal" ? Theme.textSecondary : page.levelColor(reading.level)
                title: reading.label
                subtitle: {
                    const parts = [reading.device !== "" && reading.device !== reading.chip
                                   ? reading.chip + " (" + reading.device + ")" : reading.chip]
                    if (page.has(reading.high))
                        parts.push(qsTr("high at %1").arg(page.celsius(reading.high)))
                    if (page.has(reading.critical))
                        parts.push(qsTr("critical at %1").arg(page.celsius(reading.critical)))
                    return parts.join(" · ")
                }

                NxBadge {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: tempRow.reading.level !== "normal"
                    text: tempRow.reading.level === "critical" ? qsTr("Critical") : qsTr("High")
                    kind: tempRow.reading.level === "critical" ? "danger" : "warning"
                }
                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    text: page.celsius(tempRow.reading.celsius)
                    color: page.levelColor(tempRow.reading.level)
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fontSizeBody
                }
            }
        }
        ListRow {
            first: true
            visible: page.temperatures.length === 0
            icon: "info"
            title: page.monitor.ready ? qsTr("No temperature sensor is available") : qsTr("Looking for sensors…")
            subtitle: page.monitor.ready
                ? qsTr("The kernel exposes no hardware-monitoring device (/sys/class/hwmon) and no thermal zone on this machine. Virtual machines usually have none.")
                : ""
        }
    }

    // ---- fans ----------------------------------------------------------------------
    NxSection {
        title: qsTr("Fans")

        Repeater {
            model: page.fans.length
            ListRow {
                id: fanRow
                required property int index
                readonly property var reading: page.fans[index] ?? ({ chip: "", device: "", label: "", rpm: 0 })

                first: index === 0
                icon: "refresh"
                title: reading.label
                subtitle: reading.device !== "" && reading.device !== reading.chip
                    ? reading.chip + " (" + reading.device + ")" : reading.chip

                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    text: fanRow.reading.rpm > 0 ? qsTr("%1 RPM").arg(fanRow.reading.rpm) : qsTr("Stopped")
                    color: fanRow.reading.rpm > 0 ? Theme.textPrimary : Theme.textMuted
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fontSizeBody
                }
            }
        }
        ListRow {
            first: true
            visible: page.fans.length === 0
            icon: "info"
            title: page.monitor.ready ? qsTr("No fan speed is reported") : qsTr("Looking for sensors…")
            subtitle: page.monitor.ready
                ? qsTr("No hardware-monitoring device of this machine exposes a fan tachometer.") : ""
        }
    }

    // ---- graphics devices ---------------------------------------------------------
    NxSection {
        title: qsTr("Graphics")

        Repeater {
            model: page.gpus.length
            ListRow {
                id: gpuRow
                required property int index
                readonly property var gpu: page.gpus[index]
                    ?? ({ card: "", name: "", driver: "", note: "", suspended: false,
                          utilizationAvailable: false, utilizationPercent: null,
                          temperatureAvailable: false, temperatureCelsius: null,
                          memoryAvailable: false, memoryUsed: null, memoryTotal: null, powerWatts: null, fanRpm: null })

                first: index === 0
                icon: "monitor"
                title: gpu.name
                subtitle: [gpu.driver !== "" ? qsTr("%1 driver").arg(gpu.driver) : "", gpu.card, gpu.note]
                    .filter(s => s !== "").join(" · ")

                Figure {
                    anchors.verticalCenter: parent.verticalCenter
                    minimumWidth: 76
                    label: qsTr("Utilisation")
                    value: gpuRow.gpu.utilizationAvailable ? Math.round(gpuRow.gpu.utilizationPercent) + " %" : "—"
                    valueColor: gpuRow.gpu.utilizationAvailable ? Theme.textPrimary : Theme.textMuted
                }
                Figure {
                    anchors.verticalCenter: parent.verticalCenter
                    minimumWidth: 76
                    label: qsTr("Temperature")
                    value: gpuRow.gpu.temperatureAvailable ? page.celsius(gpuRow.gpu.temperatureCelsius) : "—"
                    valueColor: gpuRow.gpu.temperatureAvailable ? Theme.textPrimary : Theme.textMuted
                }
                Figure {
                    anchors.verticalCenter: parent.verticalCenter
                    minimumWidth: 150
                    label: qsTr("Video memory")
                    value: gpuRow.gpu.memoryAvailable
                        ? qsTr("%1 of %2").arg(Theme.formatBytes(gpuRow.gpu.memoryUsed)).arg(Theme.formatBytes(gpuRow.gpu.memoryTotal))
                        : "—"
                    valueColor: gpuRow.gpu.memoryAvailable ? Theme.textPrimary : Theme.textMuted
                }
                Figure {
                    anchors.verticalCenter: parent.verticalCenter
                    minimumWidth: 64
                    label: qsTr("Power")
                    value: page.has(gpuRow.gpu.powerWatts) ? gpuRow.gpu.powerWatts.toFixed(1) + " W" : "—"
                    valueColor: page.has(gpuRow.gpu.powerWatts) ? Theme.textPrimary : Theme.textMuted
                }
            }
        }
        ListRow {
            first: true
            visible: page.gpus.length === 0
            icon: "info"
            title: page.monitor.ready ? qsTr("No graphics device detected") : qsTr("Looking for graphics devices…")
            subtitle: page.monitor.ready
                ? qsTr("The kernel registered no graphics device (/sys/class/drm is empty or missing).") : ""
        }
    }

    // ---- batteries -----------------------------------------------------------------
    NxSection {
        title: qsTr("Battery")
        // Only where the machine has a battery of its own: a desktop's USB-C
        // port also registers as a (disconnected) power supply.
        description: !page.monitor.externalPowerKnown || !page.batteries.some(b => !b.peripheral) ? ""
            : page.monitor.externalPowerOnline ? qsTr("External power is connected.")
            : qsTr("No external power is connected.")

        Repeater {
            model: page.batteries.length
            Item {
                id: batteryRow
                required property int index
                readonly property var battery: page.batteries[index]
                    ?? ({ name: "", model: "", manufacturer: "", technology: "", peripheral: false, status: "",
                          capacityLevel: "", health: "", percentage: null, rateWatts: null, energyNowWh: null,
                          energyFullWh: null, energyDesignWh: null, capacityHealthPercent: null, cycles: null,
                          voltage: null, temperatureCelsius: null, secondsRemaining: null, timeEstimated: false })
                readonly property bool charging: battery.status === "Charging"
                readonly property bool hasPercentage: page.has(battery.percentage)
                readonly property string heading: [battery.peripheral ? qsTr("Device battery") : qsTr("Battery"), battery.name,
                                                   [battery.manufacturer, battery.model].filter(s => s !== "").join(" ")]
                    .filter(s => s !== "").join(" · ")
                readonly property string state: page.batteryStates[battery.status] ?? battery.status

                Layout.fillWidth: true
                implicitHeight: batteryColumn.implicitHeight + Theme.spaceLg * 2

                Rectangle {
                    visible: batteryRow.index > 0
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: Theme.spaceLg
                    height: 1
                    color: Theme.borderSubtle
                }
                ColumnLayout {
                    id: batteryColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: Theme.spaceLg
                    anchors.rightMargin: Theme.spaceLg
                    spacing: Theme.spaceMd

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spaceMd
                        NxIcon {
                            name: batteryRow.charging ? "battery-charging" : "battery"
                            size: Theme.icon
                            color: batteryRow.charging ? Theme.success : Theme.textSecondary
                        }
                        NxMeter {
                            Layout.fillWidth: true
                            visible: batteryRow.hasPercentage
                            // A battery is not a disk: only a nearly empty one is alarming.
                            toned: false
                            color: batteryRow.charging || !batteryRow.hasPercentage || batteryRow.battery.percentage > 20 ? Theme.success
                                : batteryRow.battery.percentage > 10 ? Theme.warning : Theme.danger
                            label: batteryRow.heading
                            valueText: [batteryRow.hasPercentage ? Math.round(batteryRow.battery.percentage) + " %" : "", batteryRow.state]
                                .filter(s => s !== "").join(" · ")
                            value: batteryRow.hasPercentage ? batteryRow.battery.percentage / 100 : 0
                        }
                        // A battery that reports no percentage (many wireless
                        // peripherals only give a level) gets no bar: an empty
                        // one would read as "0 %".
                        Text {
                            Layout.fillWidth: true
                            visible: !batteryRow.hasPercentage
                            text: batteryRow.heading
                            color: Theme.textSecondary
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeSmall
                            elide: Text.ElideRight
                        }
                        Text {
                            visible: !batteryRow.hasPercentage
                            text: [batteryRow.battery.capacityLevel !== "" ? qsTr("Level: %1").arg(batteryRow.battery.capacityLevel)
                                                                           : qsTr("Charge not reported"),
                                   batteryRow.state].filter(s => s !== "").join(" · ")
                            color: Theme.textPrimary
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeSmall
                            font.weight: Theme.weightMedium
                        }
                    }
                    Flow {
                        Layout.fillWidth: true
                        Layout.leftMargin: Theme.icon + Theme.spaceMd
                        spacing: Theme.spaceXl

                        Figure {
                            visible: page.has(batteryRow.battery.secondsRemaining)
                            minimumWidth: 0
                            rightAligned: false
                            label: batteryRow.charging ? qsTr("Until full") : qsTr("Remaining")
                            value: page.has(batteryRow.battery.secondsRemaining)
                                ? (batteryRow.battery.timeEstimated ? qsTr("about %1") : "%1")
                                      .arg(Theme.formatDuration(batteryRow.battery.secondsRemaining))
                                : ""
                        }
                        Figure {
                            visible: page.has(batteryRow.battery.rateWatts)
                            minimumWidth: 0
                            rightAligned: false
                            label: batteryRow.charging ? qsTr("Charging rate") : qsTr("Power draw")
                            value: page.has(batteryRow.battery.rateWatts) ? batteryRow.battery.rateWatts.toFixed(1) + " W" : ""
                        }
                        Figure {
                            visible: page.has(batteryRow.battery.energyNowWh)
                            minimumWidth: 0
                            rightAligned: false
                            label: qsTr("Charge")
                            value: page.has(batteryRow.battery.energyNowWh)
                                ? qsTr("%1 of %2 Wh").arg(batteryRow.battery.energyNowWh.toFixed(1)).arg(batteryRow.battery.energyFullWh.toFixed(1))
                                : ""
                        }
                        Figure {
                            visible: page.has(batteryRow.battery.capacityHealthPercent)
                            minimumWidth: 0
                            rightAligned: false
                            label: qsTr("Capacity vs. design")
                            value: page.has(batteryRow.battery.capacityHealthPercent)
                                ? Math.round(batteryRow.battery.capacityHealthPercent) + " %"
                                  + (page.has(batteryRow.battery.energyDesignWh)
                                     ? " (" + qsTr("%1 Wh new").arg(batteryRow.battery.energyDesignWh.toFixed(1)) + ")" : "")
                                : ""
                            valueColor: page.has(batteryRow.battery.capacityHealthPercent) && batteryRow.battery.capacityHealthPercent < 60
                                ? Theme.warning : Theme.textPrimary
                        }
                        Figure {
                            visible: batteryRow.battery.health !== ""
                            minimumWidth: 0
                            rightAligned: false
                            label: qsTr("Health")
                            value: batteryRow.battery.health
                        }
                        Figure {
                            visible: page.has(batteryRow.battery.cycles)
                            minimumWidth: 0
                            rightAligned: false
                            label: qsTr("Charge cycles")
                            value: page.has(batteryRow.battery.cycles) ? String(batteryRow.battery.cycles) : ""
                        }
                        Figure {
                            visible: page.has(batteryRow.battery.voltage)
                            minimumWidth: 0
                            rightAligned: false
                            label: qsTr("Voltage")
                            value: page.has(batteryRow.battery.voltage) ? batteryRow.battery.voltage.toFixed(2) + " V" : ""
                        }
                        Figure {
                            visible: page.has(batteryRow.battery.temperatureCelsius)
                            minimumWidth: 0
                            rightAligned: false
                            label: qsTr("Temperature")
                            value: page.has(batteryRow.battery.temperatureCelsius) ? page.celsius(batteryRow.battery.temperatureCelsius) : ""
                        }
                        Figure {
                            visible: batteryRow.battery.technology !== ""
                            minimumWidth: 0
                            rightAligned: false
                            label: qsTr("Technology")
                            value: batteryRow.battery.technology
                        }
                    }
                }
            }
        }
        ListRow {
            first: true
            visible: page.batteries.length === 0
            icon: "battery"
            title: page.monitor.ready ? qsTr("No battery detected") : qsTr("Looking for batteries…")
            subtitle: page.monitor.ready
                ? qsTr("The kernel registered no battery (/sys/class/power_supply). Desktop computers and virtual machines have none.")
                : ""
        }
    }
}
