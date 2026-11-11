// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui

// Form for the IPv4 or IPv6 part of a NetworkManager connection profile.
// load() fills it from a profile; value() returns what it says now, in the
// shape Nm.planIpChange() expects. Validation happens there, not here.
NxSection {
    id: root

    property int family: 4
    readonly property string method: methodBox.currentValue === undefined ? "" : methodBox.currentValue
    // NetworkManager accepts no addresses, gateway or DNS with these methods.
    readonly property bool staticAllowed: method !== "disabled" && method !== "ignore" && method !== "link-local"

    signal edited()

    property var methods: []

    function methodOptions(current) {
        let options = family === 4
            ? [{ value: "auto", text: qsTr("Automatic (DHCP)") },
               { value: "manual", text: qsTr("Manual") },
               { value: "link-local", text: qsTr("Link-local only (169.254.x.x)") },
               { value: "disabled", text: qsTr("Disabled") }]
            : [{ value: "auto", text: qsTr("Automatic (router advertisements, DHCPv6)") },
               { value: "dhcp", text: qsTr("DHCPv6 only") },
               { value: "manual", text: qsTr("Manual") },
               { value: "link-local", text: qsTr("Link-local only (fe80::)") },
               { value: "disabled", text: qsTr("Disabled") }]
        // A method this form does not offer (shared, ignore) stays selectable
        // if the profile already uses it, so loading never changes it.
        if (current !== "" && !options.some(o => o.value === current))
            options.push({ value: current, text: current === "shared" ? qsTr("Shared to other computers")
                                                   : current === "ignore" ? qsTr("Ignored (left to the kernel)") : current })
        return options
    }

    function load(ip) {
        const current = ip && ip.method ? ip.method : ""
        methods = methodOptions(current)
        methodBox.currentIndex = Math.max(0, methods.findIndex(o => o.value === current))
        addresses.text = ip && ip.addresses ? ip.addresses.join(", ") : ""
        gateway.text = ip && ip.gateway ? ip.gateway : ""
        dns.text = ip && ip.dns ? ip.dns.join(", ") : ""
        search.text = ip && ip.search ? ip.search.join(", ") : ""
        ignoreAuto.checked = ip ? ip.ignoreAutoDns === true : false
    }

    function value() {
        return {
            method: root.method,
            addresses: addresses.text,
            gateway: gateway.text,
            dns: dns.text,
            search: search.text,
            ignoreAutoDns: ignoreAuto.checked,
        }
    }

    title: family === 4 ? qsTr("IPv4") : qsTr("IPv6")

    FormRow {
        label: qsTr("Method")
        hint: root.staticAllowed ? "" : qsTr("Addresses, gateway and DNS are not used with this method and will be cleared.")
        ComboBox {
            id: methodBox
            Layout.preferredWidth: 340
            model: root.methods
            textRole: "text"
            valueRole: "value"
            Accessible.name: qsTr("%1 method").arg(root.title)
            onActivated: root.edited()
        }
        Item { Layout.fillWidth: true }
    }
    FormRow {
        label: qsTr("Addresses")
        hint: root.family === 4 ? qsTr("address/prefix, e.g. 192.168.1.10/24; separate several with commas")
                                : qsTr("address/prefix, e.g. 2001:db8::10/64; separate several with commas")
        enabled: root.staticAllowed
        TextField {
            id: addresses
            Layout.fillWidth: true
            font.family: Theme.fontMono
            placeholderText: root.method === "manual" ? qsTr("Required") : qsTr("None (optional)")
            Accessible.name: qsTr("%1 addresses").arg(root.title)
            onTextEdited: root.edited()
        }
    }
    FormRow {
        label: qsTr("Gateway")
        hint: qsTr("Next hop of the default route; needs a static address")
        enabled: root.staticAllowed
        TextField {
            id: gateway
            Layout.fillWidth: true
            font.family: Theme.fontMono
            placeholderText: qsTr("None")
            Accessible.name: qsTr("%1 gateway").arg(root.title)
            onTextEdited: root.edited()
        }
    }
    FormRow {
        label: qsTr("DNS servers")
        hint: qsTr("Addresses, separated by commas")
        enabled: root.staticAllowed
        TextField {
            id: dns
            Layout.fillWidth: true
            font.family: Theme.fontMono
            placeholderText: qsTr("None")
            Accessible.name: qsTr("%1 DNS servers").arg(root.title)
            onTextEdited: root.edited()
        }
    }
    FormRow {
        label: qsTr("Search domains")
        hint: qsTr("Appended to short host names; “~domain” only routes queries")
        enabled: root.staticAllowed
        TextField {
            id: search
            Layout.fillWidth: true
            font.family: Theme.fontMono
            placeholderText: qsTr("None")
            Accessible.name: qsTr("%1 search domains").arg(root.title)
            onTextEdited: root.edited()
        }
    }
    FormRow {
        label: qsTr("Ignore automatic DNS")
        hint: qsTr("Use only the servers above, not those announced by DHCP or the router")
        Switch {
            id: ignoreAuto
            Accessible.name: qsTr("%1: ignore automatic DNS").arg(root.title)
            onToggled: root.edited()
        }
        Item { Layout.fillWidth: true }
    }
}
