// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.monitor

NxWindow {
    id: appWindow
    title: qsTr("System Monitor")
    width: 1240
    height: 800
    minimumWidth: 980
    minimumHeight: 600

    // key: stable identifier, also the command-line argument ("nexus-monitor --page <key>").
    readonly property var pages: [
        { key: "overview",  title: qsTr("Overview"),  icon: "gauge",       section: qsTr("System") },
        { key: "processes", title: qsTr("Processes"), icon: "list",        section: qsTr("System") },
        { key: "services",  title: qsTr("Services"),  icon: "layers",      section: qsTr("System") },
        { key: "disks",     title: qsTr("Disks"),     icon: "harddrive",   section: qsTr("Resources") },
        { key: "network",   title: qsTr("Network"),   icon: "network",     section: qsTr("Resources") },
        { key: "sensors",   title: qsTr("Sensors"),   icon: "thermometer", section: qsTr("Resources") },
    ]
    readonly property var intervals: [1000, 2000, 5000]
    // The page named on the command line, otherwise the overview.
    property string currentKey: nexusApp.arguments.length > 0 && isPage(nexusApp.arguments[0])
        ? nexusApp.arguments[0] : "overview"

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

    // A second launch (for example the Task Manager launcher while the monitor
    // is open) hands its page over to this window.
    Connections {
        target: nexusApp
        function onActivationRequested(args) {
            if (args.length > 0) appWindow.openPage(args[0])
            appWindow.show()
            appWindow.raise()
            appWindow.requestActivate()
        }
    }

    // Nothing is sampled while the window is hidden or minimised.
    ExposureWatcher { id: exposure; window: appWindow }
    SystemMonitor { id: sysmon; active: exposure.exposed }

    // A page exists only while it is shown, so a hidden page costs nothing —
    // neither its bindings nor the frames its animations would request.
    // Pages with state worth keeping (search text, sort order, selection) set
    // keepAlive: they stay loaded after their first visit and stop sampling
    // while hidden.
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

    RowLayout {
        anchors.fill: parent
        spacing: 0

        NxSidebar {
            Layout.fillHeight: true
            title: qsTr("Monitor")
            model: appWindow.pages
            currentKey: appWindow.currentKey
            onActivated: (key) => appWindow.openPage(key)

            footer: ColumnLayout {
                Layout.fillWidth: true
                Layout.margins: Theme.spaceSm
                spacing: Theme.spaceXs + 2
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Update interval")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSizeSmall
                }
                NxSegmented {
                    model: [qsTr("1 s"), qsTr("2 s"), qsTr("5 s")]
                    currentIndex: Math.max(0, appWindow.intervals.indexOf(sysmon.interval))
                    onActivated: (index) => sysmon.interval = appWindow.intervals[index]
                }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            PageSlot {
                current: appWindow.currentKey === "overview"
                sourceComponent: OverviewPage { monitor: sysmon }
            }
            PageSlot {
                current: appWindow.currentKey === "processes"
                keepAlive: true
                sourceComponent: ProcessesPage { monitor: sysmon }
            }
            PageSlot {
                current: appWindow.currentKey === "services"
                keepAlive: true
                sourceComponent: ServicesPage { monitor: sysmon }
            }
            PageSlot {
                current: appWindow.currentKey === "disks"
                sourceComponent: DisksPage { monitor: sysmon }
            }
            PageSlot {
                current: appWindow.currentKey === "network"
                sourceComponent: NetworkPage { monitor: sysmon }
            }
            PageSlot {
                current: appWindow.currentKey === "sensors"
                sourceComponent: SensorsPage { monitor: sysmon }
            }
        }
    }
}
