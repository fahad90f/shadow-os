// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import "Fmt.js" as Fmt

// State of the host firewall (section "firewall" = shadow-firewall status
// --json) and the changes shadow-firewall offers.
SectionPage {
    id: page

    signal openPage(string key)

    readonly property var fw: ready ? section : ({})
    readonly property var allowed: Fmt.list(fw.allowed)
    readonly property bool busy: center.firewallBusy
    readonly property var listening: center.report.listening
    readonly property int exposed: Fmt.has(listening) && listening.available !== false ? (listening.exposed_count || 0) : 0
    readonly property string portProblem: portField.text.trim() === "" ? "" : center.portError(portField.text)
    readonly property string nameProblem: center.nameError(nameField.text.trim())
    readonly property bool duplicate: {
        const wanted = portField.text.trim() + "/" + protocolBox.currentText.toLowerCase()
        for (let i = 0; i < allowed.length; i++)
            if (allowed[i].port + "/" + allowed[i].protocol === wanted) return true
        return false
    }

    function addPort() {
        if (portField.text.trim() === "" || portProblem !== "" || nameProblem !== "" || busy) return
        center.firewallAllow(portField.text.trim(), protocolBox.currentText.toLowerCase(), nameField.text.trim())
    }

    sectionKey: "firewall"
    title: qsTr("Firewall")
    sectionTitle: qsTr("The firewall state")
    subtitle: qsTr("The nftables host firewall of Shadow OS: inbound connections are dropped unless a port is opened here.")

    Connections {
        target: page.center
        function onFirewallFinished(ok, action, message) {
            if (ok && action === "allow") { portField.text = ""; nameField.text = "" }
        }
    }

    NxBanner {
        visible: page.center.firewallError !== ""
        kind: "danger"
        title: qsTr("The firewall was not changed")
        message: page.center.firewallError
        actionText: qsTr("Dismiss")
        onActionTriggered: page.center.clearFirewallError()
    }
    NxBanner {
        visible: page.ready && page.fw.installed === false
        kind: "danger"
        title: qsTr("The SHADOW ruleset is missing")
        message: qsTr("/etc/nftables.conf does not exist, so the firewall cannot be loaded. Reinstall the shadow-hardening package.")
    }
    NxBanner {
        visible: page.ready && page.fw.active === false && page.fw.installed !== false
        kind: "danger"
        title: qsTr("The firewall is not active")
        message: page.exposed > 0
            ? qsTr("Inbound connections are not filtered. %1 listening socket(s) are bound to addresses other computers can reach.").arg(page.exposed)
            : qsTr("Inbound connections to this computer are not filtered.")
        actionText: page.exposed > 0 ? qsTr("Show them") : ""
        onActionTriggered: page.openPage("network")
    }

    NxSection {
        visible: page.ready
        title: qsTr("State")

        NxRow {
            icon: page.fw.active ? "shield-check" : "shield-off"
            iconColor: page.fw.active ? Theme.success : Theme.danger
            title: page.fw.active ? qsTr("The firewall is active") : qsTr("The firewall is not active")
            subtitle: page.fw.active ? qsTr("The table “inet shadow_filter” is loaded in the kernel.")
                                     : qsTr("The table “inet shadow_filter” is not loaded in the kernel.")
            NxButton {
                visible: !page.fw.active
                text: qsTr("Enable…")
                kind: "primary"
                busy: page.busy && page.center.firewallAction === "enable"
                enabled: !page.busy && page.fw.installed !== false
                onClicked: enableDialog.open()
            }
            NxButton {
                visible: page.fw.active === true
                text: qsTr("Disable…")
                kind: "danger"
                busy: page.busy && page.center.firewallAction === "disable"
                enabled: !page.busy
                onClicked: disableDialog.open()
            }
        }
        NxRow {
            title: qsTr("Starts at boot")
            subtitle: qsTr("Whether nftables.service is enabled.")
            NxBadge {
                text: page.fw.enabled_at_boot ? qsTr("Yes") : qsTr("No")
                kind: page.fw.enabled_at_boot ? "success" : page.fw.active ? "warning" : "neutral"
            }
        }
        NxRow {
            title: qsTr("Default policy for inbound connections")
            subtitle: qsTr("What happens to a connection no rule accepts.")
            value: page.fw.active ? (page.fw.input_policy || qsTr("not reported")) : qsTr("no policy — not active")
            monoValue: page.fw.active === true
        }
        NxRow {
            title: qsTr("Default policy for forwarded traffic")
            value: page.fw.active ? (page.fw.forward_policy || qsTr("not reported")) : qsTr("no policy — not active")
            monoValue: page.fw.active === true
        }
        NxRow {
            title: qsTr("Rules loaded")
            subtitle: qsTr("Rules in the SHADOW table, including the ones for opened ports.")
            value: String(page.fw.rule_count || 0)
            NxButton {
                visible: page.fw.active === true
                text: qsTr("Reload")
                iconName: "refresh"
                busy: page.busy && page.center.firewallAction === "reload"
                enabled: !page.busy
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Validate /etc/nftables.conf and load it again")
                onClicked: page.center.firewallReload()
            }
        }
        NxRow {
            visible: Fmt.list(page.fw.other_tables).length > 0
            title: qsTr("Other nftables tables")
            subtitle: qsTr("Owned by other software (containers, virtual machines, VPN). They are not managed or changed here: %1")
                .arg(Fmt.list(page.fw.other_tables).join(", "))
            value: String(Fmt.list(page.fw.other_tables).length)
        }
    }

    NxSection {
        visible: page.ready
        title: qsTr("Opened ports")
        description: page.fw.active
            ? qsTr("Inbound connections to these ports are accepted from any address.")
            : qsTr("These ports are recorded and take effect when the firewall is enabled.")
        headerActions: [
            NxButton {
                text: qsTr("Close all…")
                kind: "danger"
                enabled: !page.busy && page.allowed.length > 0
                busy: page.busy && page.center.firewallAction === "reset"
                onClicked: resetDialog.open()
            }
        ]

        NxRow {
            visible: page.allowed.length === 0
            icon: "lock"
            title: qsTr("No port is opened")
            subtitle: qsTr("Only replies to connections this computer started, loopback traffic and essential ICMP are accepted.")
        }
        Repeater {
            model: page.allowed
            NxRow {
                id: portRow
                required property var modelData
                required property int index
                Layout.topMargin: index === 0 ? -1 : 0      // hides NxRow's divider on the first visible row
                icon: "plug"
                title: modelData.port + "/" + modelData.protocol
                subtitle: modelData.name !== undefined && modelData.name !== "" ? modelData.name : ""
                value: String(modelData.port).indexOf("-") >= 0 ? qsTr("port range, %1").arg(String(modelData.protocol).toUpperCase())
                                                               : String(modelData.protocol).toUpperCase()
                NxIconButton {
                    iconName: "trash"
                    tooltip: qsTr("Close %1/%2").arg(portRow.modelData.port).arg(portRow.modelData.protocol)
                    enabled: !page.busy
                    onClicked: page.center.firewallRemove(portRow.modelData.port, portRow.modelData.protocol)
                }
            }
        }

        // Add a port.
        ColumnLayout {
            Layout.fillWidth: true
            Layout.margins: Theme.spaceLg
            spacing: Theme.spaceSm

            NxDivider { Layout.fillWidth: true; Layout.bottomMargin: Theme.spaceXs }
            Text {
                text: qsTr("Open a port")
                color: Theme.textPrimary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
                font.weight: Theme.weightMedium
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceSm
                TextField {
                    id: portField
                    Layout.preferredWidth: 150
                    placeholderText: qsTr("Port or range")
                    font.family: Theme.fontMono
                    inputMethodHints: Qt.ImhPreferNumbers
                    maximumLength: 11
                    enabled: !page.busy
                    Accessible.name: qsTr("Port or port range")
                    onAccepted: page.addPort()
                }
                ComboBox {
                    id: protocolBox
                    Layout.preferredWidth: 96
                    model: ["TCP", "UDP"]
                    enabled: !page.busy
                    Accessible.name: qsTr("Protocol")
                }
                TextField {
                    id: nameField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Description (optional), e.g. SSH")
                    maximumLength: 48
                    enabled: !page.busy
                    Accessible.name: qsTr("Description")
                    onAccepted: page.addPort()
                }
                NxButton {
                    text: page.duplicate ? qsTr("Update") : qsTr("Open port")
                    iconName: "plus"
                    kind: "primary"
                    busy: page.busy && page.center.firewallAction === "allow"
                    enabled: !page.busy && portField.text.trim() !== "" && page.portProblem === "" && page.nameProblem === ""
                    onClicked: page.addPort()
                }
            }
            Text {
                Layout.fillWidth: true
                readonly property string problem: page.portProblem !== "" ? page.portProblem : page.nameProblem
                text: problem !== "" ? problem
                    : page.duplicate ? qsTr("This port is already open; its description will be replaced.")
                    : qsTr("A single port (22) or a range (6000-6010), 1–65535. Anyone who can reach this computer can connect to an opened port.")
                color: problem !== "" ? Theme.danger : Theme.textMuted
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WordWrap
            }
        }
    }

    NxSection {
        visible: page.ready
        title: qsTr("Active ruleset")
        description: qsTr("What the kernel is enforcing for the SHADOW table. Read-only.")
        headerActions: [
            NxButton {
                visible: page.rulesetText !== ""
                text: qsTr("Copy")
                iconName: "copy"
                onClicked: {
                    page.center.copyText(page.rulesetText)
                    ApplicationWindow.window.toaster.show(qsTr("Ruleset copied to the clipboard"), "success")
                }
            }
        ]

        NxRow {
            visible: page.rulesetText === "" && page.fw.active !== true
            icon: "shield-off"
            title: qsTr("No ruleset is loaded")
            subtitle: qsTr("The firewall is not active, so the kernel holds no SHADOW rules.")
        }
        NxRow {
            visible: page.rulesetText === "" && page.fw.active === true
            icon: "info"
            title: qsTr("The rule text is not part of this system's status report")
            subtitle: qsTr("shadow-security-status reports %1 loaded rule(s) and the policies above, but not the rules themselves. "
                           + "In a terminal: sudo nft list table inet shadow_filter").arg(page.fw.rule_count || 0)
        }
        ScrollView {
            id: rulesetView
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(420, rulesetArea.implicitHeight + Theme.spaceMd * 2)
            visible: page.rulesetText !== ""
            TextArea {
                id: rulesetArea
                text: page.rulesetText
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.NoWrap
                font.family: Theme.fontMono
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.textPrimary
                background: null
                tabStopDistance: rulesetMetrics.averageCharacterWidth * 4
                FontMetrics { id: rulesetMetrics; font: rulesetArea.font }
                Accessible.name: qsTr("Active nftables ruleset")
            }
        }
    }

    // The ruleset text, when the status tool provides it (field "ruleset").
    readonly property string rulesetText: typeof fw.ruleset === "string" ? fw.ruleset : ""

    NxConfirmDialog {
        id: enableDialog
        anchors.centerIn: Overlay.overlay
        title: qsTr("Enable the firewall?")
        message: qsTr("The SHADOW ruleset is loaded now and at every start. Inbound connections are dropped, except replies "
                      + "to connections this computer started, loopback traffic and essential ICMP. %1 "
                      + "Services that other computers use on this machine stop being reachable until their port is opened here. "
                      + "If you administer this computer remotely, open that port first.")
            .arg(page.allowed.length === 0 ? qsTr("No port is opened.")
                 : qsTr("Opened ports stay reachable: %1.").arg(page.allowed.map(r => r.port + "/" + r.protocol).join(", ")))
        confirmText: qsTr("Enable firewall")
        onConfirmed: page.center.firewallEnable()
    }
    NxConfirmDialog {
        id: disableDialog
        anchors.centerIn: Overlay.overlay
        title: qsTr("Disable the firewall?")
        message: (page.exposed > 0
                ? qsTr("Inbound connections will no longer be filtered, now and after a restart. %1 listening socket(s) on this computer are bound to addresses other computers can reach and become reachable immediately.").arg(page.exposed)
                : qsTr("Inbound connections will no longer be filtered, now and after a restart. Every service that listens on a network address becomes reachable from other computers."))
        confirmText: qsTr("Disable firewall")
        destructive: true
        onConfirmed: page.center.firewallDisable()
    }
    NxConfirmDialog {
        id: resetDialog
        anchors.centerIn: Overlay.overlay
        title: qsTr("Close all opened ports?")
        message: qsTr("All %1 opened port(s) are removed from the firewall: %2. Services on these ports stop being reachable "
                      + "from other computers. Whether the firewall is enabled does not change.")
            .arg(page.allowed.length).arg(page.allowed.map(r => r.port + "/" + r.protocol).join(", "))
        confirmText: qsTr("Close all ports")
        destructive: true
        onConfirmed: page.center.firewallReset()
    }
}
