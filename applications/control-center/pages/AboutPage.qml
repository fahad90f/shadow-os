// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.controlcenter

NxPage {
    id: page
    title: qsTr("About")
    subtitle: qsTr("The system installed on this computer.")

    AboutBackend { id: about }

    NxCard {
        Layout.fillWidth: true
        implicitHeight: 132
        RowLayout {
            anchors.fill: parent
            spacing: Theme.spaceXl

            Rectangle {
                implicitWidth: 84
                implicitHeight: 84
                radius: Theme.radiusXl
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0; color: Theme.brandGradientStart }
                    GradientStop { position: 0.55; color: Theme.brandGradientMid }
                    GradientStop { position: 1; color: Theme.brandGradientEnd }
                }
                NxIcon { anchors.centerIn: parent; name: "nexus"; size: 52; color: Theme.brandObsidian }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceXs
                Label {
                    Layout.fillWidth: true
                    text: about.osName !== "" ? about.osName : qsTr("Unknown system")
                    font.pixelSize: Theme.fontSizeDisplay
                    font.weight: Theme.weightSemibold
                    elide: Text.ElideRight
                }
                Label {
                    Layout.fillWidth: true
                    text: [about.osVersion !== "" ? qsTr("Version %1").arg(about.osVersion) : "",
                           about.osCodename !== "" ? "“" + about.osCodename + "”" : ""].filter(s => s !== "").join("  ")
                    color: Theme.textSecondary
                    elide: Text.ElideRight
                }
                NxBadge {
                    visible: about.osChannel !== ""
                    text: qsTr("%1 channel").arg(about.osChannel)
                    kind: about.osChannel === "stable" ? "success" : "warning"
                }
            }
        }
    }

    NxSection {
        title: qsTr("System")
        NxRow { icon: "layers"; title: qsTr("Based on"); value: about.baseSystem !== "" ? about.baseSystem : qsTr("Not declared in /etc/os-release") }
        NxRow { icon: "cpu"; title: qsTr("Kernel"); value: about.kernel; monoValue: true }
        NxRow { icon: "code"; title: qsTr("Architecture"); value: about.architecture; monoValue: true }
        NxRow { icon: "server"; title: qsTr("Host name"); value: about.hostname; monoValue: true }
        NxRow {
            icon: "info"
            title: qsTr("Hardware and firmware details")
            subtitle: about.systemInformationAvailable ? qsTr("Processor, memory, graphics, firmware and Secure Boot state")
                                                       : qsTr("NEXUS System Information is not installed")
            chevron: about.systemInformationAvailable
            enabled: about.systemInformationAvailable
            onClicked: if (!about.openSystemInformation()) ApplicationWindow.window.toaster.show(qsTr("Could not start System Information"), "danger")
        }
    }

    NxSection {
        title: qsTr("Project")
        visible: about.homeUrl !== "" || about.bugUrl !== ""
        NxRow {
            visible: about.homeUrl !== ""
            icon: "globe"; title: qsTr("Website"); value: about.homeUrl
            chevron: true
            onClicked: Qt.openUrlExternally(about.homeUrl)
        }
        NxRow {
            visible: about.bugUrl !== ""
            icon: "bug"; title: qsTr("Report a problem"); value: about.bugUrl
            chevron: true
            onClicked: Qt.openUrlExternally(about.bugUrl)
        }
    }
}
