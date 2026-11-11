// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.securitycenter
import "Fmt.js" as Fmt

NxWindow {
    id: window
    title: qsTr("NEXUS Security Center")
    width: 1240
    height: 820

    SecurityCenter {
        id: backend
        // The slow timer only runs while somebody can see the result.
        autoRefresh: window.visible && window.visibility !== Window.Minimized
    }

    property string currentKey: "overview"

    readonly property var report: backend.report
    readonly property int failedUnits: Fmt.has(report.services) ? Fmt.list(report.services.failed).length : 0
    readonly property int securityUpdates: Fmt.has(report.updates) && report.updates.security ? report.updates.security : 0
    readonly property int exposed: Fmt.has(report.listening) && report.listening.exposed_count ? report.listening.exposed_count : 0

    readonly property var pages: [
        { key: "overview",   title: qsTr("Overview"),         icon: "shield",          section: qsTr("Status"), badge: "" },
        { key: "firewall",   title: qsTr("Firewall"),         icon: "firewall",        section: qsTr("Protection"),
          badge: Fmt.has(report.firewall) && report.firewall.available !== false && report.firewall.active === false ? "!" : "" },
        { key: "apparmor",   title: qsTr("AppArmor"),         icon: "shield-check",    section: qsTr("Protection"), badge: "" },
        { key: "encryption", title: qsTr("Encryption"),       icon: "disk-lock",       section: qsTr("Protection"), badge: "" },
        { key: "secureboot", title: qsTr("Secure Boot"),      icon: "key",             section: qsTr("Protection"), badge: "" },
        { key: "updates",    title: qsTr("Updates"),          icon: "arrow-up-circle", section: qsTr("Protection"),
          badge: securityUpdates > 0 ? String(securityUpdates) : "" },
        { key: "hardening",  title: qsTr("Hardening"),        icon: "sliders",         section: qsTr("Protection"), badge: "" },
        { key: "network",    title: qsTr("Network exposure"), icon: "globe",           section: qsTr("Activity"),
          badge: exposed > 0 ? String(exposed) : "" },
        { key: "services",   title: qsTr("Services"),         icon: "server",          section: qsTr("Activity"),
          badge: failedUnits > 0 ? String(failedUnits) : "" },
        { key: "logins",     title: qsTr("Logins & audit"),   icon: "users",           section: qsTr("Activity"), badge: "" },
    ]

    function indexOfPage(key) {
        for (let i = 0; i < pages.length; i++) if (pages[i].key === key) return i
        return -1
    }
    function openPage(key) {
        if (indexOfPage(key) < 0) return false
        currentKey = key
        return true
    }
    function copyReport(format) {
        backend.copyText(format === "json" ? backend.reportAsJson() : backend.reportAsText())
        toaster.show(format === "json" ? qsTr("Report copied to the clipboard as JSON")
                                       : qsTr("Report copied to the clipboard as text"), "success")
    }
    function runAction(action) {
        if (action === "firewall-enable") openPage("firewall")
        else if (action === "open-updater") launchUpdater()
    }
    function launchUpdater() {
        if (!backend.openUpdater())
            toaster.show(qsTr("NEXUS Updater could not be started"), "danger")
    }

    Component.onCompleted: {
        const args = nexusApp.arguments
        if (args.length > 0 && !openPage(args[0]))
            toaster.show(qsTr("There is no page named “%1”").arg(args[0]), "warning")
        backend.refresh()
    }

    Connections {
        target: nexusApp
        function onActivationRequested(args) {
            if (args.length > 0) window.openPage(args[0])
            window.raise()
            window.requestActivate()
        }
    }

    Connections {
        target: backend
        function onFirewallFinished(ok, action, message) {
            if (ok) window.toaster.show(message !== "" ? message : qsTr("Firewall changed"), "success")
            else window.toaster.show(qsTr("The firewall was not changed"), "danger")
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        NxSidebar {
            Layout.fillHeight: true
            title: qsTr("Security Center")
            model: window.pages
            currentKey: window.currentKey
            onActivated: (key) => window.openPage(key)
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: window.indexOfPage(window.currentKey)

            OverviewPage {
                center: backend
                onOpenPage: (key) => window.openPage(key)
                onActionRequested: (action) => window.runAction(action)
                onCopyRequested: (format) => window.copyReport(format)
                onSaveRequested: saveDialog.open()
            }
            FirewallPage { center: backend; onOpenPage: (key) => window.openPage(key) }
            AppArmorPage { center: backend }
            EncryptionPage { center: backend }
            SecureBootPage { center: backend; onOpenPage: (key) => window.openPage(key) }
            UpdatesPage { center: backend; onUpdaterRequested: window.launchUpdater() }
            HardeningPage { center: backend }
            NetworkPage { center: backend; onOpenPage: (key) => window.openPage(key) }
            ServicesPage { center: backend }
            LoginsPage { center: backend }
        }
    }

    SaveDialog {
        id: saveDialog
        anchors.centerIn: parent
        center: backend
        onSaved: (path) => window.toaster.show(qsTr("Report saved to %1").arg(path), "success")
    }
}
