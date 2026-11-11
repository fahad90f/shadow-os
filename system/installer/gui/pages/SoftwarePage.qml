// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.installer

StepPage {
    id: page
    required property WizardState wiz

    readonly property bool online: Installer.probeState === "ready" && Installer.probe.network_online === true
    property string expanded: ""

    title: qsTr("Software")
    subtitle: qsTr("Optional tool profiles of NEXUS Arsenal. They are downloaded from the package repositories at the end of "
                 + "the installation, so they need a network connection — and they can just as well be installed later from NEXUS Arsenal.")

    function toggle(id, on) {
        const list = wiz.profiles.filter(x => x !== id)
        if (on) list.push(id)
        wiz.profiles = list
    }
    function entered() { if (!online && wiz.profiles.length > 0) wiz.profiles = [] }

    ScrollColumn {
        NxBanner {
            kind: page.online ? "info" : "warning"
            title: page.online ? qsTr("Selected profiles are downloaded during the installation")
                               : qsTr("No network connection: profiles cannot be installed now")
            message: page.online
                     ? qsTr("If a download fails, the installation still completes and the engine reports which profiles are missing.")
                     : qsTr("The system check found no network connection. Continue without profiles and install them later from "
                          + "NEXUS Arsenal, or connect to a network, go back to “System check” and choose “Check again”.")
        }

        NxSection {
            title: qsTr("Tool profiles")
            description: qsTr("From %1").arg(Installer.profilesDir)
            Repeater {
                model: Installer.profiles
                ColumnLayout {
                    id: entry
                    required property var modelData
                    required property int index
                    readonly property bool open: page.expanded === modelData.id
                    Layout.fillWidth: true
                    spacing: 0
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.leftMargin: Theme.spaceLg
                        visible: entry.index > 0
                        implicitHeight: 1
                        color: Theme.borderSubtle
                    }
                    NxRow {
                        icon: "package"
                        title: entry.modelData.name
                        subtitle: entry.modelData.summary !== "" ? entry.modelData.summary : entry.modelData.description
                        NxBadge {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: entry.modelData.installed
                            text: qsTr("Already included")
                            kind: "success"
                        }
                        NxButton {
                            anchors.verticalCenter: parent.verticalCenter
                            kind: "ghost"
                            width: 112
                            text: entry.modelData.toolCount === 1 ? qsTr("1 tool") : qsTr("%1 tools").arg(entry.modelData.toolCount)
                            iconName: entry.open ? "chevron-up" : "chevron-down"
                            Accessible.name: qsTr("Show the packages of %1").arg(entry.modelData.name)
                            onClicked: page.expanded = entry.open ? "" : entry.modelData.id
                        }
                        CheckBox {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: !entry.modelData.installed
                            enabled: page.online
                            checked: page.wiz.profiles.indexOf(entry.modelData.id) >= 0
                            Accessible.name: qsTr("Install %1").arg(entry.modelData.name)
                            onToggled: page.toggle(entry.modelData.id, checked)
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        Layout.leftMargin: Theme.spaceLg + Theme.icon + Theme.spaceMd
                        Layout.rightMargin: Theme.spaceLg
                        Layout.bottomMargin: Theme.spaceMd
                        visible: entry.open
                        wrapMode: Text.WordWrap
                        color: Theme.textSecondary
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fontSizeSmall
                        text: (entry.modelData.description !== "" && entry.modelData.summary !== "" ? entry.modelData.description + "\n\n" : "")
                              + (entry.modelData.includes.length > 0 ? qsTr("Includes the profiles: %1").arg(entry.modelData.includes.join(", ")) + "\n\n" : "")
                              + qsTr("Package %1").arg(entry.modelData.metapackage) + ": " + entry.modelData.packages.join(", ")
                    }
                }
            }
        }
    }
}
