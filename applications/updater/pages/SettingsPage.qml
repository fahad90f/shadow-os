// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.updater

NxPage {
    id: page

    required property QtObject updater

    readonly property var intervals: [
        { hours: 6, label: qsTr("Every 6 hours") },
        { hours: 12, label: qsTr("Every 12 hours") },
        { hours: 24, label: qsTr("Once a day") },
        { hours: 72, label: qsTr("Every 3 days") },
        { hours: 168, label: qsTr("Once a week") },
    ]
    function intervalIndex(hours) {
        for (let i = 0; i < intervals.length; i++) if (intervals[i].hours === hours) return i
        return -1
    }

    title: qsTr("Settings")
    subtitle: qsTr("How the desktop session looks for updates. Updates are never installed automatically.")

    NxSection {
        title: qsTr("Automatic update check")
        NxRow {
            icon: "refresh"
            title: qsTr("Check for updates automatically")
            subtitle: qsTr("Stores whether the desktop session should look for updates periodically and notify you. The check is “shadow-updater --check”; it only refreshes the software lists and counts the updates.")
            Switch {
                checked: page.updater.autoCheck
                Accessible.name: qsTr("Check for updates automatically")
                onToggled: page.updater.autoCheck = checked
            }
        }
        NxRow {
            icon: "clock"
            title: qsTr("How often")
            subtitle: page.intervalIndex(page.updater.checkIntervalHours) < 0
                ? qsTr("Currently every %1 hours (set in the settings file)").arg(page.updater.checkIntervalHours) : ""
            enabled: page.updater.autoCheck
            ComboBox {
                width: 190
                enabled: page.updater.autoCheck
                model: page.intervals.map(i => i.label)
                currentIndex: page.intervalIndex(page.updater.checkIntervalHours)
                Accessible.name: qsTr("How often to check for updates")
                onActivated: (index) => page.updater.checkIntervalHours = page.intervals[index].hours
            }
        }
    }

    NxSection {
        title: qsTr("Last check")
        NxRow {
            icon: "calendar"
            title: qsTr("When")
            value: page.updater.lastCheck.getTime() > 0
                ? Qt.formatDateTime(page.updater.lastCheck, Qt.DefaultLocaleShortDate) : qsTr("Never")
        }
        NxRow {
            icon: "info"
            title: qsTr("Result")
            subtitle: page.updater.lastCheckResult !== "" ? page.updater.lastCheckResult : qsTr("No check recorded")
        }
        NxRow {
            icon: "file-text"
            title: qsTr("Settings file")
            value: page.updater.settingsPath
            monoValue: true
        }
    }
}
