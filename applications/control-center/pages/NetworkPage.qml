// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.controlcenter

NxPage {
    id: page
    title: qsTr("Wired & Proxy")
    subtitle: qsTr("Wired network interfaces, their IP configuration, and the proxy used by applications.")

    // Bumped when NetworkManager refuses a change, so switches fall back to the real state.
    property int revision: 0
    property string proxyNote: ""
    readonly property real hostWidth: Math.max(140, Math.min(260, width * 0.28))

    readonly property var methods4: [
        { value: "auto", text: qsTr("Automatic (DHCP)") },
        { value: "manual", text: qsTr("Manual") },
    ]
    readonly property var methods6: [
        { value: "auto", text: qsTr("Automatic") },
        { value: "manual", text: qsTr("Manual") },
        { value: "disabled", text: qsTr("Disabled") },
    ]
    readonly property var proxyModes: ["none", "manual", "auto"]

    function indexOfValue(items, value) {
        for (let i = 0; i < items.length; i++) {
            if (items[i].value === value) return i
        }
        return -1
    }

    function methodText(methods, value) {
        const index = indexOfValue(methods, value)
        return index >= 0 ? methods[index].text : value
    }

    // ---- proxy draft ---------------------------------------------------------------
    property string proxyMode: "none"
    readonly property var proxyDraft: ({
        mode: proxyMode,
        httpHost: httpHost.text.trim(), httpPort: httpPort.text.trim(),
        httpsHost: httpsHost.text.trim(), httpsPort: httpsPort.text.trim(),
        socksHost: socksHost.text.trim(), socksPort: socksPort.text.trim(),
        noProxy: noProxy.text.trim(), pacUrl: pacUrl.text.trim()
    })
    readonly property string proxyProblem: net.proxyProblem(proxyDraft)
    readonly property bool proxyDirty: {
        const saved = net.proxy
        if (proxyDraft.mode !== saved.mode) return true
        if (proxyDraft.mode === "manual")
            return ["httpHost", "httpPort", "httpsHost", "httpsPort", "socksHost", "socksPort", "noProxy"]
                .some(key => proxyDraft[key] !== (saved[key] || ""))
        if (proxyDraft.mode === "auto") return proxyDraft.pacUrl !== (saved.pacUrl || "")
        return false
    }

    function loadProxy() {
        const saved = net.proxy
        proxyMode = saved.mode || "none"
        httpHost.text = saved.httpHost || ""
        httpPort.text = saved.httpPort || ""
        httpsHost.text = saved.httpsHost || ""
        httpsPort.text = saved.httpsPort || ""
        socksHost.text = saved.socksHost || ""
        socksPort.text = saved.socksPort || ""
        noProxy.text = saved.noProxy || ""
        pacUrl.text = saved.pacUrl || ""
    }

    Component.onCompleted: loadProxy()

    NetworkBackend {
        id: net
        onFailed: (message) => {
            page.revision++
            page.ApplicationWindow.window.toaster.show(message, "danger")
        }
        onIpApplied: (device) => page.ApplicationWindow.window.toaster.show(qsTr("The IP settings of %1 were applied").arg(device), "success")
        onProxyApplied: (note) => page.proxyNote = note
        onProxyChanged: page.loadProxy()
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

    // Host and port of one proxy server.
    component PortField: TextField {
        width: 84
        placeholderText: qsTr("Port")
        horizontalAlignment: TextInput.AlignHCenter
        font.family: Theme.fontMono
        inputMethodHints: Qt.ImhDigitsOnly
        validator: IntValidator { bottom: 1; top: 65535 }
    }

    NxBanner {
        visible: net.ready && !net.available
        kind: "warning"
        title: net.state === "nm-missing" ? qsTr("NetworkManager is not installed") : qsTr("NetworkManager is not running")
        message: net.statusMessage
        actionText: qsTr("Check again")
        onActionTriggered: net.refresh()
    }

    // ---- wired interfaces ---------------------------------------------------------------
    NxSection {
        visible: net.available && net.deviceCount === 0
        title: qsTr("Wired connections")
        EmptyRow { text: qsTr("NetworkManager reports no wired network interface.") }
    }

    Repeater {
        model: net.devices

        NxSection {
            id: wired
            required property string name
            required property int stateCode
            required property string stateText
            required property bool connected
            required property bool managed
            required property bool carrier
            required property string speed
            required property string mac
            required property string profile
            required property string uuid
            required property string ipv4
            required property string gateway4
            required property string ipv6
            required property string gateway6
            required property string dns
            required property string method4
            required property string addresses4
            required property string profileGateway4
            required property string dns4
            required property string method6
            required property string addresses6
            required property string profileGateway6
            required property string dns6

            readonly property bool up: stateCode >= 40 && stateCode <= 100

            title: qsTr("Wired interface %1").arg(name)
            description: [mac, speed, profile !== "" ? qsTr("profile “%1”").arg(profile) : ""].filter(s => s !== "").join("  ·  ")

            NxRow {
                icon: "ethernet"
                iconColor: wired.connected ? Theme.success : Theme.textSecondary
                title: wired.stateText
                subtitle: !wired.managed ? qsTr("Another tool controls this interface.")
                        : !wired.carrier && !wired.up ? qsTr("Plug in a network cable to connect.") : ""
                Switch {
                    anchors.verticalCenter: parent.verticalCenter
                    checked: (page.revision, wired.up)
                    enabled: wired.managed && (wired.up || wired.stateCode === 30) && !net.busy
                    Accessible.name: qsTr("Connect %1").arg(wired.name)
                    onToggled: net.setDeviceEnabled(wired.name, checked)
                }
            }
            Item {
                visible: wired.connected
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
                    NxKeyValue { label: qsTr("IPv4 address"); value: wired.ipv4; mono: true }
                    NxKeyValue { label: qsTr("Gateway"); value: [wired.gateway4, wired.gateway6].filter(s => s !== "").join(", "); mono: true }
                    NxKeyValue { label: qsTr("DNS servers"); value: wired.dns; mono: true }
                    NxKeyValue { visible: wired.ipv6 !== ""; label: qsTr("IPv6 address"); value: wired.ipv6; mono: true }
                }
            }
            NxRow {
                icon: "sliders"
                title: qsTr("IP configuration")
                subtitle: wired.uuid === "" ? qsTr("There is no connection profile for this interface yet. Connect it once and NetworkManager creates one.")
                        : qsTr("IPv4: %1  ·  IPv6: %2").arg(page.methodText(page.methods4, wired.method4)).arg(page.methodText(page.methods6, wired.method6))
                NxButton {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Edit…")
                    enabled: wired.uuid !== "" && wired.managed && !net.busy
                    onClicked: ipDialog.edit(wired.name, wired.method4, wired.addresses4, wired.profileGateway4, wired.dns4,
                                             wired.method6, wired.addresses6, wired.profileGateway6, wired.dns6, wired.up)
                }
            }
        }
    }

    // ---- proxy ----------------------------------------------------------------------------
    NxSection {
        title: qsTr("Proxy")
        description: qsTr("Stored in %1 for the whole session; an automatic configuration is also given to NetworkManager. "
                          + "Applications that are already running pick the change up only after you log out and back in.").arg(net.proxyFile)

        NxRow {
            icon: "globe"
            title: qsTr("Proxy")
            subtitle: page.proxyMode === "none" ? qsTr("Applications connect to the internet directly.")
                    : page.proxyMode === "manual" ? qsTr("Applications use the proxy servers below.")
                    : qsTr("Applications fetch a configuration script that picks the proxy.")
            NxSegmented {
                anchors.verticalCenter: parent.verticalCenter
                model: [qsTr("None"), qsTr("Manual"), qsTr("Automatic")]
                currentIndex: page.proxyModes.indexOf(page.proxyMode)
                onActivated: (index) => {
                    page.proxyMode = page.proxyModes[index]
                    // NxSegmented assigns currentIndex itself; keep it following proxyMode (Discard).
                    currentIndex = Qt.binding(() => page.proxyModes.indexOf(page.proxyMode))
                }
            }
        }
        NxRow {
            visible: page.proxyMode === "manual"
            icon: "server"
            title: qsTr("HTTP proxy")
            TextField { id: httpHost; width: page.hostWidth; placeholderText: qsTr("Host name or address"); Accessible.name: qsTr("HTTP proxy host") }
            PortField { id: httpPort; Accessible.name: qsTr("HTTP proxy port") }
        }
        NxRow {
            visible: page.proxyMode === "manual"
            icon: "lock"
            title: qsTr("HTTPS proxy")
            TextField { id: httpsHost; width: page.hostWidth; placeholderText: qsTr("Host name or address"); Accessible.name: qsTr("HTTPS proxy host") }
            PortField { id: httpsPort; Accessible.name: qsTr("HTTPS proxy port") }
        }
        NxRow {
            visible: page.proxyMode === "manual"
            icon: "shield"
            title: qsTr("SOCKS proxy")
            TextField { id: socksHost; width: page.hostWidth; placeholderText: qsTr("Host name or address"); Accessible.name: qsTr("SOCKS proxy host") }
            PortField { id: socksPort; Accessible.name: qsTr("SOCKS proxy port") }
        }
        NxRow {
            visible: page.proxyMode === "manual"
            icon: "filter"
            title: qsTr("No proxy for")
            subtitle: qsTr("Hosts, domains and networks to reach directly, separated by commas")
            TextField {
                id: noProxy
                width: page.hostWidth + 92
                placeholderText: "localhost, 127.0.0.1, ::1"
                Accessible.name: qsTr("No proxy for")
            }
        }
        NxRow {
            visible: page.proxyMode === "auto"
            icon: "file-text"
            title: qsTr("Configuration script")
            subtitle: qsTr("Address of the proxy auto-configuration (PAC) file")
            TextField {
                id: pacUrl
                width: page.hostWidth + 92
                placeholderText: "http://wpad.example.org/proxy.pac"
                inputMethodHints: Qt.ImhUrlCharactersOnly
                Accessible.name: qsTr("Address of the configuration script")
            }
        }
        NxRow {
            icon: page.proxyProblem !== "" ? "alert-triangle" : page.proxyDirty ? "edit" : "check-circle"
            iconColor: page.proxyProblem !== "" ? Theme.warning : page.proxyDirty ? Theme.textSecondary : Theme.success
            title: page.proxyProblem !== "" ? qsTr("Not complete yet")
                 : page.proxyDirty ? qsTr("Changes are not saved yet") : qsTr("Saved")
            subtitle: page.proxyProblem !== "" ? page.proxyProblem
                    : net.proxyFileProblem !== "" ? net.proxyFileProblem
                    : net.proxyProfiles.length > 0 ? qsTr("NetworkManager uses an automatic proxy configuration for: %1").arg(net.proxyProfiles.join(", "))
                    : ""
            NxButton {
                anchors.verticalCenter: parent.verticalCenter
                visible: page.proxyDirty
                text: qsTr("Discard")
                kind: "ghost"
                onClicked: page.loadProxy()
            }
            NxButton {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Apply")
                kind: "primary"
                enabled: page.proxyDirty && page.proxyProblem === ""
                busy: net.busy
                onClicked: net.applyProxy(page.proxyDraft)
            }
        }
    }

    NxBanner {
        visible: page.proxyNote !== ""
        kind: "info"
        title: qsTr("Log out to finish")
        message: page.proxyNote
        dismissible: true
        onDismissed: { page.proxyNote = ""; visible = Qt.binding(() => page.proxyNote !== "") }
    }

    // ---- IP settings of one interface ---------------------------------------------------------
    Dialog {
        id: ipDialog
        property string device: ""
        property bool live: false
        readonly property var settings: ({
            method4: method4Box.currentIndex >= 0 ? method4Box.currentValue : "",
            addresses4: addresses4Field.text, gateway4: gateway4Field.text, dns4: dns4Field.text,
            method6: method6Box.currentIndex >= 0 ? method6Box.currentValue : "",
            addresses6: addresses6Field.text, gateway6: gateway6Field.text, dns6: dns6Field.text
        })
        readonly property string problem: net.ipProblem(settings)
        readonly property bool manual4: settings.method4 === "manual"
        readonly property bool manual6: settings.method6 === "manual"

        function edit(name, method4, addresses4, gateway4, dns4, method6, addresses6, gateway6, dns6, up) {
            device = name
            live = up
            method4Box.currentIndex = page.indexOfValue(page.methods4, method4)
            addresses4Field.text = addresses4
            gateway4Field.text = gateway4
            dns4Field.text = dns4
            method6Box.currentIndex = page.indexOfValue(page.methods6, method6 === "ignore" ? "disabled" : method6)
            addresses6Field.text = addresses6
            gateway6Field.text = gateway6
            dns6Field.text = dns6
            open()
        }

        title: qsTr("IP configuration of %1").arg(device)
        width: Math.min(520, page.width - Theme.spaceXxl)
        footer: null

        contentItem: GridLayout {
            columns: 2
            columnSpacing: Theme.spaceMd
            rowSpacing: Theme.spaceSm

            Label { Layout.columnSpan: 2; text: qsTr("IPv4"); font.weight: Theme.weightSemibold }
            Label { text: qsTr("Method"); color: Theme.textSecondary }
            ComboBox {
                id: method4Box
                Layout.fillWidth: true
                model: page.methods4
                textRole: "text"
                valueRole: "value"
                displayText: currentIndex < 0 ? qsTr("Choose a method") : currentText
                Accessible.name: qsTr("IPv4 method")
            }
            Label { text: qsTr("Address"); color: Theme.textSecondary }
            TextField {
                id: addresses4Field
                Layout.fillWidth: true
                enabled: ipDialog.manual4
                font.family: Theme.fontMono
                placeholderText: ipDialog.manual4 ? qsTr("Address/prefix, e.g. 192.168.1.20/24") : qsTr("Assigned by the network")
                Accessible.name: qsTr("IPv4 address with prefix length")
            }
            Label { text: qsTr("Gateway"); color: Theme.textSecondary }
            TextField {
                id: gateway4Field
                Layout.fillWidth: true
                enabled: ipDialog.manual4
                font.family: Theme.fontMono
                placeholderText: ipDialog.manual4 ? qsTr("e.g. 192.168.1.1") : qsTr("Assigned by the network")
                Accessible.name: qsTr("IPv4 gateway")
            }
            Label { text: qsTr("DNS servers"); color: Theme.textSecondary }
            TextField {
                id: dns4Field
                Layout.fillWidth: true
                font.family: Theme.fontMono
                placeholderText: ipDialog.manual4 ? qsTr("e.g. 9.9.9.9, 149.112.112.112")
                                                  : qsTr("Automatic — enter addresses to override")
                Accessible.name: qsTr("IPv4 DNS servers")
            }

            Label { Layout.columnSpan: 2; Layout.topMargin: Theme.spaceMd; text: qsTr("IPv6"); font.weight: Theme.weightSemibold }
            Label { text: qsTr("Method"); color: Theme.textSecondary }
            ComboBox {
                id: method6Box
                Layout.fillWidth: true
                model: page.methods6
                textRole: "text"
                valueRole: "value"
                displayText: currentIndex < 0 ? qsTr("Choose a method") : currentText
                Accessible.name: qsTr("IPv6 method")
            }
            Label { text: qsTr("Address"); color: Theme.textSecondary }
            TextField {
                id: addresses6Field
                Layout.fillWidth: true
                enabled: ipDialog.manual6
                font.family: Theme.fontMono
                placeholderText: ipDialog.manual6 ? qsTr("Address/prefix, e.g. 2001:db8::20/64") : qsTr("Assigned by the network")
                Accessible.name: qsTr("IPv6 address with prefix length")
            }
            Label { text: qsTr("Gateway"); color: Theme.textSecondary }
            TextField {
                id: gateway6Field
                Layout.fillWidth: true
                enabled: ipDialog.manual6
                font.family: Theme.fontMono
                placeholderText: ipDialog.manual6 ? qsTr("e.g. 2001:db8::1") : qsTr("Assigned by the network")
                Accessible.name: qsTr("IPv6 gateway")
            }
            Label { text: qsTr("DNS servers"); color: Theme.textSecondary }
            TextField {
                id: dns6Field
                Layout.fillWidth: true
                enabled: ipDialog.settings.method6 !== "disabled"
                font.family: Theme.fontMono
                placeholderText: ipDialog.manual6 ? qsTr("e.g. 2620:fe::fe")
                                                  : qsTr("Automatic — enter addresses to override")
                Accessible.name: qsTr("IPv6 DNS servers")
            }

            Label {
                Layout.columnSpan: 2
                Layout.fillWidth: true
                Layout.topMargin: Theme.spaceXs
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontSizeSmall
                color: ipDialog.problem !== "" ? Theme.warning : Theme.textMuted
                text: ipDialog.problem !== "" ? ipDialog.problem
                    : ipDialog.live ? qsTr("The interface is reconnected to apply the settings; connections through it are interrupted briefly.")
                    : qsTr("The settings are used the next time this interface connects.")
            }
            RowLayout {
                Layout.columnSpan: 2
                Layout.fillWidth: true
                Layout.topMargin: Theme.spaceXs
                spacing: Theme.spaceSm
                Item { Layout.fillWidth: true }
                NxButton {
                    text: qsTr("Cancel")
                    onClicked: ipDialog.close()
                }
                NxButton {
                    text: qsTr("Apply")
                    kind: "primary"
                    enabled: ipDialog.problem === ""
                    onClicked: {
                        net.applyIp(ipDialog.device, ipDialog.settings)
                        ipDialog.close()
                    }
                }
            }
        }
    }
}
