// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui

NxWindow {
    id: window
    title: qsTr("Control Center")
    width: 1080
    height: 760

    // The page table. One entry per settings page:
    //   key      stable identifier — also the command-line argument and --dump topic
    //   title    sidebar label
    //   icon     NexusUI symbolic icon
    //   section  sidebar group
    //   source   QML file under pages/
    //   keywords extra search terms
    // Only pages that are part of this build are offered: an entry whose QML
    // is not compiled in would be a control that does nothing.
    readonly property var pages: allPages.filter(p => availablePages.indexOf(p.source.split("/").pop()) >= 0)

    readonly property var allPages: [
        { key: "wifi",          title: qsTr("Wi-Fi"),              icon: "wifi",          section: qsTr("Network"),         source: "pages/WifiPage.qml",          keywords: "wireless wlan hotspot" },
        { key: "network",       title: qsTr("Wired & Proxy"),      icon: "ethernet",      section: qsTr("Network"),         source: "pages/NetworkPage.qml",       keywords: "ethernet lan ip dns proxy" },
        { key: "bluetooth",     title: qsTr("Bluetooth"),          icon: "bluetooth",     section: qsTr("Network"),         source: "pages/BluetoothPage.qml",     keywords: "pair device" },
        { key: "vpn",           title: qsTr("VPN"),                icon: "shield",        section: qsTr("Network"),         source: "pages/VpnPage.qml",           keywords: "wireguard openvpn tunnel" },
        { key: "display",       title: qsTr("Displays"),           icon: "monitor",       section: qsTr("Hardware"),        source: "pages/DisplayPage.qml",       keywords: "screen resolution scale monitor refresh rotation" },
        { key: "sound",         title: qsTr("Sound"),              icon: "volume-high",   section: qsTr("Hardware"),        source: "pages/SoundPage.qml",         keywords: "audio volume microphone speaker output input" },
        { key: "power",         title: qsTr("Power & Battery"),    icon: "battery",       section: qsTr("Hardware"),        source: "pages/PowerPage.qml",         keywords: "suspend sleep profile energy lid" },
        { key: "keyboard",      title: qsTr("Keyboard"),           icon: "keyboard",      section: qsTr("Hardware"),        source: "pages/KeyboardPage.qml",      keywords: "layout shortcuts repeat" },
        { key: "mouse",         title: qsTr("Mouse & Touchpad"),   icon: "mouse",         section: qsTr("Hardware"),        source: "pages/MousePage.qml",         keywords: "pointer speed scroll tap" },
        { key: "storage",       title: qsTr("Storage"),            icon: "harddrive",     section: qsTr("Hardware"),        source: "pages/StoragePage.qml",       keywords: "disk space usage" },
        { key: "appearance",    title: qsTr("Appearance"),         icon: "palette",       section: qsTr("Personalisation"), source: "pages/AppearancePage.qml",    keywords: "theme dark light accent wallpaper dock panel font" },
        { key: "accessibility", title: qsTr("Accessibility"),      icon: "accessibility", section: qsTr("Personalisation"), source: "pages/AccessibilityPage.qml", keywords: "motion contrast text size zoom" },
        { key: "startup",       title: qsTr("Startup Applications"), icon: "play",        section: qsTr("Personalisation"), source: "pages/StartupPage.qml",       keywords: "autostart login" },
        { key: "users",         title: qsTr("Users"),              icon: "users",         section: qsTr("Privacy & Security"), source: "pages/UsersPage.qml",      keywords: "account password administrator" },
        { key: "privacy",       title: qsTr("Privacy"),            icon: "eye-off",       section: qsTr("Privacy & Security"), source: "pages/PrivacyPage.qml",    keywords: "history lock screen location recent files" },
        { key: "firewall",      title: qsTr("Firewall"),           icon: "firewall",      section: qsTr("Privacy & Security"), source: "pages/FirewallPage.qml",   keywords: "nftables ports rules" },
        { key: "security",      title: qsTr("Security"),           icon: "shield-check",  section: qsTr("Privacy & Security"), source: "pages/SecurityPage.qml",   keywords: "apparmor encryption secure boot hardening" },
        { key: "updates",       title: qsTr("Updates"),            icon: "arrow-up-circle", section: qsTr("System"),        source: "pages/UpdatesPage.qml",       keywords: "upgrade packages automatic" },
        { key: "language",      title: qsTr("Region & Language"),  icon: "language",      section: qsTr("System"),          source: "pages/LanguagePage.qml",      keywords: "locale format translation" },
        { key: "datetime",      title: qsTr("Date & Time"),        icon: "clock",         section: qsTr("System"),          source: "pages/DateTimePage.qml",      keywords: "timezone clock ntp" },
        { key: "about",         title: qsTr("About"),              icon: "info",          section: qsTr("System"),          source: "pages/AboutPage.qml",         keywords: "version system information hardware" },
    ]

    property string currentKey: ""
    property string filter: ""

    readonly property var visiblePages: {
        const needle = filter.trim().toLowerCase()
        if (needle === "") return pages
        return pages.filter(p => (p.title + " " + p.section + " " + p.keywords).toLowerCase().indexOf(needle) >= 0)
    }

    function pageFor(key) {
        for (let i = 0; i < pages.length; i++) if (pages[i].key === key) return pages[i]
        return null
    }

    function openPage(key) {
        const page = pageFor(key)
        if (!page) return false
        currentKey = key
        return true
    }

    Component.onCompleted: {
        const args = nexusApp.arguments
        if (!(args.length > 0 && openPage(args[0]))) openPage(pages[0].key)
    }

    Connections {
        target: nexusApp
        function onActivationRequested(args) {
            if (args.length > 0) window.openPage(args[0])
            window.raise()
            window.requestActivate()
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        NxSidebar {
            Layout.fillHeight: true
            title: qsTr("Settings")
            model: window.visiblePages
            currentKey: window.currentKey
            onActivated: (key) => window.openPage(key)
            header: NxSearchField {
                Layout.fillWidth: true
                placeholderText: qsTr("Search settings")
                onTextChanged: window.filter = text
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            Loader {
                id: loader
                anchors.fill: parent
                readonly property var page: window.pageFor(window.currentKey)
                source: page ? Qt.resolvedUrl(page.source) : ""
                asynchronous: false
            }

            // A page that fails to load is a packaging or programming error;
            // show it plainly instead of an empty pane.
            NxEmptyState {
                anchors.fill: parent
                visible: loader.status === Loader.Error || (loader.page === null && window.currentKey !== "")
                tone: "danger"
                icon: "x-circle"
                title: qsTr("This settings page could not be loaded")
                message: loader.page ? qsTr("%1 failed to load. Run nexus-control-center from a terminal to see the error.").arg(loader.page.source) : ""
            }
        }
    }
}
