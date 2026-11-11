// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.controlcenter

NxPage {
    id: page
    title: qsTr("Wi-Fi")
    subtitle: qsTr("Wireless networks in range, saved networks and the current connection.")

    // Bumped when NetworkManager refuses a change, so switches fall back to the real state.
    property int revision: 0
    // The network the password dialog is asking about.
    property string pendingKey: ""
    property string pendingSsid: ""
    property string pendingKind: ""
    property string forgetUuid: ""
    property string forgetName: ""

    readonly property var macPolicies: [
        { value: "", text: qsTr("NetworkManager's default") },
        { value: "permanent", text: qsTr("Hardware address") },
        { value: "stable", text: qsTr("Random, the same for this network") },
        { value: "random", text: qsTr("Random on every connection") },
        { value: "preserve", text: qsTr("Keep the address the adapter has") },
    ]
    readonly property var hiddenSecurity: [
        { value: "psk", text: qsTr("WPA / WPA2 Personal") },
        { value: "sae", text: qsTr("WPA3 Personal") },
        { value: "open", text: qsTr("None (open network)") },
    ]

    function indexOfValue(items, value) {
        for (let i = 0; i < items.length; i++) {
            if (items[i].value === value) return i
        }
        return -1
    }

    function join(key, ssid, kind, saved) {
        if (saved || kind === "open" || kind === "owe") {
            wifi.connectNetwork(key, "")
            return
        }
        pendingKey = key
        pendingSsid = ssid
        pendingKind = kind
        passwordField.text = ""
        passwordField.revealed = false
        passwordDialog.open()
    }

    WifiBackend {
        id: wifi
        onFailed: (message) => {
            page.revision++
            page.ApplicationWindow.window.toaster.show(message, "danger")
        }
        onConnected: (ssid) => page.ApplicationWindow.window.toaster.show(qsTr("Connected to %1").arg(ssid), "success")
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

    // Four bars, 'level' of them lit.
    component SignalBars: Row {
        id: bars
        property int level: 0
        property color litColor: Theme.textPrimary
        spacing: 2
        Repeater {
            model: 4
            Item {
                id: bar
                required property int index
                width: 4
                height: 16
                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: 4 + bar.index * 4
                    radius: 1
                    color: bar.index < bars.level ? bars.litColor : Theme.bgPressed
                }
            }
        }
    }

    NxCard {
        visible: wifi.ready && !wifi.available
        Layout.fillWidth: true
        implicitHeight: 320
        NxEmptyState {
            anchors.fill: parent
            icon: "wifi-off"
            tone: "warning"
            title: wifi.state === "nm-missing" ? qsTr("NetworkManager is not installed")
                 : wifi.state === "nm-not-running" ? qsTr("NetworkManager is not running")
                 : wifi.state === "rfkill-hard" ? qsTr("Wi-Fi is switched off by hardware")
                 : wifi.state === "unmanaged" ? qsTr("The Wi-Fi adapter is not managed")
                 : qsTr("No Wi-Fi adapter")
            message: wifi.statusMessage
            actionText: qsTr("Check again")
            onActionTriggered: wifi.refresh()
        }
    }

    NxSection {
        visible: wifi.available

        NxRow {
            icon: wifi.radioEnabled ? "wifi" : "wifi-off"
            iconColor: wifi.radioEnabled ? Theme.accent : Theme.textSecondary
            title: qsTr("Wi-Fi")
            subtitle: !wifi.radioEnabled ? wifi.statusMessage
                    : wifi.deviceCount > 1 ? qsTr("Adapter %1 (one of %2)").arg(wifi.device).arg(wifi.deviceCount)
                    : qsTr("Adapter %1").arg(wifi.device)
            Switch {
                anchors.verticalCenter: parent.verticalCenter
                checked: (page.revision, wifi.radioEnabled)
                Accessible.name: qsTr("Wi-Fi")
                onToggled: wifi.setRadioEnabled(checked)
            }
        }
    }

    // ---- the current connection --------------------------------------------------
    NxSection {
        visible: wifi.radioEnabled && wifi.details.stateCode !== undefined && wifi.details.stateCode >= 40 && wifi.details.stateCode <= 110
        title: qsTr("Current connection")

        NxRow {
            icon: "wifi"
            iconColor: wifi.details.connected ? Theme.success : Theme.warning
            title: wifi.details.ssid !== "" && wifi.details.ssid !== undefined ? wifi.details.ssid : (wifi.details.profile || "")
            subtitle: {
                const parts = [wifi.details.stateText || ""]
                if (wifi.details.connected && wifi.details.ssid) {
                    parts.push(qsTr("Signal %1 %").arg(wifi.details.strength))
                    if (wifi.details.security) parts.push(wifi.details.security)
                    if (wifi.details.band) parts.push(wifi.details.band)
                    if (wifi.details.channel) parts.push(qsTr("channel %1").arg(wifi.details.channel))
                }
                return parts.join("  ·  ")
            }
            NxButton {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Disconnect")
                onClicked: wifi.disconnectDevice()
            }
        }
        Item {
            Layout.fillWidth: true
            implicitHeight: facts.implicitHeight + Theme.spaceMd * 2
            Rectangle {
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: Theme.spaceLg
                height: 1
                color: Theme.borderSubtle
            }
            ColumnLayout {
                id: facts
                anchors.fill: parent
                anchors.leftMargin: Theme.spaceLg
                anchors.rightMargin: Theme.spaceLg
                anchors.topMargin: Theme.spaceMd
                anchors.bottomMargin: Theme.spaceMd
                spacing: Theme.spaceXs + 2

                NxKeyValue {
                    label: qsTr("IPv4 address")
                    value: (wifi.details.ipv4 || []).join(", ")
                    mono: true
                }
                NxKeyValue {
                    label: qsTr("Gateway")
                    value: [wifi.details.gateway4 || "", wifi.details.gateway6 || ""].filter(s => s !== "").join(", ")
                    mono: true
                }
                NxKeyValue {
                    label: qsTr("DNS servers")
                    value: (wifi.details.dns || []).join(", ")
                    mono: true
                }
                NxKeyValue {
                    visible: (wifi.details.ipv6 || []).length > 0
                    label: qsTr("IPv6 address")
                    value: (wifi.details.ipv6 || []).join(", ")
                    mono: true
                }
                NxKeyValue {
                    label: qsTr("Link speed")
                    value: wifi.details.speed ? (wifi.details.maxRate ? qsTr("%1 (the access point offers up to %2)").arg(wifi.details.speed).arg(wifi.details.maxRate)
                                                                      : wifi.details.speed)
                                              : ""
                }
                NxKeyValue {
                    label: qsTr("Access point")
                    value: wifi.details.bssid || ""
                    mono: true
                }
                NxKeyValue {
                    label: qsTr("MAC address in use")
                    value: wifi.details.mac || ""
                    mono: true
                }
            }
        }
        NxRow {
            visible: wifi.details.uuid !== undefined && wifi.details.uuid !== ""
            icon: "fingerprint"
            title: qsTr("MAC address for this network")
            subtitle: qsTr("A random address makes this computer harder to track. Applies from the next connection.")
            ComboBox {
                width: 290
                model: page.macPolicies
                textRole: "text"
                valueRole: "value"
                currentIndex: (page.revision, page.indexOfValue(page.macPolicies, wifi.details.macPolicy || ""))
                // A fixed address set with other tools is shown as it is.
                displayText: currentIndex < 0 ? wifi.details.macPolicy : currentText
                Accessible.name: qsTr("MAC address for this network")
                onActivated: wifi.setMacPolicy(wifi.details.uuid, currentValue)
            }
        }
    }

    // ---- networks in range ------------------------------------------------------------
    NxSection {
        visible: wifi.radioEnabled
        title: qsTr("Networks")
        headerActions: [
            NxButton {
                text: qsTr("Hidden network…")
                kind: "ghost"
                onClicked: {
                    hiddenSsid.text = ""
                    hiddenPassword.text = ""
                    hiddenPassword.revealed = false
                    hiddenDialog.open()
                }
            },
            NxButton {
                text: qsTr("Scan")
                iconName: "refresh"
                busy: wifi.scanning
                onClicked: wifi.scan()
            }
        ]

        Repeater {
            model: wifi.networks
            NxRow {
                id: network
                required property string key
                required property string ssid
                required property int strength
                required property int bars
                required property string security
                required property string securityKind
                required property string band
                required property bool inUse
                required property bool saved
                required property bool connectable
                required property string note

                readonly property bool attempting: wifi.connecting && wifi.connectingSsid === ssid

                icon: securityKind === "open" ? "wifi" : "lock"
                iconColor: inUse ? Theme.accent : Theme.textSecondary
                title: ssid
                subtitle: {
                    if (!connectable) return note
                    const parts = [securityKind === "open" ? qsTr("Open — not encrypted") : security]
                    if (band !== "") parts.push(band)
                    if (inUse) parts.push(qsTr("Connected"))
                    else if (saved) parts.push(qsTr("Saved"))
                    return parts.join("  ·  ")
                }

                SignalBars {
                    anchors.verticalCenter: parent.verticalCenter
                    level: network.bars
                    litColor: network.inUse ? Theme.accent : Theme.textPrimary
                    Accessible.name: qsTr("Signal %1 %").arg(network.strength)
                }
                NxButton {
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.max(implicitWidth, 104)
                    text: network.inUse ? qsTr("Disconnect") : qsTr("Connect")
                    kind: network.inUse || !network.saved ? "secondary" : "primary"
                    enabled: network.connectable && (!wifi.connecting || network.attempting)
                    busy: network.attempting
                    Accessible.name: network.inUse ? qsTr("Disconnect from %1").arg(network.ssid) : qsTr("Connect to %1").arg(network.ssid)
                    onClicked: network.inUse ? wifi.disconnectDevice()
                                             : page.join(network.key, network.ssid, network.securityKind, network.saved)
                }
            }
        }
        EmptyRow {
            visible: wifi.networkCount === 0
            text: wifi.scanning ? qsTr("Scanning for networks…") : qsTr("No network found. Scan again, or move closer to the access point.")
        }
    }

    // ---- saved networks -----------------------------------------------------------------
    NxSection {
        visible: wifi.available && wifi.savedCount > 0
        title: qsTr("Saved networks")
        description: qsTr("Networks this computer has joined. Their passwords are kept by NetworkManager.")

        Repeater {
            model: wifi.saved
            NxRow {
                id: known
                required property string uuid
                required property string name
                required property string ssid
                required property bool autoconnect
                required property double lastUsed
                required property bool active

                icon: "wifi"
                iconColor: active ? Theme.accent : Theme.textSecondary
                title: name
                subtitle: {
                    const parts = []
                    if (ssid !== name) parts.push(ssid)
                    parts.push(active ? qsTr("Connected")
                             : lastUsed > 0 ? qsTr("Last used %1").arg(new Date(lastUsed * 1000).toLocaleDateString(Qt.locale(), Locale.ShortFormat))
                             : qsTr("Never used"))
                    return parts.join("  ·  ")
                }
                CheckBox {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Connect automatically")
                    checked: (page.revision, known.autoconnect)
                    onToggled: wifi.setAutoconnect(known.uuid, checked)
                }
                NxIconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    iconName: "trash"
                    tooltip: qsTr("Forget %1").arg(known.name)
                    onClicked: {
                        page.forgetUuid = known.uuid
                        page.forgetName = known.name
                        forgetDialog.open()
                    }
                }
            }
        }
    }

    NxConfirmDialog {
        id: forgetDialog
        title: qsTr("Forget %1?").arg(page.forgetName)
        message: qsTr("The saved password and settings of this network are deleted. To join it again you have to enter the password.")
        confirmText: qsTr("Forget")
        destructive: true
        onConfirmed: wifi.forget(page.forgetUuid)
    }

    Dialog {
        id: passwordDialog
        title: qsTr("Password for %1").arg(page.pendingSsid)
        width: Math.min(440, page.width - Theme.spaceXxl)
        footer: null
        readonly property string problem: wifi.passwordProblem(page.pendingKind, passwordField.text)
        function submit() {
            if (problem !== "") return
            wifi.connectNetwork(page.pendingKey, passwordField.text)
            passwordField.text = ""
            close()
        }
        onOpened: passwordField.forceActiveFocus()
        onClosed: passwordField.text = ""

        contentItem: ColumnLayout {
            spacing: Theme.spaceMd
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textSecondary
                text: page.pendingKind === "sae" ? qsTr("This network is protected with WPA3.") : qsTr("This network is protected with WPA.")
            }
            NxPasswordField {
                id: passwordField
                Layout.fillWidth: true
                placeholderText: qsTr("Password")
                Accessible.name: qsTr("Password for %1").arg(page.pendingSsid)
                onAccepted: passwordDialog.submit()
            }
            Label {
                Layout.fillWidth: true
                visible: passwordField.text.length > 0 && passwordDialog.problem !== ""
                wrapMode: Text.WordWrap
                color: Theme.textMuted
                font.pixelSize: Theme.fontSizeSmall
                text: passwordDialog.problem
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spaceXs
                spacing: Theme.spaceSm
                Item { Layout.fillWidth: true }
                NxButton {
                    text: qsTr("Cancel")
                    onClicked: passwordDialog.close()
                }
                NxButton {
                    text: qsTr("Connect")
                    kind: "primary"
                    enabled: passwordDialog.problem === ""
                    onClicked: passwordDialog.submit()
                }
            }
        }
    }

    Dialog {
        id: hiddenDialog
        title: qsTr("Connect to a hidden network")
        width: Math.min(460, page.width - Theme.spaceXxl)
        footer: null
        readonly property string kind: hiddenSecurityBox.currentIndex >= 0 ? hiddenSecurityBox.currentValue : "psk"
        readonly property string problem: hiddenSsid.text.trim() === "" ? qsTr("Enter the name of the network.")
                                        : wifi.passwordProblem(kind, hiddenPassword.text)
        function submit() {
            if (problem !== "") return
            wifi.connectHidden(hiddenSsid.text.trim(), kind, hiddenPassword.text)
            hiddenPassword.text = ""
            close()
        }
        onOpened: hiddenSsid.forceActiveFocus()
        onClosed: hiddenPassword.text = ""

        contentItem: ColumnLayout {
            spacing: Theme.spaceMd
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textSecondary
                text: qsTr("A hidden network does not announce its name. Enter it exactly as it was set up.")
            }
            TextField {
                id: hiddenSsid
                Layout.fillWidth: true
                placeholderText: qsTr("Network name (SSID)")
                maximumLength: 32
                Accessible.name: placeholderText
            }
            ComboBox {
                id: hiddenSecurityBox
                Layout.fillWidth: true
                model: page.hiddenSecurity
                textRole: "text"
                valueRole: "value"
                Accessible.name: qsTr("Security")
            }
            NxPasswordField {
                id: hiddenPassword
                Layout.fillWidth: true
                visible: hiddenDialog.kind !== "open"
                placeholderText: qsTr("Password")
                Accessible.name: qsTr("Password")
                onAccepted: hiddenDialog.submit()
            }
            Label {
                Layout.fillWidth: true
                visible: (hiddenSsid.text.length > 0 || hiddenPassword.text.length > 0) && hiddenDialog.problem !== ""
                wrapMode: Text.WordWrap
                color: Theme.textMuted
                font.pixelSize: Theme.fontSizeSmall
                text: hiddenDialog.problem
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spaceXs
                spacing: Theme.spaceSm
                Item { Layout.fillWidth: true }
                NxButton {
                    text: qsTr("Cancel")
                    onClicked: hiddenDialog.close()
                }
                NxButton {
                    text: qsTr("Connect")
                    kind: "primary"
                    enabled: hiddenDialog.problem === ""
                    onClicked: hiddenDialog.submit()
                }
            }
        }
    }
}
