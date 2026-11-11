// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.networkcenter

NxWindow {
    id: window
    title: qsTr("Network Center")
    width: 1200
    height: 800
    minimumWidth: 980
    minimumHeight: 600

    // key: stable identifier, also the command-line argument (`--page <key>`).
    readonly property var pages: [
        { key: "overview",    title: qsTr("Overview"),           icon: "gauge",    section: qsTr("Network"),       source: "pages/OverviewPage.qml" },
        { key: "wifi",        title: qsTr("Wi-Fi"),              icon: "wifi",     section: qsTr("Network"),       source: "pages/WifiPage.qml" },
        { key: "interfaces",  title: qsTr("Interfaces"),         icon: "ethernet", section: qsTr("Network"),       source: "pages/InterfacesPage.qml" },
        { key: "ip",          title: qsTr("IP configuration"),   icon: "sliders",  section: qsTr("Configuration"), source: "pages/IpConfigPage.qml" },
        { key: "dns",         title: qsTr("DNS"),                icon: "globe",    section: qsTr("Configuration"), source: "pages/DnsPage.qml" },
        { key: "routes",      title: qsTr("Routing table"),      icon: "network",  section: qsTr("Monitoring"),    source: "pages/RoutesPage.qml" },
        { key: "connections", title: qsTr("Active connections"), icon: "plug",     section: qsTr("Monitoring"),    source: "pages/ConnectionsPage.qml" },
        { key: "vpn",         title: qsTr("VPN"),                icon: "shield",   section: qsTr("Security and tools"), source: "pages/VpnPage.qml" },
        { key: "diagnostics", title: qsTr("Diagnostics"),        icon: "wrench",   section: qsTr("Security and tools"), source: "pages/DiagnosticsPage.qml" },
        { key: "capture",     title: qsTr("Packet capture"),     icon: "radar",    section: qsTr("Security and tools"), source: "pages/CapturePage.qml" },
    ]

    property string currentKey: ""
    // Pages are created on first visit and then kept, so a running capture,
    // diagnostic output and half-filled forms survive navigation.
    property var visited: ({})

    function openPage(key) {
        for (let i = 0; i < pages.length; i++) {
            if (pages[i].key === key) {
                if (visited[key] !== true) {
                    visited[key] = true
                    visitedChanged()
                }
                currentKey = key
                return true
            }
        }
        return false
    }

    Component.onCompleted: {
        const args = nexusApp.arguments
        if (!(args.length > 0 && openPage(args[0])))
            openPage("overview")
    }

    Connections {
        target: nexusApp
        function onActivationRequested(args) {
            if (args.length > 0)
                window.openPage(args[0])
            window.raise()
            window.requestActivate()
        }
    }

    // The back-ends only poll while the window is on screen.
    Binding { target: NetState; property: "active"; value: window.visible }
    Binding { target: Nm; property: "active"; value: window.visible }

    Connections {
        target: Nm
        function onOperationFinished(operation, target, ok, message) {
            window.toaster.show(message, ok ? "success" : "danger")
        }
        function onSecretsRequired(uuid, name) {
            secretDialog.uuid = uuid
            secretDialog.title = qsTr("Password required")
            secretDialog.message = qsTr("“%1” needs a password that is not stored. It is passed to NetworkManager and not kept by this application.").arg(name)
            secretDialog.open()
        }
    }

    SecretDialog {
        id: secretDialog
        property string uuid: ""
        confirmText: qsTr("Connect")
        onSubmitted: (secret) => Nm.activateWithSecret(uuid, secret)
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        NxSidebar {
            Layout.fillHeight: true
            title: qsTr("Network Center")
            model: window.pages
            currentKey: window.currentKey
            onActivated: (key) => window.openPage(key)
            footer: RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.spaceSm
                Layout.bottomMargin: Theme.spaceXs
                spacing: Theme.spaceSm
                NxBadge {
                    kind: Nm.status === "running" ? "success" : Nm.status === "unknown" ? "neutral" : "warning"
                    text: Nm.status === "running" ? qsTr("NetworkManager %1").arg(Nm.version)
                        : Nm.status === "unknown" ? qsTr("Checking NetworkManager…")
                        : Nm.status === "missing" ? qsTr("NetworkManager not installed")
                        : Nm.status === "not-running" ? qsTr("NetworkManager not running")
                        : qsTr("NetworkManager unavailable")
                }
                Item { Layout.fillWidth: true }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            Repeater {
                model: window.pages
                Item {
                    id: slot
                    required property var modelData
                    anchors.fill: parent
                    visible: modelData.key === window.currentKey

                    Loader {
                        id: pageLoader
                        anchors.fill: parent
                        active: window.visited[slot.modelData.key] === true
                        source: Qt.resolvedUrl(slot.modelData.source)
                    }

                    // A page that fails to load is a packaging or programming
                    // error; show it plainly instead of an empty pane.
                    NxEmptyState {
                        anchors.fill: parent
                        visible: pageLoader.status === Loader.Error
                        tone: "danger"
                        icon: "x-circle"
                        title: qsTr("This page could not be loaded")
                        message: qsTr("%1 failed to load. Run nexus-network-center from a terminal to see the error.").arg(slot.modelData.source)
                    }
                }
            }
        }
    }
}
