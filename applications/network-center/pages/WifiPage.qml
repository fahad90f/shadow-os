// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.networkcenter

NxPage {
    id: page
    title: qsTr("Wi-Fi")
    subtitle: qsTr("Wireless radio, networks in range, the current link and saved networks.")
    maxContentWidth: 1000

    readonly property bool available: Nm.running && Nm.wifiDevices.length > 0
    readonly property var current: {
        const list = Nm.wifiNetworks
        for (let i = 0; i < list.length; i++)
            if (list[i].inUse) return list[i]
        return null
    }
    readonly property var kernelWireless: NetState.wirelessInterfaces
    readonly property var radioSwitches: NetState.rfkill.filter(s => s.type === "wlan")

    // What the kernel itself knows about wireless hardware, for the states in
    // which NetworkManager cannot tell.
    function kernelFacts() {
        let facts = []
        if (kernelWireless.length > 0)
            facts.push(qsTr("The kernel has %1 wireless interface(s): %2.").arg(kernelWireless.length).arg(kernelWireless.map(i => i.name).join(", ")))
        else
            facts.push(qsTr("The kernel reports no wireless interface either, so no Wi-Fi adapter (or no driver for it) is present."))
        for (let i = 0; i < radioSwitches.length; i++) {
            const s = radioSwitches[i]
            if (s.hardBlocked)
                facts.push(qsTr("Radio “%1” is switched off by a hardware switch.").arg(s.name))
            else if (s.softBlocked)
                facts.push(qsTr("Radio “%1” is switched off in software (airplane mode).").arg(s.name))
        }
        return facts.join(" ")
    }

    function securityNote(network) {
        if (network.enterprise)
            return qsTr("Enterprise (802.1X) networks need an identity and certificates; they cannot be set up here.")
        if (network.wep)
            return qsTr("WEP is broken and not supported.")
        return ""
    }

    function connectTo(network) {
        if (network.inUse) {
            Nm.disconnectDevice(network.device)
        } else if (network.savedUuid !== "") {
            Nm.activateConnection(network.savedUuid)
        } else if (!network.secured || network.owe) {
            Nm.connectWifi(network.ssid, network.device, "", false)
        } else {
            passwordDialog.ssid = network.ssid
            passwordDialog.device = network.device
            passwordDialog.hidden = false
            passwordDialog.title = qsTr("Connect to “%1”").arg(network.ssid)
            passwordDialog.message = qsTr("Security: %1. Enter the network password; it is handed to NetworkManager, which stores it with the connection.").arg(network.security)
            passwordDialog.open()
        }
    }

    actions: [
        NxButton {
            visible: page.available
            text: qsTr("Hidden network…")
            enabled: Nm.wifiEnabled
            onClicked: hiddenDialog.open()
        },
        NxButton {
            visible: page.available
            text: qsTr("Scan")
            iconName: "refresh"
            busy: Nm.scanning
            enabled: Nm.wifiEnabled
            onClicked: Nm.refreshWifi(true)
        }
    ]

    Timer {
        interval: 10000
        running: page.visible && page.available && Nm.wifiEnabled
        repeat: true
        onTriggered: Nm.refreshWifi(false)
    }

    RecordModel {
        id: networks
        roles: ["inUse", "bssid", "ssid", "hidden", "mode", "channel", "frequency", "rate", "strength", "security",
                "secured", "enterprise", "wep", "owe", "device", "savedUuid"]
        records: Nm.wifiNetworks
    }

    SecretDialog {
        id: passwordDialog
        property string ssid: ""
        property string device: ""
        property bool hidden: false
        placeholder: qsTr("Wi-Fi password")
        validator: (text) => text === "" ? " " : Nm.wifiPasswordError(text)
        onSubmitted: (secret) => Nm.connectWifi(ssid, device, secret, hidden)
    }

    Dialog {
        id: hiddenDialog
        title: qsTr("Connect to a hidden network")
        width: Math.min(460, parent ? parent.width - Theme.spaceXxl : 460)
        closePolicy: Popup.CloseOnEscape
        footer: null
        readonly property string passwordProblem: hiddenPassword.text === "" ? "" : Nm.wifiPasswordError(hiddenPassword.text)
        onOpened: { hiddenSsid.text = ""; hiddenPassword.text = ""; hiddenSsid.forceActiveFocus() }
        onClosed: hiddenPassword.text = ""
        contentItem: ColumnLayout {
            spacing: Theme.spaceMd
            Label {
                Layout.fillWidth: true
                text: qsTr("A hidden network does not announce its name. Enter the name exactly; leave the password empty for an open network.")
                color: Theme.textSecondary
                wrapMode: Text.WordWrap
            }
            TextField {
                id: hiddenSsid
                Layout.fillWidth: true
                placeholderText: qsTr("Network name (SSID)")
                Accessible.name: placeholderText
                maximumLength: 32
            }
            NxPasswordField {
                id: hiddenPassword
                Layout.fillWidth: true
                placeholderText: qsTr("Password")
                Accessible.name: placeholderText
            }
            Label {
                Layout.fillWidth: true
                visible: hiddenDialog.passwordProblem !== ""
                text: hiddenDialog.passwordProblem
                color: Theme.danger
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WordWrap
            }
            ComboBox {
                id: hiddenDevice
                Layout.fillWidth: true
                visible: Nm.wifiDevices.length > 1
                model: Nm.wifiDevices.map(d => d.name)
                Accessible.name: qsTr("Wi-Fi adapter")
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceSm
                Item { Layout.fillWidth: true }
                NxButton { text: qsTr("Cancel"); onClicked: hiddenDialog.reject() }
                NxButton {
                    text: qsTr("Connect")
                    kind: "primary"
                    enabled: hiddenSsid.text.trim() !== "" && hiddenDialog.passwordProblem === "" && Nm.wifiDevices.length > 0
                    onClicked: {
                        const device = Nm.wifiDevices.length > 1 ? hiddenDevice.currentText : Nm.wifiDevices[0].name
                        const ssid = hiddenSsid.text
                        const secret = hiddenPassword.text
                        hiddenDialog.accept()
                        Nm.connectWifi(ssid, device, secret, true)
                    }
                }
            }
        }
    }

    NxConfirmDialog {
        id: forgetDialog
        property string uuid: ""
        property string name: ""
        title: qsTr("Forget “%1”?").arg(name)
        message: qsTr("The saved network and its password are removed from NetworkManager. To use the network again you will have to enter the password again.")
        confirmText: qsTr("Forget network")
        destructive: true
        onConfirmed: Nm.deleteConnection(uuid)
    }

    // ---- NetworkManager cannot be used -------------------------------------------
    NmUnavailable {
        Layout.fillWidth: true
        Layout.topMargin: Theme.spaceXl
        visible: !Nm.running
        feature: Nm.status === "unknown" ? "" : qsTr("Wi-Fi networks are scanned, joined and saved through NetworkManager.") + " " + page.kernelFacts()
    }

    // ---- NetworkManager runs, but there is no adapter -------------------------------
    NxEmptyState {
        Layout.fillWidth: true
        Layout.topMargin: Theme.spaceXl
        visible: Nm.running && Nm.wifiDevices.length === 0
        icon: "wifi-off"
        title: qsTr("No Wi-Fi adapter")
        message: (Nm.wifiHardware === "missing" ? qsTr("NetworkManager found no Wi-Fi hardware.")
                                                : qsTr("NetworkManager manages no Wi-Fi device (radio hardware: %1).").arg(Nm.wifiHardware))
                 + " " + page.kernelFacts()
    }

    // ---- normal operation ---------------------------------------------------------------
    NxSection {
        visible: page.available
        title: qsTr("Radio")

        NxRow {
            icon: Nm.wifiEnabled ? "wifi" : "wifi-off"
            title: qsTr("Wi-Fi")
            subtitle: Nm.wifiHardware === "disabled" ? qsTr("Switched off by a hardware switch or the firmware; it cannot be enabled from software.")
                : Nm.wifiEnabled ? qsTr("The radio is on.") : qsTr("The radio is off; no networks are scanned or joined.")
            Switch {
                checked: Nm.wifiEnabled
                enabled: Nm.wifiHardware !== "disabled" && Nm.busyTargets.indexOf("wifi-radio") < 0
                Accessible.name: qsTr("Wi-Fi radio")
                onToggled: Nm.setWifiEnabled(checked)
            }
        }
        Repeater {
            model: Nm.wifiDevices
            NxRow {
                id: deviceRow
                required property var modelData
                icon: "signal"
                title: modelData.name
                subtitle: modelData.connection !== "" ? qsTr("%1 — “%2”").arg(modelData.stateText).arg(modelData.connection) : modelData.stateText
                value: modelData.mac
                monoValue: true
                NxButton {
                    visible: deviceRow.modelData.stateCode >= 40 && deviceRow.modelData.stateCode <= 100
                    text: qsTr("Disconnect")
                    busy: Nm.busyTargets.indexOf(deviceRow.modelData.name) >= 0
                    onClicked: Nm.disconnectDevice(deviceRow.modelData.name)
                }
            }
        }
    }

    NxSection {
        visible: page.available && page.current !== null
        title: qsTr("Current link")

        NxRow {
            icon: "wifi"
            title: page.current ? (page.current.ssid || qsTr("(hidden network)")) : ""
            subtitle: page.current ? qsTr("Access point %1").arg(page.current.bssid) : ""
            NxBadge {
                kind: !page.current ? "neutral" : page.current.strength >= 60 ? "success" : page.current.strength >= 35 ? "warning" : "danger"
                text: page.current ? qsTr("Signal %1 %").arg(page.current.strength) : ""
            }
        }
        NxRow {
            icon: "zap"
            title: qsTr("Link rate")
            subtitle: page.current ? qsTr("The access point supports up to %1").arg(page.current.rate) : ""
            value: {
                if (!page.current) return ""
                const device = Nm.devices.find(d => d.name === page.current.device)
                return device && device.speed ? device.speed : qsTr("Not reported")
            }
        }
        NxRow {
            icon: "activity"
            title: qsTr("Band and channel")
            value: page.current ? [NetUtil.wifiBand(page.current.frequency), qsTr("channel %1").arg(page.current.channel),
                                   qsTr("%1 MHz").arg(page.current.frequency)].filter(s => s !== "").join(" · ") : ""
        }
        NxRow {
            icon: page.current && page.current.secured ? "lock" : "lock-open"
            iconColor: page.current && !page.current.secured ? Theme.warning : Theme.textSecondary
            title: qsTr("Security")
            value: page.current ? (page.current.security || qsTr("None — traffic is not encrypted")) : ""
        }
    }

    NxSection {
        visible: page.available
        title: qsTr("Networks in range")
        description: Nm.wifiEnabled ? qsTr("One line per access point, strongest first. Last scan results are refreshed every 10 seconds.")
                                    : qsTr("Switch the radio on to see networks.")

        Repeater {
            model: networks
            NxRow {
                id: networkRow
                required property bool inUse
                required property string bssid
                required property string ssid
                required property int channel
                required property int frequency
                required property string rate
                required property int strength
                required property string security
                required property bool secured
                required property bool enterprise
                required property bool wep
                required property bool owe
                required property string device
                required property string savedUuid

                readonly property string note: savedUuid === "" && !inUse ? page.securityNote(networkRow) : ""
                readonly property bool working: Nm.busyTargets.indexOf(ssid) >= 0 || Nm.busyTargets.indexOf(device) >= 0
                    || (savedUuid !== "" && Nm.busyTargets.indexOf(savedUuid) >= 0)

                icon: "wifi"
                iconColor: inUse ? Theme.accent : Theme.textSecondary
                title: ssid !== "" ? ssid : qsTr("(hidden network)")
                subtitle: [NetUtil.wifiBand(frequency), qsTr("channel %1").arg(channel), bssid, rate, note].filter(s => s !== "").join(" · ")

                NxBadge {
                    kind: networkRow.strength >= 60 ? "success" : networkRow.strength >= 35 ? "warning" : "danger"
                    text: networkRow.strength + " %"
                }
                NxBadge {
                    kind: networkRow.secured ? "neutral" : "warning"
                    dot: false
                    text: networkRow.secured ? networkRow.security : qsTr("Open")
                }
                NxBadge {
                    visible: networkRow.savedUuid !== ""
                    kind: "accent"
                    dot: false
                    text: qsTr("Saved")
                }
                NxButton {
                    text: networkRow.inUse ? qsTr("Disconnect") : qsTr("Connect")
                    kind: networkRow.inUse ? "secondary" : "primary"
                    busy: networkRow.working
                    // Hidden access points are joined by name through “Hidden network…”.
                    enabled: networkRow.inUse || (networkRow.ssid !== "" && networkRow.note === "")
                    onClicked: page.connectTo(networkRow)
                }
            }
        }
        NxRow {
            visible: networks.count === 0
            icon: "info"
            title: !Nm.wifiEnabled ? qsTr("The Wi-Fi radio is off")
                : Nm.scanning ? qsTr("Scanning…") : qsTr("No networks found")
            subtitle: Nm.wifiEnabled && !Nm.scanning ? qsTr("Press Scan to search again.") : ""
        }
    }

    NxSection {
        visible: Nm.running && (page.available || Nm.savedWifi.length > 0)
        title: qsTr("Saved networks")
        description: qsTr("Networks NetworkManager remembers. Forgetting one deletes its profile and stored password.")

        Repeater {
            model: Nm.savedWifi
            NxRow {
                id: savedRow
                required property var modelData
                icon: "wifi"
                title: modelData.ssid !== "" ? modelData.ssid : modelData.name
                subtitle: [modelData.ssid !== modelData.name ? qsTr("profile “%1”").arg(modelData.name) : "",
                           modelData.keyMgmt !== "" ? modelData.keyMgmt : qsTr("open"),
                           modelData.autoconnect ? qsTr("joins automatically") : qsTr("joins only on request"),
                           modelData.active ? qsTr("in use") : ""].filter(s => s !== "").join(" · ")
                NxIconButton {
                    iconName: "trash"
                    tooltip: qsTr("Forget %1").arg(savedRow.title)
                    enabled: !savedRow.modelData.readOnly
                    onClicked: {
                        forgetDialog.uuid = savedRow.modelData.uuid
                        forgetDialog.name = savedRow.title
                        forgetDialog.open()
                    }
                }
            }
        }
        NxRow {
            visible: Nm.savedWifi.length === 0
            icon: "info"
            title: qsTr("No saved networks")
        }
    }
}
