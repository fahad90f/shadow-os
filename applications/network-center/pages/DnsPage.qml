// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.networkcenter

NxPage {
    id: page
    title: qsTr("DNS")
    subtitle: qsTr("Which resolvers this computer asks, where that configuration comes from, and the DNS servers of each connection.")
    maxContentWidth: 960

    readonly property var dns: NetState.dns
    readonly property bool resolved: dns.source === "systemd-resolved"
    readonly property var resolvedInfo: dns.resolved || ({})
    readonly property var resolvConf: dns.resolvConf || ({})
    readonly property var links: resolvedInfo.links || []

    function dotText(mode) {
        return mode === "yes" ? qsTr("Required — queries are only sent over TLS")
            : mode === "opportunistic" ? qsTr("Opportunistic — TLS when the server supports it, plain text otherwise")
            : mode === "no" ? qsTr("Off — queries are sent in plain text")
            : mode ? mode : qsTr("Unknown")
    }

    // ---- editing the DNS settings of a connection --------------------------------
    readonly property var profiles: Nm.connections.filter(c => c.hasDetails && c.type !== "loopback")
    property string uuid: ""
    readonly property var connection: Nm.connections.find(c => c.uuid === uuid) || ({})
    readonly property bool v4Static: connection.ipv4 !== undefined && ["disabled", "link-local"].indexOf(connection.ipv4.method) < 0
    readonly property bool v6Static: connection.ipv6 !== undefined && ["disabled", "ignore", "link-local"].indexOf(connection.ipv6.method) < 0
    property var config: ({})
    readonly property var plan: connection.uuid !== undefined ? Nm.planIpChange(connection.uuid, config)
                                                              : ({ ok: false, errors: [], warnings: [], changes: [] })
    property bool reloadAfterRefresh: false
    readonly property var dotModes: [
        { value: "default", text: qsTr("System default") },
        { value: "no", text: qsTr("Off") },
        { value: "opportunistic", text: qsTr("Opportunistic") },
        { value: "yes", text: qsTr("Required") },
    ]

    function select(newUuid) {
        uuid = newUuid
        load()
    }
    function load() {
        const c = connection
        dns4.text = c.ipv4 ? c.ipv4.dns.join(", ") : ""
        dns6.text = c.ipv6 ? c.ipv6.dns.join(", ") : ""
        search.text = c.ipv4 ? c.ipv4.search.join(", ") : ""
        onlyThese.checked = (c.ipv4 ? c.ipv4.ignoreAutoDns : false) && (c.ipv6 ? c.ipv6.ignoreAutoDns : false)
        dot.currentIndex = Math.max(0, dotModes.findIndex(m => m.value === (c.dnsOverTls || "default")))
        collect()
    }
    function collect() {
        let wanted = { dnsOverTls: dotModes[Math.max(0, dot.currentIndex)].value }
        // Families that cannot carry DNS settings are left untouched.
        if (v4Static)
            wanted.ipv4 = { dns: dns4.text, search: search.text, ignoreAutoDns: onlyThese.checked }
        if (v6Static)
            wanted.ipv6 = { dns: dns6.text, ignoreAutoDns: onlyThese.checked }
        config = wanted
    }
    function ensureSelection() {
        if (uuid !== "" && profiles.some(c => c.uuid === uuid))
            return
        const preferred = profiles.find(c => c.active) || profiles[0]
        select(preferred ? preferred.uuid : "")
    }
    onProfilesChanged: ensureSelection()
    Component.onCompleted: ensureSelection()

    Connections {
        target: Nm
        function onOperationFinished(operation, target, ok, message) {
            if (operation === "ip-config" && target === page.uuid && ok)
                page.reloadAfterRefresh = true
        }
        function onRefreshed() {
            if (page.reloadAfterRefresh) {
                page.reloadAfterRefresh = false
                page.load()
                NetState.refresh()
            }
        }
    }

    ChangeReviewDialog {
        id: review
        plan: page.plan
        onApply: (reactivate) => Nm.applyIpChange(page.uuid, page.config, reactivate)
    }

    actions: [
        NxIconButton {
            iconName: "refresh"
            tooltip: qsTr("Refresh")
            onClicked: { NetState.refresh(); Nm.refresh() }
        }
    ]

    // ---- how names are resolved ---------------------------------------------------
    NxSection {
        title: qsTr("Resolver")

        NxRow {
            icon: "server"
            title: qsTr("Name resolution is done by")
            subtitle: page.resolved
                ? qsTr("Applications query the local systemd-resolved service, which forwards to the servers below. Source of this page: its D-Bus interface.")
                : qsTr("systemd-resolved is not running. Applications read /etc/resolv.conf and query the servers listed there directly. Source of this page: that file.")
            value: page.resolved ? qsTr("systemd-resolved") : qsTr("C library (resolv.conf)")
        }
        NxRow {
            icon: "file-text"
            title: qsTr("/etc/resolv.conf")
            subtitle: page.resolvConf.linkTarget ? qsTr("Symbolic link to %1").arg(page.resolvConf.linkTarget) : ""
            value: page.resolvConf.exists === false ? qsTr("Missing") : qsTr("Written by: %1").arg(page.resolvConf.managedBy || "")
        }
        NxRow {
            icon: page.resolved && page.resolvedInfo.dnsOverTls === "yes" ? "lock" : "lock-open"
            iconColor: page.resolved && page.resolvedInfo.dnsOverTls === "yes" ? Theme.success : Theme.textSecondary
            title: qsTr("DNS over TLS")
            subtitle: page.resolved ? qsTr("Global setting of systemd-resolved; a link can override it (see below).")
                                    : qsTr("Not available: the C library resolver cannot encrypt queries. DNS over TLS needs systemd-resolved (package systemd-resolved).")
            value: page.resolved ? page.dotText(page.resolvedInfo.dnsOverTls) : qsTr("Not available")
        }
        NxRow {
            visible: page.resolved
            icon: "shield-check"
            title: qsTr("DNSSEC validation")
            value: page.resolvedInfo.dnssec === "yes" ? qsTr("On") : page.resolvedInfo.dnssec === "allow-downgrade" ? qsTr("On, with downgrade")
                : page.resolvedInfo.dnssec === "no" ? qsTr("Off") : (page.resolvedInfo.dnssec || qsTr("Unknown"))
        }
    }

    // ---- effective servers: systemd-resolved, per link -----------------------------
    NxSection {
        visible: page.resolved
        title: qsTr("Resolvers per link")
        description: page.resolvedInfo.usingFallback
            ? qsTr("No link provides a DNS server; systemd-resolved is using its built-in fallback servers: %1.").arg((page.resolvedInfo.fallbackServers || []).join(", "))
            : qsTr("Servers and domains systemd-resolved uses for each link. “Global” entries apply to all links.")

        Repeater {
            model: page.links
            NxRow {
                required property var modelData
                icon: modelData.global ? "globe" : "ethernet"
                title: modelData.global ? qsTr("Global") : modelData.name
                subtitle: [modelData.searchDomains.length > 0 ? qsTr("search: %1").arg(modelData.searchDomains.join(", ")) : "",
                           modelData.routingDomains.length > 0 ? qsTr("routes queries for: %1").arg(modelData.routingDomains.join(", ")) : "",
                           modelData.currentServer ? qsTr("currently using %1").arg(modelData.currentServer) : "",
                           modelData.dnsOverTls ? qsTr("DNS over TLS: %1").arg(modelData.dnsOverTls) : ""].filter(s => s !== "").join(" · ")
                value: modelData.servers.length > 0 ? modelData.servers.join("   ") : qsTr("No servers")
                monoValue: modelData.servers.length > 0
            }
        }
        NxRow {
            visible: page.links.length === 0
            icon: "info"
            title: qsTr("systemd-resolved has no DNS servers configured")
        }
    }

    // ---- effective servers: resolv.conf -----------------------------------------------
    NxSection {
        title: page.resolved ? qsTr("What applications read: /etc/resolv.conf") : qsTr("Servers in use (/etc/resolv.conf)")
        description: page.resolved ? qsTr("With systemd-resolved this normally points at its local stub, 127.0.0.53.") : ""

        Repeater {
            model: page.resolvConf.nameservers || []
            NxRow {
                required property string modelData
                required property int index
                icon: "server"
                title: qsTr("Name server %1").arg(index + 1)
                value: modelData
                monoValue: true
            }
        }
        NxRow {
            visible: (page.resolvConf.nameservers || []).length === 0
            icon: "alert-triangle"
            iconColor: Theme.warning
            title: qsTr("No name server is configured")
            subtitle: qsTr("Host names cannot be resolved until a DNS server is set.")
        }
        NxRow {
            icon: "search"
            title: qsTr("Search domains")
            value: (page.resolvConf.search || []).length > 0 ? page.resolvConf.search.join("   ") : qsTr("None")
            monoValue: (page.resolvConf.search || []).length > 0
        }
        NxRow {
            visible: (page.resolvConf.options || []).length > 0
            icon: "settings"
            title: qsTr("Options")
            value: (page.resolvConf.options || []).join("   ")
            monoValue: true
        }
    }

    // ---- NetworkManager's view ---------------------------------------------------------
    NxSection {
        visible: Nm.running
        title: qsTr("DNS servers by device (NetworkManager)")
        description: qsTr("What NetworkManager received or was configured with for each active device, before it is handed to the resolver.")

        Repeater {
            model: Nm.devices.filter(d => d.stateCode === 100 && d.type !== "loopback")
            NxRow {
                required property var modelData
                icon: modelData.type === "wifi" ? "wifi" : "ethernet"
                title: modelData.name
                subtitle: [modelData.connection !== "" ? qsTr("connection “%1”").arg(modelData.connection) : "",
                           modelData.domains4.concat(modelData.domains6).length > 0
                               ? qsTr("domains: %1").arg(modelData.domains4.concat(modelData.domains6).join(", ")) : ""].filter(s => s !== "").join(" · ")
                value: modelData.dns4.concat(modelData.dns6).length > 0 ? modelData.dns4.concat(modelData.dns6).join("   ") : qsTr("No servers")
                monoValue: modelData.dns4.concat(modelData.dns6).length > 0
            }
        }
        NxRow {
            visible: Nm.devices.filter(d => d.stateCode === 100 && d.type !== "loopback").length === 0
            icon: "info"
            title: qsTr("No device is connected through NetworkManager")
        }
    }

    // ---- change ------------------------------------------------------------------------------
    NxBanner {
        visible: !Nm.running && Nm.status !== "unknown"
        kind: "info"
        title: qsTr("DNS servers cannot be changed here")
        message: qsTr("%1 DNS servers are set per NetworkManager connection; without it, edit /etc/resolv.conf or the service that writes it (%2).")
                     .arg(Nm.statusText).arg(page.resolvConf.managedBy || "")
    }

    NxSection {
        visible: Nm.running && page.profiles.length > 0
        title: qsTr("Change the DNS servers of a connection")
        description: qsTr("Stored in the connection profile; the same review-and-apply mechanism as on the IP configuration page.")

        FormRow {
            label: qsTr("Connection")
            ComboBox {
                Layout.fillWidth: true
                model: page.profiles.map(c => ({ text: c.name + "  —  " + c.typeLabel + (c.device !== "" ? qsTr(" on %1").arg(c.device) : qsTr(", not active")), uuid: c.uuid }))
                textRole: "text"
                valueRole: "uuid"
                currentIndex: page.profiles.findIndex(c => c.uuid === page.uuid)
                Accessible.name: qsTr("Connection profile")
                onActivated: page.select(currentValue)
            }
        }
        FormRow {
            label: qsTr("IPv4 DNS servers")
            hint: page.v4Static ? qsTr("Addresses, separated by commas") : qsTr("IPv4 is disabled or link-local for this connection")
            enabled: page.v4Static
            TextField {
                id: dns4
                Layout.fillWidth: true
                font.family: Theme.fontMono
                placeholderText: qsTr("None — only automatic servers")
                Accessible.name: qsTr("IPv4 DNS servers")
                onTextEdited: page.collect()
            }
        }
        FormRow {
            label: qsTr("IPv6 DNS servers")
            hint: page.v6Static ? qsTr("Addresses, separated by commas") : qsTr("IPv6 is disabled or link-local for this connection")
            enabled: page.v6Static
            TextField {
                id: dns6
                Layout.fillWidth: true
                font.family: Theme.fontMono
                placeholderText: qsTr("None — only automatic servers")
                Accessible.name: qsTr("IPv6 DNS servers")
                onTextEdited: page.collect()
            }
        }
        FormRow {
            label: qsTr("Search domains")
            hint: qsTr("Appended to short host names")
            enabled: page.v4Static
            TextField {
                id: search
                Layout.fillWidth: true
                font.family: Theme.fontMono
                placeholderText: qsTr("None")
                Accessible.name: qsTr("Search domains")
                onTextEdited: page.collect()
            }
        }
        FormRow {
            label: qsTr("Use only these servers")
            hint: qsTr("Ignore the DNS servers announced by DHCP or the router")
            Switch {
                id: onlyThese
                Accessible.name: qsTr("Use only these servers")
                onToggled: page.collect()
            }
            Item { Layout.fillWidth: true }
        }
        FormRow {
            label: qsTr("DNS over TLS")
            hint: page.resolved ? qsTr("Applied to this connection's link by systemd-resolved")
                                : qsTr("Stored in the profile, but only takes effect when systemd-resolved does the resolving")
            ComboBox {
                id: dot
                Layout.preferredWidth: 240
                model: page.dotModes
                textRole: "text"
                valueRole: "value"
                Accessible.name: qsTr("DNS over TLS")
                onActivated: page.collect()
            }
            Item { Layout.fillWidth: true }
        }
    }

    NxBanner {
        visible: Nm.running && page.profiles.length > 0 && (page.plan.errors || []).length > 0
        kind: "danger"
        title: qsTr("These settings cannot be applied")
        message: (page.plan.errors || []).join("\n")
    }

    RowLayout {
        Layout.fillWidth: true
        visible: Nm.running && page.profiles.length > 0
        spacing: Theme.spaceSm
        Label {
            Layout.fillWidth: true
            text: page.plan.ok !== true ? ""
                : (page.plan.changes || []).length === 0 ? qsTr("No changes.")
                : qsTr("%1 setting(s) will change.").arg(page.plan.changes.length)
            color: Theme.textMuted
        }
        NxButton {
            text: qsTr("Revert")
            enabled: (page.plan.changes || []).length > 0 || page.plan.ok !== true
            onClicked: page.load()
        }
        NxButton {
            text: qsTr("Review changes…")
            kind: "primary"
            busy: Nm.busyTargets.indexOf(page.uuid) >= 0
            enabled: page.plan.ok === true && (page.plan.changes || []).length > 0 && page.connection.readOnly !== true
            onClicked: review.open()
        }
    }
}
