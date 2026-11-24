// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.networkcenter

// Encrypted DNS and the hardware (MAC) address policy, through
// shadow-netprivacy (netprivacy.h; docs/security/private-dns.md).
NxPage {
    id: page
    title: qsTr("Privacy")
    subtitle: qsTr("Who answers this computer's name lookups, and which hardware address networks see.")
    maxContentWidth: 960

    readonly property var dns: NetPrivacy.dns
    readonly property var mac: NetPrivacy.mac
    readonly property bool usable: NetPrivacy.status === "ok"

    readonly property var dnsModes: ["dot", "dot-strict", "doh", "off"]
    readonly property var providerIds: ["quad9", "mullvad", "cloudflare", "custom"]
    readonly property var dnssecValues: ["allow-downgrade", "yes", "no"]
    readonly property var macModes: ["per-boot", "per-network", "hardware"]

    function dnsModeName(mode) {
        return mode === "dot" ? qsTr("Automatic DoT") : mode === "dot-strict" ? qsTr("Strict DoT")
             : mode === "doh" ? qsTr("DoH") : mode === "off" ? qsTr("Off")
             : mode === "plain" ? qsTr("Fixed servers, not encrypted") : qsTr("Unknown")
    }
    function dnsModeText(mode) {
        return mode === "dot" ? qsTr("DNS-over-TLS (port 853) to the provider whenever it answers there; plain DNS to the same provider otherwise.")
             : mode === "dot-strict" ? qsTr("DNS-over-TLS only. Where port 853 is blocked, no name can be looked up.")
             : mode === "doh" ? qsTr("DNS-over-HTTPS (port 443) through dnscrypt-proxy, which must be installed. Works where port 853 is blocked.")
             : mode === "off" ? qsTr("The DNS servers of the network you are connected to, usually without encryption.")
             : ""
    }
    function providerName(id) {
        return id === "quad9" ? qsTr("Quad9") : id === "mullvad" ? qsTr("Mullvad") : id === "cloudflare" ? qsTr("Cloudflare")
             : id === "custom" ? qsTr("Your own servers") : id === "network" ? qsTr("the network's own servers")
             : id ? id : qsTr("unknown")
    }
    function providerChoice(id) {
        return id === "quad9" ? qsTr("Quad9 — blocks known malicious domains") : id === "mullvad" ? qsTr("Mullvad — no filtering")
             : id === "cloudflare" ? qsTr("Cloudflare") : qsTr("Your own server…")
    }
    function dnssecName(value) {
        return value === "allow-downgrade" ? qsTr("Checked where answers are signed")
             : value === "yes" ? qsTr("Required: unsigned or broken answers are rejected")
             : value === "no" ? qsTr("Off") : value ? value : qsTr("Unknown")
    }
    function encryptedTitle(value) {
        return value === "always" ? qsTr("Encrypted")
             : value === "opportunistic" ? qsTr("Encrypted when possible")
             : value === "partly" ? qsTr("Partly encrypted")
             : value === "no" ? qsTr("Not encrypted")
             : value === "tor" ? qsTr("Through Tor (Hide Me)") : qsTr("Unknown")
    }
    function encryptedText(value) {
        return value === "always" ? qsTr("Every name goes to the provider over an encrypted connection.")
             : value === "opportunistic" ? qsTr("Encrypted while the provider answers on port 853; plain text where that port is blocked.")
             : value === "partly" ? qsTr("Encrypted to the provider, but some names go to the network's own server.")
             : value === "no" ? qsTr("The network, and anyone on it, can read which names are looked up.")
             : value === "tor" ? qsTr("Hide Me sends every lookup through Tor while its DNS protection is on; the setting below applies when it is off.")
             : qsTr("systemd-resolved did not report its settings.")
    }
    function encryptedKind(value) {
        return value === "always" || value === "tor" ? "success" : value === "opportunistic" || value === "partly" ? "warning"
             : value === "no" ? "danger" : "neutral"
    }
    function macModeName(mode) {
        return mode === "per-boot" ? qsTr("New address every boot") : mode === "per-network" ? qsTr("Stable per network")
             : mode === "hardware" ? qsTr("Real address") : mode === "random" ? qsTr("New random address at every connection")
             : mode === "custom" ? qsTr("Custom setting") : qsTr("Unknown")
    }
    function macModeText(mode) {
        return mode === "per-boot" ? qsTr("A private address for each network, new at every start. At most one new DHCP lease per network and start.")
             : mode === "per-network" ? qsTr("A private address for each network that stays the same across starts.")
             : mode === "hardware" ? qsTr("The device's real address on every network.")
             : mode === "random" ? qsTr("Networks then block the “new device”, and DHCP address pools run out. Choose one of the policies below.")
             : mode === "custom" ? qsTr("NetworkManager's configuration sets something other than these three policies (%1).").arg(page.macDetail())
             : ""
    }
    function macDetail() {
        let parts = [qsTr("Wi-Fi: %1").arg(mac.wifi || "?"), qsTr("wired: %1").arg(mac.ethernet || "?")]
        if (mac.stableId) parts.push(qsTr("stable-id %1").arg(mac.stableId))
        return parts.join(", ")
    }

    // ---- the DNS form: what the user is choosing, applied together ---------------------
    property string pMode: "dot"
    property string pProvider: "quad9"
    property string pDnssec: "allow-downgrade"
    property bool formLoaded: false
    readonly property string currentProviderSpec: dns.provider === "custom" ? "custom:" + (dns.servers || []).join(",") : (dns.provider || "")
    readonly property string providerSpec: pProvider === "custom" ? "custom:" + customServers.text : pProvider
    readonly property string requestError: pMode === "" ? qsTr("Choose a mode.") : NetPrivacy.dnsRequestError(pMode, providerSpec, pDnssec)
    readonly property bool dnsChanged: page.usable && (pMode !== dns.mode || pDnssec !== dns.dnssec
                                        || (pMode !== "off" && (pProvider === "custom" ? NetPrivacy.customProvider(customServers.text) !== currentProviderSpec
                                                                                         : pProvider !== dns.provider)))

    function loadForm() {
        pMode = dnsModes.indexOf(dns.mode) >= 0 ? dns.mode : ""
        pProvider = providerIds.indexOf(dns.provider) >= 0 ? dns.provider : "quad9"
        customServers.text = dns.provider === "custom" ? (dns.servers || []).join(", ") : ""
        pDnssec = dnssecValues.indexOf(dns.dnssec) >= 0 ? dns.dnssec : "allow-downgrade"
        formLoaded = true
    }

    Component.onCompleted: NetPrivacy.refresh()
    onVisibleChanged: if (visible && !NetPrivacy.refreshing && !NetPrivacy.busy) NetPrivacy.refresh()

    Connections {
        target: NetPrivacy
        function onRefreshed() {
            // A form the user has not touched follows what is in force.
            if (!page.formLoaded || !page.dnsChanged)
                page.loadForm()
        }
        function onOperationFinished(operation, target, ok, message) {
            page.ApplicationWindow.window.toaster.show(message, ok ? "success" : "danger")
            if (operation === "dns" && ok)
                page.formLoaded = false          // take the new state when it arrives
        }
    }

    actions: [
        NxIconButton {
            iconName: "refresh"
            tooltip: qsTr("Read the settings again")
            enabled: !NetPrivacy.refreshing
            onClicked: NetPrivacy.refresh()
        }
    ]

    // ---- availability and problems ----------------------------------------------------
    NxBanner {
        visible: NetPrivacy.status === "missing" || NetPrivacy.status === "failed"
        kind: NetPrivacy.status === "missing" ? "info" : "danger"
        title: NetPrivacy.status === "missing" ? qsTr("Not available") : qsTr("The settings could not be read")
        message: NetPrivacy.statusText
    }
    NxBanner {
        visible: page.usable && NetPrivacy.statusText !== ""
        kind: "warning"
        title: qsTr("Part of the status was not understood")
        message: NetPrivacy.statusText
    }
    NxBanner {
        visible: page.usable && NetPrivacy.escapeHatch
        kind: "warning"
        title: qsTr("Turned off for this boot")
        message: qsTr("The computer was started with shadow.netprivacy=off (the recovery escape hatch): the network's own DNS servers are used without encryption, and every device uses its real address. The settings below apply again at the next normal start.")
    }
    NxBanner {
        visible: page.usable && NetPrivacy.errors.length > 0
        kind: "danger"
        title: qsTr("Not working")
        message: NetPrivacy.errors.join("\n")
    }
    NxBanner {
        visible: page.usable && NetPrivacy.warnings.length > 0
        kind: "warning"
        title: NetPrivacy.warnings.length === 1 ? qsTr("Something weakens it") : qsTr("Things that weaken it")
        message: NetPrivacy.warnings.map(w => (NetPrivacy.warnings.length > 1 ? "• " : "") + w).join("\n")
    }

    // ---- encrypted DNS -----------------------------------------------------------------
    NxSection {
        title: qsTr("Encrypted DNS")
        description: qsTr("Every web address you open is first looked up by name. The provider chosen here sees every name this computer looks up; the network you are on sees none of them when the lookup is encrypted.")

        NxRow {
            icon: page.dns.encrypted === "always" || page.dns.encrypted === "tor" ? "lock" : "lock-open"
            iconColor: page.encryptedKind(page.dns.encrypted) === "success" ? Theme.success
                     : page.encryptedKind(page.dns.encrypted) === "danger" ? Theme.danger : Theme.textSecondary
            title: page.usable ? qsTr("In force: %1").arg(page.dnsModeName(page.dns.mode))
                                 + (page.dns.mode !== "off" ? qsTr(" · %1").arg(page.providerName(page.dns.provider)) : "")
                               : NetPrivacy.ready ? qsTr("Unknown") : qsTr("Reading…")
            subtitle: page.usable ? page.encryptedText(page.dns.encrypted)
                                    + (page.dns.currentServer ? " " + qsTr("Server in use: %1.").arg(page.dns.currentServer) : "")
                                  : ""
            NxBadge {
                anchors.verticalCenter: parent.verticalCenter
                visible: page.usable
                text: page.encryptedTitle(page.dns.encrypted)
                kind: page.encryptedKind(page.dns.encrypted)
            }
        }
        NxRow {
            visible: page.usable
            icon: "shield-check"
            title: qsTr("DNSSEC")
            subtitle: page.dns.live && page.dns.live.dnssec
                ? qsTr("Checks that signed answers were not forged on the way. As systemd-resolved reports it.")
                : qsTr("Checks that signed answers were not forged on the way. From the configuration files: systemd-resolved did not answer.")
            value: page.dnssecName(page.dns.live && page.dns.live.dnssec ? page.dns.live.dnssec : page.dns.dnssec)
        }

        FormRow {
            label: qsTr("Mode")
            enabled: page.usable && !NetPrivacy.busy
            ColumnLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spaceXs
                Layout.bottomMargin: Theme.spaceXs
                spacing: Theme.spaceXs
                NxSegmented {
                    model: [qsTr("Automatic DoT"), qsTr("Strict DoT"), qsTr("DoH"), qsTr("Off")]
                    currentIndex: page.dnsModes.indexOf(page.pMode)
                    Accessible.name: qsTr("DNS mode")
                    onActivated: (index) => page.pMode = page.dnsModes[index]
                }
                Label {
                    Layout.fillWidth: true
                    visible: text !== ""
                    text: page.dnsModeText(page.pMode)
                    textFormat: Text.PlainText
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSizeSmall
                    wrapMode: Text.WordWrap
                }
            }
        }
        FormRow {
            label: qsTr("Provider")
            hint: page.pMode === "off" ? qsTr("Not used: the network's own servers answer.")
                : qsTr("Sees every name this computer looks up: choose one you trust.")
            enabled: page.usable && !NetPrivacy.busy && page.pMode !== "off"
            ComboBox {
                Layout.preferredWidth: 320
                model: page.providerIds.map(id => ({ value: id, text: page.providerChoice(id) }))
                textRole: "text"
                valueRole: "value"
                currentIndex: page.providerIds.indexOf(page.pProvider)
                Accessible.name: qsTr("DNS provider")
                onActivated: page.pProvider = currentValue
            }
            Item { Layout.fillWidth: true }
        }
        FormRow {
            visible: page.pProvider === "custom" && page.pMode !== "off"
            label: qsTr("Your servers")
            hint: qsTr("Address#TLS name, separated by commas")
            error: customServers.text.trim() === "" ? "" : NetPrivacy.customServersError(customServers.text)
            enabled: page.usable && !NetPrivacy.busy
            TextField {
                id: customServers
                Layout.fillWidth: true
                font.family: Theme.fontMono
                placeholderText: "192.0.2.53#dns.example.net"
                Accessible.name: qsTr("Your DNS servers")
            }
        }
        FormRow {
            label: qsTr("DNSSEC")
            hint: page.pDnssec === "yes" ? qsTr("Required fails behind many routers and log-in pages, through Tor, and while the clock is wrong.")
                : page.pDnssec === "no" ? qsTr("Forged answers are not detected.")
                : qsTr("Signed answers are checked; unsigned ones are accepted. The default.")
            enabled: page.usable && !NetPrivacy.busy
            ComboBox {
                Layout.preferredWidth: 320
                model: page.dnssecValues.map(v => ({ value: v, text: v === "allow-downgrade" ? qsTr("Check where signed")
                                                                         : v === "yes" ? qsTr("Required") : qsTr("Off") }))
                textRole: "text"
                valueRole: "value"
                currentIndex: page.dnssecValues.indexOf(page.pDnssec)
                Accessible.name: qsTr("DNSSEC")
                onActivated: page.pDnssec = currentValue
            }
            Item { Layout.fillWidth: true }
        }
        Text {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spaceLg
            Layout.rightMargin: Theme.spaceLg
            Layout.topMargin: Theme.spaceSm
            Layout.bottomMargin: Theme.spaceMd
            text: qsTr("Strict DoT finds no names where port 853 is blocked or before you log in to a hotel or train Wi-Fi page; Automatic DoT falls back to plain DNS there, which someone on that network can force.")
            color: Theme.textMuted
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
            wrapMode: Text.WordWrap
        }
    }

    RowLayout {
        Layout.fillWidth: true
        visible: page.usable
        spacing: Theme.spaceSm
        Label {
            Layout.fillWidth: true
            text: !page.dnsChanged ? qsTr("This is the setting in force.")
                : page.requestError !== "" ? page.requestError
                : qsTr("Not applied yet. Applying restarts the resolver and checks that it took the new setting; an administrator password is needed.")
            color: page.dnsChanged && page.requestError !== "" ? Theme.danger : Theme.textMuted
            wrapMode: Text.WordWrap
        }
        NxButton {
            text: qsTr("Revert")
            enabled: page.dnsChanged && !NetPrivacy.busy
            onClicked: page.loadForm()
        }
        NxButton {
            text: qsTr("Apply")
            kind: "primary"
            busy: NetPrivacy.busyOperation === "dns"
            enabled: page.dnsChanged && page.requestError === "" && !NetPrivacy.busy
            onClicked: NetPrivacy.setDns(page.pMode, page.providerSpec, page.pDnssec)
        }
    }

    // ---- hardware address ----------------------------------------------------------------
    NxSection {
        title: qsTr("Hardware address")
        description: qsTr("The address (MAC) your Wi-Fi or Ethernet adapter shows to the network. A private address keeps networks from recognising this computer from place to place. Nothing is reconnected when you change it: each network uses the new address the next time it connects.")

        NxRow {
            icon: "fingerprint"
            iconColor: page.mac.mode === "per-boot" || page.mac.mode === "per-network" ? Theme.success
                     : page.mac.mode === "random" ? Theme.danger : Theme.textSecondary
            title: page.usable ? qsTr("In force: %1").arg(page.macModeName(page.mac.mode))
                               : NetPrivacy.ready ? qsTr("Unknown") : qsTr("Reading…")
            subtitle: page.usable ? (page.mac.override === "escape" ? qsTr("Real addresses for this boot (shadow.netprivacy=off).") : page.macDetail()) : ""
        }
        NxRow {
            icon: "wifi"
            visible: page.usable
            title: qsTr("While searching for Wi-Fi networks")
            value: page.mac.scanRandomization ? qsTr("Random address") : qsTr("Real address")
        }
        NxRow {
            icon: "sliders"
            title: qsTr("Policy")
            subtitle: page.macModeText(page.mac.mode)
            enabled: page.usable && !NetPrivacy.busy
            NxSegmented {
                anchors.verticalCenter: parent.verticalCenter
                model: [qsTr("New every boot"), qsTr("Stable per network"), qsTr("Real address")]
                currentIndex: page.macModes.indexOf(page.mac.mode)
                Accessible.name: qsTr("Hardware address policy")
                onActivated: (index) => NetPrivacy.setMac(page.macModes[index])
            }
        }
        NxRow {
            visible: page.usable && (page.mac.fixedDrivers || []).length > 0
            icon: "info"
            title: qsTr("Adapters that keep their real address")
            subtitle: qsTr("Their drivers cannot change it (%1).").arg((page.mac.fixedDrivers || []).join(", "))
        }
    }

    // ---- per network --------------------------------------------------------------------
    readonly property var addressProfiles: Nm.connections.filter(c => c.type === "802-11-wireless" || c.type === "802-3-ethernet")
    readonly property var exceptions: page.mac.exceptions || []
    // Exceptions whose profile NetworkManager does not list (it is not running, or the profile is gone).
    readonly property var unlistedExceptions: page.exceptions.filter(u => !page.addressProfiles.some(c => c.uuid.toLowerCase() === u))

    NxSection {
        visible: page.usable
        title: qsTr("Keep a network's address")
        description: page.mac.mode === "hardware"
            ? qsTr("Not used while every network sees the real address.")
            : qsTr("For a network that should recognise this computer — a Wi-Fi log-in page, or an allowlist of devices: it keeps one private address across starts.")
              + (page.mac.mode === "per-network" ? " " + qsTr("With “Stable per network” every network does that already.") : "")

        Repeater {
            model: page.addressProfiles
            NxRow {
                id: profileRow
                required property var modelData
                readonly property bool kept: page.exceptions.indexOf(modelData.uuid.toLowerCase()) >= 0
                icon: modelData.type === "802-11-wireless" ? "wifi" : "ethernet"
                title: modelData.name
                subtitle: (modelData.type === "802-11-wireless" ? qsTr("Wi-Fi") : qsTr("Wired"))
                          + (modelData.active ? qsTr(" · connected") : "")
                          + (profileRow.kept ? qsTr(" · keeps its address") : "")
                enabled: page.mac.exceptionsKnown === true && !NetPrivacy.busy && page.mac.mode !== "hardware"
                Switch {
                    anchors.verticalCenter: parent.verticalCenter
                    checked: profileRow.kept
                    Accessible.name: qsTr("Keep the address of %1").arg(profileRow.modelData.name)
                    onToggled: {
                        const wanted = checked
                        checked = Qt.binding(() => profileRow.kept)
                        NetPrivacy.setMacException(profileRow.modelData.uuid, wanted)
                    }
                }
            }
        }
        Repeater {
            model: page.unlistedExceptions
            NxRow {
                required property string modelData
                icon: "network"
                title: qsTr("Connection %1").arg(modelData)
                subtitle: Nm.running ? qsTr("Keeps its address; NetworkManager does not list this connection.")
                                     : qsTr("Keeps its address.")
                monoValue: true
            }
        }
        NxRow {
            visible: page.addressProfiles.length === 0
            icon: "info"
            title: Nm.running ? qsTr("No saved Wi-Fi or wired networks") : qsTr("The networks cannot be listed")
            subtitle: Nm.running ? "" : Nm.statusText
        }
        NxRow {
            visible: page.mac.exceptionsKnown === false
            icon: "alert-triangle"
            iconColor: Theme.warning
            title: qsTr("Which networks keep their address is unknown")
            subtitle: qsTr("shadow-netprivacy could not ask NetworkManager.")
        }
    }
}
