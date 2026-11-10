// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.sysinfo

NxWindow {
    id: window
    title: qsTr("System Information")
    width: 860
    height: 760

    SystemInfo { id: info }

    Timer {
        interval: 30000
        running: window.visible
        repeat: true
        onTriggered: info.refreshUptime()
    }

    NxPage {
        anchors.fill: parent
        maxContentWidth: 760
        title: info.osName !== "" ? info.osName + " " + info.osVersion : qsTr("Unknown system")
        subtitle: [info.osCodename !== "" ? qsTr("Codename “%1”").arg(info.osCodename) : "",
                   info.osBase !== "" ? qsTr("based on %1").arg(info.osBase) : ""].filter(s => s !== "").join(" · ")
        actions: [
            NxButton {
                text: qsTr("Copy report")
                iconName: "copy"
                onClicked: { info.copyToClipboard(info.report()); window.toaster.show(qsTr("System report copied to the clipboard"), "success") }
            },
            NxIconButton {
                iconName: "refresh"
                tooltip: qsTr("Refresh")
                onClicked: info.refresh()
            }
        ]

        NxSection {
            title: qsTr("Software")
            NxRow { icon: "nexus"; title: qsTr("Operating system"); value: (info.osName + " " + info.osVersion).trim() }
            NxRow { icon: "cpu"; title: qsTr("Kernel"); value: info.kernel; monoValue: true }
            NxRow { icon: "layers"; title: qsTr("Architecture"); value: info.architecture; monoValue: true }
            NxRow { icon: "monitor"; title: qsTr("Desktop"); value: info.desktop === "" ? qsTr("Not in a desktop session") : info.desktop + (info.sessionType !== "" ? " (" + info.sessionType + ")" : "") }
            NxRow { icon: "code"; title: qsTr("Qt version"); value: info.qtVersion; monoValue: true }
            NxRow { icon: "server"; title: qsTr("Host name"); value: info.hostname; monoValue: true }
            NxRow { icon: "clock"; title: qsTr("Uptime"); value: Theme.formatDuration(info.uptimeSeconds) }
        }

        NxSection {
            title: qsTr("Hardware")
            NxRow {
                icon: "laptop"; title: qsTr("Computer")
                value: (info.systemVendor + " " + info.systemProduct).trim() || qsTr("Not reported by firmware")
            }
            NxRow { visible: info.boardName !== ""; icon: "memory"; title: qsTr("Mainboard"); value: info.boardName }
            NxRow {
                icon: "cpu"; title: qsTr("Processor")
                subtitle: qsTr("%1 cores, %2 threads").arg(info.cpuCores).arg(info.cpuThreads)
                value: info.cpuModel || qsTr("Unknown")
            }
            NxRow { icon: "memory"; title: qsTr("Memory"); value: Theme.formatBytes(info.memoryTotal) }
            NxRow { icon: "harddrive"; title: qsTr("Swap"); value: info.swapTotal > 0 ? Theme.formatBytes(info.swapTotal) : qsTr("None configured") }
            Repeater {
                model: info.gpus
                NxRow {
                    required property string modelData
                    icon: "monitor"; title: qsTr("Graphics"); value: modelData
                }
            }
            NxRow { visible: info.gpus.length === 0; icon: "monitor"; title: qsTr("Graphics"); value: qsTr("No graphics device detected") }
            NxRow { visible: info.virtualization !== ""; icon: "layers"; title: qsTr("Virtualization"); value: info.virtualization }
        }

        NxSection {
            title: qsTr("Firmware")
            NxRow { icon: "settings"; title: qsTr("Boot firmware"); value: info.firmwareType }
            NxRow { visible: info.firmwareVersion !== ""; icon: "info"; title: qsTr("Firmware version"); value: info.firmwareVersion }
            NxRow {
                icon: info.secureBoot === "Enabled" ? "shield-check" : "shield-off"
                iconColor: info.secureBoot === "Enabled" ? Theme.success : Theme.textSecondary
                title: qsTr("Secure Boot")
                NxBadge {
                    text: info.secureBoot
                    kind: info.secureBoot === "Enabled" ? "success" : info.secureBoot.startsWith("Disabled") ? "warning" : "neutral"
                }
            }
        }

        NxSection {
            title: qsTr("Storage")
            visible: info.filesystems.length > 0
            Repeater {
                model: info.filesystems
                Item {
                    id: fsRow
                    required property var modelData
                    required property int index
                    Layout.fillWidth: true
                    implicitHeight: meter.implicitHeight + Theme.spaceMd * 2
                    Rectangle {
                        visible: fsRow.index > 0
                        anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
                        anchors.leftMargin: Theme.spaceLg
                        height: 1; color: Theme.borderSubtle
                    }
                    NxMeter {
                        id: meter
                        anchors.left: parent.left; anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: Theme.spaceLg; anchors.rightMargin: Theme.spaceLg
                        label: fsRow.modelData.mount + "  ·  " + fsRow.modelData.device + "  ·  " + fsRow.modelData.type
                        valueText: qsTr("%1 of %2 used").arg(Theme.formatBytes(fsRow.modelData.used)).arg(Theme.formatBytes(fsRow.modelData.total))
                        value: fsRow.modelData.total > 0 ? fsRow.modelData.used / fsRow.modelData.total : 0
                    }
                }
            }
        }
    }
}
