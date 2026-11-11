// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.controlcenter

NxPage {
    id: page
    title: qsTr("Firewall")
    subtitle: qsTr("Filters connections coming in from the network. Everything is blocked unless a port is opened here.")

    // Bumped when a change is refused, so the switch shows the real state again.
    property int revision: 0
    property string removePort: ""
    property string removeProtocol: ""

    function policyText(policy) {
        if (policy === "drop") return qsTr("Blocked unless allowed (drop)")
        if (policy === "accept") return qsTr("Allowed (accept)")
        return policy !== "" ? policy : qsTr("Not loaded")
    }

    actions: [
        NxButton {
            text: qsTr("Refresh")
            iconName: "refresh"
            enabled: !firewall.busy
            onClicked: firewall.refresh()
        }
    ]

    FirewallBackend {
        id: firewall
        onFailed: (message) => {
            page.revision++
            page.ApplicationWindow.window.toaster.show(message, "danger")
        }
        onSucceeded: (message) => page.ApplicationWindow.window.toaster.show(message, "success")
    }

    NxEmptyState {
        visible: firewall.ready && !firewall.installed
        Layout.fillWidth: true
        tone: "warning"
        icon: "shield-off"
        title: qsTr("The firewall cannot be managed here")
        message: firewall.statusMessage
    }

    NxBanner {
        visible: firewall.installed && firewall.ready && !firewall.statusKnown
        kind: "warning"
        title: qsTr("The current state of the firewall could not be read")
        message: firewall.statusMessage
        actionText: qsTr("Try again")
        onActionTriggered: firewall.refresh()
    }

    NxBanner {
        visible: firewall.statusKnown && !firewall.active
        kind: "danger"
        title: qsTr("The firewall is not active")
        message: qsTr("Connections from the network to this computer are not filtered.")
    }

    NxBanner {
        visible: firewall.statusKnown && !firewall.rulesetInstalled
        kind: "warning"
        title: qsTr("The NEXUS ruleset is missing")
        message: qsTr("/etc/nftables.conf does not exist, so the firewall cannot be loaded. Reinstall the nexus-hardening package.")
    }

    NxSection {
        visible: firewall.installed
        title: qsTr("Status")

        NxRow {
            icon: !firewall.statusKnown ? "shield" : firewall.active ? "shield-check" : "shield-off"
            iconColor: !firewall.statusKnown ? Theme.textSecondary : firewall.active ? Theme.success : Theme.danger
            title: !firewall.ready ? qsTr("Reading the firewall state…")
                 : !firewall.statusKnown ? qsTr("State unknown")
                 : firewall.active ? qsTr("The firewall is active") : qsTr("The firewall is not active")
            subtitle: qsTr("Changing the firewall asks for an administrator password.")
            BusyIndicator {
                anchors.verticalCenter: parent.verticalCenter
                width: Theme.icon
                height: Theme.icon
                padding: 0
                visible: running
                running: firewall.busy || !firewall.ready
            }
            Switch {
                anchors.verticalCenter: parent.verticalCenter
                enabled: firewall.ready && !firewall.busy
                checked: (page.revision, firewall.statusKnown && firewall.active)
                Accessible.name: qsTr("Firewall")
                onToggled: {
                    const wanted = checked
                    checked = Qt.binding(() => (page.revision, firewall.statusKnown && firewall.active))
                    if (wanted) firewall.setEnabled(true)
                    else disableDialog.open()
                }
            }
        }
        NxRow {
            visible: firewall.statusKnown
            icon: "power"
            title: qsTr("Start when the computer starts")
            subtitle: firewall.active && !firewall.enabledAtBoot ? qsTr("Turn the firewall on again to enable it at boot.") : ""
            value: firewall.enabledAtBoot ? qsTr("Yes") : qsTr("No")
        }
        NxRow {
            visible: firewall.statusKnown
            icon: "arrow-down"
            title: qsTr("Incoming connections")
            value: page.policyText(firewall.inputPolicy)
        }
        NxRow {
            visible: firewall.statusKnown
            icon: "share"
            title: qsTr("Forwarded connections")
            value: page.policyText(firewall.forwardPolicy)
        }
        NxRow {
            visible: firewall.statusKnown
            icon: "list"
            title: qsTr("Rules loaded in the kernel")
            subtitle: qsTr("nftables table “inet nexus_filter”")
            value: String(firewall.ruleCount)
            monoValue: true
        }
        NxRow {
            visible: firewall.statusKnown && firewall.otherTables.length > 0
            icon: "layers"
            title: qsTr("Other nftables tables")
            subtitle: qsTr("Created by other software (containers, virtual machines, VPN clients). They are not managed here: %1")
                .arg(firewall.otherTables.join(", "))
            value: String(firewall.otherTables.length)
            monoValue: true
        }
    }

    NxSection {
        visible: firewall.installed
        title: qsTr("Open ports")
        description: firewall.statusKnown ? qsTr("Other computers can connect to these ports. Recorded in /etc/nexus/firewall.json.")
                                          : qsTr("As recorded in /etc/nexus/firewall.json.")
        headerActions: [
            NxButton {
                text: qsTr("Reload")
                enabled: firewall.ready && !firewall.busy && firewall.statusKnown && firewall.active
                onClicked: firewall.reload()
            },
            NxButton {
                text: qsTr("Close all…")
                enabled: firewall.ready && !firewall.busy && firewall.rules.length > 0
                onClicked: resetDialog.open()
            },
            NxButton {
                text: qsTr("Open a port…")
                kind: "primary"
                iconName: "plus"
                enabled: firewall.ready && !firewall.busy
                onClicked: addDialog.open()
            }
        ]

        Repeater {
            model: firewall.rules
            NxRow {
                id: ruleRow
                required property var modelData
                icon: "arrow-down"
                title: modelData.port + "/" + modelData.protocol
                subtitle: modelData.name !== "" ? modelData.name
                        : modelData.port.indexOf("-") >= 0 ? qsTr("Port range, no description") : qsTr("No description")
                NxBadge {
                    anchors.verticalCenter: parent.verticalCenter
                    text: ruleRow.modelData.protocol.toUpperCase()
                    dot: false
                }
                NxIconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    iconName: "trash"
                    tooltip: qsTr("Close %1/%2").arg(ruleRow.modelData.port).arg(ruleRow.modelData.protocol)
                    enabled: !firewall.busy
                    onClicked: {
                        page.removePort = ruleRow.modelData.port
                        page.removeProtocol = ruleRow.modelData.protocol
                        removeDialog.open()
                    }
                }
            }
        }
        NxRow {
            visible: firewall.rules.length === 0
            icon: "lock"
            title: firewall.ready ? qsTr("No ports are open") : qsTr("Reading…")
            subtitle: firewall.ready ? qsTr("Nothing on this computer can be reached from the network through the firewall.") : ""
        }
    }

    Dialog {
        id: addDialog
        title: qsTr("Open a port")
        width: Math.min(460, page.width - Theme.spaceXxl)
        footer: null
        readonly property string protocol: protocolBox.currentIndex === 1 ? "udp" : "tcp"
        readonly property string problem: {
            const p = firewall.portsProblem(portField.text)
            return p !== "" ? p : firewall.nameProblem(nameField.text)
        }
        function submit() {
            if (problem !== "") return
            firewall.allow(portField.text.trim(), protocol, nameField.text.trim())
            close()
        }
        onOpened: { portField.text = ""; nameField.text = ""; protocolBox.currentIndex = 0; portField.forceActiveFocus() }

        contentItem: ColumnLayout {
            spacing: Theme.spaceMd
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textSecondary
                text: qsTr("Any computer that can reach this one will be able to connect to the port. Open only what a service on this computer needs.")
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceSm
                TextField {
                    id: portField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Port or range, e.g. 22 or 8000-8100")
                    inputMethodHints: Qt.ImhPreferNumbers
                    Accessible.name: qsTr("Port or port range")
                    onAccepted: addDialog.submit()
                }
                NxSegmented {
                    id: protocolBox
                    model: ["TCP", "UDP"]
                    Accessible.name: qsTr("Protocol")
                }
            }
            TextField {
                id: nameField
                Layout.fillWidth: true
                placeholderText: qsTr("Description (optional), e.g. SSH")
                maximumLength: 48
                Accessible.name: qsTr("Description")
                onAccepted: addDialog.submit()
            }
            Label {
                Layout.fillWidth: true
                visible: text !== ""
                wrapMode: Text.WordWrap
                color: Theme.textMuted
                font.pixelSize: Theme.fontSizeSmall
                text: portField.text.trim() !== "" && addDialog.problem !== "" ? addDialog.problem
                    : addDialog.problem === "" && firewall.isOpen(portField.text, addDialog.protocol)
                      ? qsTr("%1/%2 is already open; its description will be replaced.").arg(portField.text.trim()).arg(addDialog.protocol)
                    : ""
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spaceXs
                spacing: Theme.spaceSm
                Item { Layout.fillWidth: true }
                NxButton {
                    text: qsTr("Cancel")
                    onClicked: addDialog.close()
                }
                NxButton {
                    text: qsTr("Open port")
                    kind: "primary"
                    enabled: addDialog.problem === ""
                    onClicked: addDialog.submit()
                }
            }
        }
    }

    NxConfirmDialog {
        id: removeDialog
        title: qsTr("Close %1/%2?").arg(page.removePort).arg(page.removeProtocol)
        message: qsTr("Other computers will no longer be able to connect to this port. Connections that are already established stay open.")
        confirmText: qsTr("Close port")
        destructive: true
        onConfirmed: firewall.remove(page.removePort, page.removeProtocol)
    }

    NxConfirmDialog {
        id: resetDialog
        title: qsTr("Close all open ports?")
        message: qsTr("All %n opened port(s) are removed from the firewall. Services on this computer can then no longer be reached from the network.", "", firewall.rules.length)
        confirmText: qsTr("Close all")
        destructive: true
        onConfirmed: firewall.reset()
    }

    NxConfirmDialog {
        id: disableDialog
        title: qsTr("Turn the firewall off?")
        message: qsTr("Every service listening on this computer becomes reachable from the networks it is connected to, and the firewall will not start at the next boot either. Only do this to diagnose a connection problem, and turn it on again afterwards.")
        confirmText: qsTr("Turn off")
        destructive: true
        onConfirmed: firewall.setEnabled(false)
    }
}
