// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.controlcenter

NxPage {
    id: page
    title: qsTr("Updates")
    subtitle: qsTr("How this computer looks for updates and whether it installs them by itself. Updates are reviewed and installed in NEXUS Updater.")

    // Bumped when a change is refused, so switches show the real value again.
    property int revision: 0

    function intervalText(hours) {
        if (hours === 1) return qsTr("Every hour")
        if (hours < 24 || hours % 24 !== 0) return qsTr("Every %1 hours").arg(hours)
        if (hours === 24) return qsTr("Every day")
        if (hours === 168) return qsTr("Every week")
        return qsTr("Every %1 days").arg(hours / 24)
    }
    function intervalChoices(current) {
        const hours = [1, 3, 6, 12, 24, 48, 72, 168]
        if (hours.indexOf(current) < 0) hours.push(current)
        hours.sort((a, b) => a - b)
        return hours.map(h => ({ value: h, text: page.intervalText(h) }))
    }
    function periodicText(value) {
        if (value === "" || value === "0") return qsTr("Off")
        if (value === "1") return qsTr("Daily")
        if (value === "always") return qsTr("At every run")
        return qsTr("Every %1 days").arg(value)
    }

    actions: [
        NxButton {
            visible: updates.updaterAvailable
            text: qsTr("Open NEXUS Updater")
            kind: "primary"
            iconName: "external-link"
            onClicked: if (!updates.openUpdater()) page.ApplicationWindow.window.toaster.show(qsTr("Could not start NEXUS Updater"), "danger")
        }
    ]

    UpdatesBackend {
        id: updates
        onFailed: (message) => {
            page.revision++
            page.ApplicationWindow.window.toaster.show(message, "danger")
        }
        onSucceeded: (message) => page.ApplicationWindow.window.toaster.show(message, "success")
    }

    NxBanner {
        visible: !updates.updaterAvailable
        kind: "warning"
        title: qsTr("NEXUS Updater is not installed")
        message: qsTr("The settings below are stored, but nothing on this computer checks for updates with them until the nexus-updater program is installed.")
    }

    NxBanner {
        visible: updates.rebootRequired
        kind: "info"
        title: qsTr("A restart is required to finish installing updates")
        message: qsTr("/run/reboot-required exists: updated components are not in use yet.")
    }

    NxSection {
        title: qsTr("Last check")

        NxRow {
            icon: !updates.lastCheckKnown ? "info"
                : updates.lastError !== "" ? "alert-triangle"
                : updates.lastSecurity > 0 ? "shield-alert"
                : updates.lastUpdates > 0 ? "arrow-up-circle" : "check-circle"
            iconColor: !updates.lastCheckKnown ? Theme.textSecondary
                     : updates.lastError !== "" ? Theme.danger
                     : updates.lastSecurity > 0 ? Theme.danger
                     : updates.lastUpdates > 0 ? Theme.warning : Theme.success
            title: !updates.lastCheckKnown ? qsTr("No check has been recorded for this user")
                 : updates.lastError !== "" ? qsTr("The last check failed")
                 : updates.lastUpdates === 0 ? qsTr("No updates were available")
                 : updates.lastSecurity > 0 ? qsTr("%1 update(s) were available, %2 of them security updates").arg(updates.lastUpdates).arg(updates.lastSecurity)
                 : qsTr("%1 update(s) were available").arg(updates.lastUpdates)
            subtitle: {
                if (!updates.lastCheckKnown) return qsTr("NEXUS Updater records each check in %1.").arg(updates.settingsFile)
                const parts = [qsTr("Checked %1").arg(updates.lastCheck.toLocaleString(Qt.locale(), Locale.ShortFormat))]
                if (updates.lastError !== "") parts.push(updates.lastError)
                else if (updates.lastRefreshError !== "") parts.push(qsTr("The package lists could not be refreshed: %1").arg(updates.lastRefreshError))
                return parts.join("  ·  ")
            }
            NxButton {
                anchors.verticalCenter: parent.verticalCenter
                visible: updates.updaterAvailable
                text: qsTr("Check now")
                iconName: "refresh"
                busy: updates.checking
                onClicked: updates.checkNow()
            }
        }
    }

    NxSection {
        title: qsTr("Checking for updates")
        description: qsTr("Used by NEXUS Updater. Stored in %1.").arg(updates.settingsFile)

        NxRow {
            icon: "refresh"
            title: qsTr("Check for updates automatically")
            subtitle: qsTr("Checking only looks for updates; nothing is installed.")
            Switch {
                anchors.verticalCenter: parent.verticalCenter
                checked: updates.autoCheck
                Accessible.name: qsTr("Check for updates automatically")
                onToggled: {
                    updates.autoCheck = checked
                    checked = Qt.binding(() => updates.autoCheck)
                }
            }
        }
        NxRow {
            icon: "clock"
            title: qsTr("How often")
            enabled: updates.autoCheck
            ComboBox {
                id: intervalBox
                anchors.verticalCenter: parent.verticalCenter
                width: 190
                readonly property var choices: page.intervalChoices(updates.checkIntervalHours)
                model: choices
                textRole: "text"
                valueRole: "value"
                currentIndex: {
                    for (let i = 0; i < choices.length; i++) if (choices[i].value === updates.checkIntervalHours) return i
                    return -1
                }
                Accessible.name: qsTr("How often to check for updates")
                onActivated: updates.checkIntervalHours = currentValue
            }
        }
    }

    NxSection {
        title: qsTr("Automatic installation")
        description: qsTr("Done by the unattended-upgrades service of the system, for all users. Which updates it installs is set in /etc/apt/apt.conf.d/50unattended-upgrades (Debian's default: security updates). Changing these switches asks for an administrator password.")

        NxRow {
            visible: updates.systemReady && !updates.unattendedInstalled
            icon: "info"
            title: qsTr("Automatic installation is not available")
            subtitle: qsTr("The unattended-upgrades package is not installed. Updates are installed only when you start them in NEXUS Updater.")
        }
        NxRow {
            visible: updates.systemReady && !updates.periodicKnown
            icon: "alert-triangle"
            iconColor: Theme.warning
            title: qsTr("The APT configuration could not be read")
            subtitle: updates.periodicSource
        }
        NxRow {
            visible: updates.periodicKnown
            icon: "download"
            title: qsTr("Refresh the package lists in the background")
            subtitle: qsTr("APT::Periodic::Update-Package-Lists: %1").arg(page.periodicText(updates.updatePackageLists))
            Switch {
                anchors.verticalCenter: parent.verticalCenter
                enabled: !updates.busy
                checked: (page.revision, updates.listsEnabled)
                Accessible.name: qsTr("Refresh the package lists in the background")
                onToggled: {
                    const wanted = checked
                    checked = Qt.binding(() => (page.revision, updates.listsEnabled))
                    // Installing without fresh lists would install nothing new.
                    updates.setAutomatic(wanted, wanted && updates.unattendedEnabled)
                }
            }
        }
        NxRow {
            visible: updates.periodicKnown
            icon: "shield-check"
            title: qsTr("Install updates automatically")
            subtitle: {
                const parts = [qsTr("APT::Periodic::Unattended-Upgrade: %1").arg(page.periodicText(updates.unattendedUpgrade))]
                if (!updates.unattendedInstalled) parts.push(qsTr("has no effect until unattended-upgrades is installed"))
                else if (updates.timerState !== "" && updates.timerState !== "enabled")
                    parts.push(qsTr("apt-daily-upgrade.timer is %1, so nothing runs").arg(updates.timerState))
                return parts.join("  ·  ")
            }
            Switch {
                anchors.verticalCenter: parent.verticalCenter
                enabled: !updates.busy && (updates.unattendedInstalled || updates.unattendedEnabled)
                checked: (page.revision, updates.unattendedEnabled)
                Accessible.name: qsTr("Install updates automatically")
                onToggled: {
                    const wanted = checked
                    checked = Qt.binding(() => (page.revision, updates.unattendedEnabled))
                    updates.setAutomatic(wanted || updates.listsEnabled, wanted)
                }
            }
        }
    }
}
