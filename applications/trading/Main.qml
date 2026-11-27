// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.trading

NxWindow {
    id: appWindow
    title: qsTr("Shadow Trading")
    width: 1672
    height: 941
    minimumWidth: 1240
    minimumHeight: 720

    // key: stable identifier, also the command-line argument ("shadow-trading <key>").
    readonly property var pages: [
        { key: "live",      title: qsTr("Live Trading"),  icon: "activity" },
        { key: "desk",      title: qsTr("AI Desk"),       icon: "zap" },
        { key: "analysis",  title: qsTr("AI Analysis"),   icon: "radar" },
        { key: "history",   title: qsTr("Trade History"), icon: "clock" },
        { key: "portfolio", title: qsTr("Portfolio"),     icon: "pie" },
        { key: "risk",      title: qsTr("Risk Manager"),  icon: "shield" },
        { key: "models",    title: qsTr("Model Panel"),   icon: "cpu" },
        { key: "settings",  title: qsTr("Settings"),      icon: "settings" },
        { key: "about",     title: qsTr("About"),         icon: "about" },
    ]
    property string currentKey: shadowApp.arguments.length > 0 && isPage(shadowApp.arguments[0])
        ? shadowApp.arguments[0] : "live"

    function isPage(key) {
        for (let i = 0; i < pages.length; i++) {
            if (pages[i].key === key) return true
        }
        return false
    }

    function openPage(key) {
        if (!isPage(key)) return false
        currentKey = key
        return true
    }

    Connections {
        target: shadowApp
        function onActivationRequested(args) {
            if (args.length > 0) appWindow.openPage(args[0])
            appWindow.show()
            appWindow.raise()
            appWindow.requestActivate()
        }
    }

    // A page exists only while it is shown; the live page stays loaded so the
    // chart keeps its zoom and the web view its session.
    component PageSlot: Loader {
        property bool current: false
        property bool keepAlive: false
        property bool visited: false
        onCurrentChanged: if (current) visited = true
        Component.onCompleted: if (current) visited = true
        anchors.fill: parent
        active: current || (keepAlive && visited)
        visible: current
    }

    // The desk's backdrop: near black with a violet light in one corner.
    Rectangle {
        anchors.fill: parent
        color: Theme.bgBase
        Rectangle {
            anchors.fill: parent
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: Theme.alpha(Theme.accent, 0.07) }
                GradientStop { position: 0.6; color: Theme.alpha(Theme.accent, 0.0) }
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Sidebar {
            Layout.fillHeight: true
            model: appWindow.pages
            currentKey: appWindow.currentKey
            onActivated: (key) => appWindow.openPage(key)
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            PageSlot {
                current: appWindow.currentKey === "live"
                keepAlive: true
                sourceComponent: LivePage { onOpenPage: (key) => appWindow.openPage(key) }
            }
            // Kept once opened, so the conversation keeps its scroll position and a draft.
            PageSlot {
                current: appWindow.currentKey === "desk"
                keepAlive: true
                sourceComponent: DeskPage { onOpenPage: (key) => appWindow.openPage(key) }
            }
            PageSlot { current: appWindow.currentKey === "analysis"; sourceComponent: AnalysisPage {} }
            PageSlot { current: appWindow.currentKey === "history"; sourceComponent: HistoryPage {} }
            PageSlot { current: appWindow.currentKey === "portfolio"; sourceComponent: PortfolioPage {} }
            PageSlot { current: appWindow.currentKey === "risk"; sourceComponent: RiskPage {} }
            PageSlot { current: appWindow.currentKey === "models"; sourceComponent: ModelsPage {} }
            PageSlot { current: appWindow.currentKey === "settings"; sourceComponent: SettingsPage {} }
            PageSlot { current: appWindow.currentKey === "about"; sourceComponent: AboutPage {} }
        }
    }
}
