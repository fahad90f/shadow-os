// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.networkcenter

NxPage {
    id: page
    title: qsTr("VPN")
    subtitle: qsTr("WireGuard and VPN plug-in connections of NetworkManager: connect, disconnect, import and remove.")
    maxContentWidth: 1000

    property int importTypeIndex: 0
    readonly property string importType: importTypeIndex === 0 ? "wireguard" : "openvpn"
    // WireGuard is built into NetworkManager; everything else needs a plug-in.
    readonly property bool importPluginMissing: importType !== "wireguard" && Nm.vpnPlugins.indexOf(importType) < 0

    function stateText(connection) {
        return connection.state === "activated" ? qsTr("Connected")
            : connection.state === "activating" ? qsTr("Connecting…")
            : connection.state === "deactivating" ? qsTr("Disconnecting…")
            : qsTr("Not connected")
    }
    function usePath(path) {
        pathField.text = path
        const detected = Nm.sniffVpnType(path)
        if (detected === "wireguard") importTypeIndex = 0
        else if (detected === "openvpn") importTypeIndex = 1
    }

    actions: [
        NxIconButton {
            iconName: "refresh"
            tooltip: qsTr("Refresh")
            onClicked: Nm.refresh()
        }
    ]

    NxConfirmDialog {
        id: deleteDialog
        property string uuid: ""
        property string name: ""
        title: qsTr("Remove “%1”?").arg(name)
        message: qsTr("The VPN connection and its stored keys or passwords are deleted from NetworkManager. This cannot be undone; you would have to import the configuration again.")
        confirmText: qsTr("Remove VPN")
        destructive: true
        onConfirmed: Nm.deleteConnection(uuid)
    }

    FilePickerDialog {
        id: filePicker
        title: qsTr("Choose a VPN configuration")
        nameFilters: ["*.conf", "*.ovpn"]
        filterLabel: qsTr("*.conf and *.ovpn")
        onFileChosen: (path) => page.usePath(path)
    }

    NmUnavailable {
        Layout.fillWidth: true
        Layout.topMargin: Theme.spaceXl
        visible: !Nm.running
        feature: qsTr("VPN connections are NetworkManager profiles; they are listed, activated and imported through it.")
    }

    NxSection {
        visible: Nm.running
        title: qsTr("VPN connections")

        Repeater {
            model: Nm.vpnConnections
            NxRow {
                id: vpnRow
                required property var modelData
                readonly property bool up: modelData.state === "activated"
                readonly property bool working: Nm.busyTargets.indexOf(modelData.uuid) >= 0
                icon: up ? "shield-check" : "shield"
                iconColor: up ? Theme.success : Theme.textSecondary
                title: modelData.name
                subtitle: [modelData.vpnLabel, modelData.device !== "" ? qsTr("interface %1").arg(modelData.device) : "",
                           modelData.autoconnect ? qsTr("connects automatically") : qsTr("connects on request")].filter(s => s !== "").join(" · ")

                NxBadge {
                    kind: vpnRow.up ? "success" : vpnRow.modelData.state !== "" ? "warning" : "neutral"
                    text: page.stateText(vpnRow.modelData)
                }
                Switch {
                    text: qsTr("Auto")
                    checked: vpnRow.modelData.autoconnect
                    enabled: !vpnRow.working && !vpnRow.modelData.readOnly
                    Accessible.name: qsTr("Connect %1 automatically").arg(vpnRow.modelData.name)
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Let NetworkManager bring this VPN up by itself")
                    onToggled: Nm.setAutoconnect(vpnRow.modelData.uuid, checked)
                }
                NxButton {
                    text: vpnRow.modelData.state === "" ? qsTr("Connect") : qsTr("Disconnect")
                    kind: vpnRow.modelData.state === "" ? "primary" : "secondary"
                    busy: vpnRow.working
                    onClicked: vpnRow.modelData.state === "" ? Nm.activateConnection(vpnRow.modelData.uuid)
                                                             : Nm.deactivateConnection(vpnRow.modelData.uuid)
                }
                NxIconButton {
                    iconName: "trash"
                    tooltip: qsTr("Remove %1").arg(vpnRow.modelData.name)
                    enabled: !vpnRow.working && !vpnRow.modelData.readOnly
                    onClicked: {
                        deleteDialog.uuid = vpnRow.modelData.uuid
                        deleteDialog.name = vpnRow.modelData.name
                        deleteDialog.open()
                    }
                }
            }
        }
        NxRow {
            visible: Nm.vpnConnections.length === 0
            icon: "info"
            title: qsTr("No VPN connections")
            subtitle: qsTr("Import a WireGuard or OpenVPN configuration file below.")
        }
    }

    NxSection {
        visible: Nm.running
        title: qsTr("Import a configuration")
        description: qsTr("NetworkManager reads the file and creates a connection from it (nmcli connection import). The file itself is not changed.")

        FormRow {
            label: qsTr("Configuration file")
            hint: qsTr("A WireGuard .conf or an OpenVPN .ovpn file")
            TextField {
                id: pathField
                Layout.fillWidth: true
                placeholderText: qsTr("/path/to/file.conf")
                Accessible.name: qsTr("Configuration file")
                onEditingFinished: if (text !== "") page.usePath(text.trim())
            }
            NxButton {
                text: qsTr("Browse…")
                iconName: "folder-open"
                onClicked: filePicker.open()
            }
        }
        FormRow {
            label: qsTr("Type")
            hint: qsTr("Detected from the file where possible")
            error: page.importPluginMissing
                ? qsTr("NetworkManager's OpenVPN plug-in is not installed (package network-manager-openvpn), so OpenVPN files cannot be imported.")
                : ""
            NxSegmented {
                model: ["WireGuard", "OpenVPN"]
                currentIndex: page.importTypeIndex
                onActivated: (index) => page.importTypeIndex = index
            }
            Item { Layout.fillWidth: true }
            NxButton {
                text: qsTr("Import")
                kind: "primary"
                iconName: "download"
                busy: Nm.busyTargets.indexOf(pathField.text.trim()) >= 0
                enabled: pathField.text.trim() !== "" && !page.importPluginMissing
                onClicked: Nm.importVpn(page.importType, pathField.text.trim())
            }
        }
    }

    NxSection {
        visible: Nm.running
        title: qsTr("VPN support in NetworkManager")

        NxRow {
            icon: "check-circle"
            iconColor: Theme.success
            title: qsTr("WireGuard")
            value: qsTr("Built into NetworkManager")
        }
        NxRow {
            readonly property bool installed: Nm.vpnPlugins.indexOf("openvpn") >= 0
            icon: installed ? "check-circle" : "x-circle"
            iconColor: installed ? Theme.success : Theme.textMuted
            title: qsTr("OpenVPN")
            value: installed ? qsTr("Plug-in installed") : qsTr("Plug-in not installed (network-manager-openvpn)")
        }
        NxRow {
            visible: Nm.vpnPlugins.filter(p => p !== "openvpn").length > 0
            icon: "package"
            title: qsTr("Other plug-ins")
            value: Nm.vpnPlugins.filter(p => p !== "openvpn").join(", ")
        }
    }
}
