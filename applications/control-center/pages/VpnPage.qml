// SPDX-License-Identifier: GPL-3.0-or-later
import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.controlcenter

NxPage {
    id: page
    title: qsTr("VPN")
    subtitle: qsTr("WireGuard tunnels and NetworkManager VPN connections.")

    // Bumped when NetworkManager refuses a change, so check boxes fall back to the real state.
    property int revision: 0
    property string removeUuid: ""
    property string removeName: ""

    VpnBackend {
        id: vpn
        onFailed: (message) => {
            page.revision++
            page.ApplicationWindow.window.toaster.show(message, "danger")
        }
        onImported: (name) => page.ApplicationWindow.window.toaster.show(qsTr("Imported %1").arg(name), "success")
    }

    component EmptyRow: Item {
        property alias text: emptyLabel.text
        Layout.fillWidth: true
        implicitHeight: Theme.rowHeight
        Label {
            id: emptyLabel
            anchors.fill: parent
            anchors.leftMargin: Theme.spaceLg
            anchors.rightMargin: Theme.spaceLg
            verticalAlignment: Text.AlignVCenter
            color: Theme.textMuted
            elide: Text.ElideRight
        }
    }

    NxCard {
        visible: vpn.ready && !vpn.available
        Layout.fillWidth: true
        implicitHeight: 320
        NxEmptyState {
            anchors.fill: parent
            icon: "shield-off"
            tone: "warning"
            title: vpn.state === "nm-missing" ? qsTr("NetworkManager is not installed") : qsTr("NetworkManager is not running")
            message: vpn.statusMessage
            actionText: qsTr("Check again")
            onActionTriggered: vpn.refresh()
        }
    }

    NxSection {
        visible: vpn.available
        title: qsTr("Connections")
        description: qsTr("Import a WireGuard configuration (.conf) or an OpenVPN profile (.ovpn) you received from your VPN provider or administrator.")
        headerActions: [
            NxButton {
                text: qsTr("Import…")
                iconName: "download"
                busy: vpn.importing
                onClicked: fileDialog.open()
            }
        ]

        Repeater {
            model: vpn.profiles
            NxRow {
                id: profile
                required property string uuid
                required property string name
                required property string kind
                required property string typeLabel
                required property string stateId
                required property string stateText
                required property bool active
                required property bool autoconnect
                required property string autoconnectNote
                required property string problem
                required property bool busy

                icon: stateId === "active" ? "shield-check" : "shield"
                iconColor: stateId === "active" ? Theme.success : problem !== "" ? Theme.warning : Theme.textSecondary
                title: name
                subtitle: {
                    const parts = [typeLabel + "  ·  " + stateText]
                    if (problem !== "") parts.push(problem)
                    else if (autoconnectNote !== "") parts.push(autoconnectNote)
                    return parts.join("\n")
                }

                CheckBox {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Automatic")
                    checked: (page.revision, profile.autoconnect)
                    enabled: !profile.busy && profile.problem === ""
                    Accessible.name: qsTr("Connect %1 automatically").arg(profile.name)
                    ToolTip.visible: hovered
                    ToolTip.text: profile.kind === "wireguard" ? qsTr("Connect whenever the computer is online")
                                                               : qsTr("Start together with the network that is connected now")
                    onToggled: vpn.setAutoconnect(profile.uuid, checked)
                }
                NxButton {
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.max(implicitWidth, 104)
                    text: profile.active ? qsTr("Disconnect") : qsTr("Connect")
                    kind: profile.active ? "secondary" : "primary"
                    enabled: profile.active || profile.problem === ""
                    busy: profile.busy
                    Accessible.name: profile.active ? qsTr("Disconnect %1").arg(profile.name) : qsTr("Connect %1").arg(profile.name)
                    onClicked: profile.active ? vpn.disconnectProfile(profile.uuid) : vpn.connectProfile(profile.uuid)
                }
                NxIconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    iconName: "trash"
                    tooltip: qsTr("Delete %1").arg(profile.name)
                    enabled: !profile.busy
                    onClicked: {
                        page.removeUuid = profile.uuid
                        page.removeName = profile.name
                        removeDialog.open()
                    }
                }
            }
        }
        EmptyRow {
            visible: vpn.profileCount === 0
            text: qsTr("No VPN connection is set up. Import a profile to add one.")
        }
    }

    NxSection {
        visible: vpn.available
        title: qsTr("Supported VPN types")
        description: qsTr("WireGuard is part of NetworkManager. Other VPN types need a NetworkManager plug-in.")

        NxRow {
            icon: "check-circle"
            iconColor: Theme.success
            title: "WireGuard"
            subtitle: qsTr("Built into NetworkManager — imports .conf files")
        }
        NxRow {
            icon: vpn.openvpnInstalled ? "check-circle" : "alert-triangle"
            iconColor: vpn.openvpnInstalled ? Theme.success : Theme.warning
            title: "OpenVPN"
            subtitle: vpn.openvpnInstalled ? qsTr("Plug-in installed — imports .ovpn files")
                                           : qsTr("Not installed: .ovpn files cannot be imported or connected until the package network-manager-openvpn is installed.")
        }
        Repeater {
            model: vpn.plugins.filter(p => p.service !== "org.freedesktop.NetworkManager.openvpn")
            NxRow {
                required property var modelData
                icon: "check-circle"
                iconColor: Theme.success
                title: modelData.label
                subtitle: qsTr("Plug-in installed (%1)").arg(modelData.service)
            }
        }
    }

    FileDialog {
        id: fileDialog
        title: qsTr("Import a VPN profile")
        fileMode: FileDialog.OpenFile
        currentFolder: StandardPaths.writableLocation(StandardPaths.HomeLocation)
        nameFilters: [qsTr("VPN profiles (*.conf *.ovpn)"), qsTr("WireGuard configuration (*.conf)"), qsTr("OpenVPN profile (*.ovpn)")]
        onAccepted: vpn.importFile(selectedFile)
    }

    NxConfirmDialog {
        id: removeDialog
        title: qsTr("Delete %1?").arg(page.removeName)
        message: qsTr("The VPN profile and the keys or passwords stored in it are deleted from this computer. This cannot be undone.")
        confirmText: qsTr("Delete")
        destructive: true
        onConfirmed: vpn.removeProfile(page.removeUuid)
    }
}
