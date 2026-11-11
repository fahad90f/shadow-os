// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.nexus.ui
import "Fmt.js" as Fmt

// Section "updates": what apt would upgrade, from the package lists on disk.
SectionPage {
    id: page

    signal updaterRequested()

    readonly property var up: ready ? section : ({})
    readonly property var packages: Fmt.list(up.packages)

    sectionKey: "updates"
    title: qsTr("Updates")
    sectionTitle: qsTr("The update state")
    subtitle: qsTr("Pending package upgrades according to the package lists on this computer. Nothing is downloaded or installed here.")

    actions: [
        NxButton {
            visible: page.center.updaterInstalled
            text: qsTr("Open NEXUS Updater")
            iconName: "external-link"
            kind: "primary"
            onClicked: page.updaterRequested()
        }
    ]

    NxBanner {
        visible: page.ready && page.up.lists_present === false
        kind: "warning"
        title: qsTr("Update information has never been downloaded")
        message: qsTr("Without package lists the system cannot know whether updates exist; “0 updates” below means “unknown”.")
    }
    NxBanner {
        visible: page.ready && page.up.reboot_required === true
        kind: "warning"
        title: qsTr("A restart is required to finish installing updates")
        message: Fmt.list(page.up.reboot_required_packages).length > 0
            ? qsTr("Not in use yet: %1").arg(Fmt.list(page.up.reboot_required_packages).join(", ")) : ""
    }

    GridLayout {
        Layout.fillWidth: true
        visible: page.ready
        columns: width >= 700 ? 3 : 1
        columnSpacing: Theme.spaceMd
        rowSpacing: Theme.spaceMd
        NxStatTile {
            Layout.fillWidth: true
            label: qsTr("Security updates")
            value: String(page.up.security || 0)
            detail: page.up.security > 0 ? qsTr("close known vulnerabilities") : qsTr("none waiting")
            icon: "shield-alert"
            tone: page.up.security > 0 ? "danger" : page.up.lists_present ? "success" : "neutral"
        }
        NxStatTile {
            Layout.fillWidth: true
            label: qsTr("All pending updates")
            value: String(page.up.upgradable || 0)
            detail: qsTr("packages apt would upgrade or install")
            icon: "package"
            tone: page.up.upgradable > 0 ? "warning" : "neutral"
        }
        NxStatTile {
            Layout.fillWidth: true
            label: qsTr("Package lists refreshed")
            value: Fmt.has(page.up.last_refresh_age_days) ? qsTr("%1 days ago").arg(page.up.last_refresh_age_days) : qsTr("Never")
            detail: Fmt.has(page.up.last_refresh) ? page.center.localTime(page.up.last_refresh) : qsTr("no lists in /var/lib/apt/lists")
            icon: "clock"
            tone: !Fmt.has(page.up.last_refresh_age_days) || page.up.last_refresh_age_days > 14 ? "warning" : "neutral"
        }
    }

    NxSection {
        visible: page.ready
        title: qsTr("Details")
        NxRow {
            title: qsTr("Restart required")
            subtitle: qsTr("/run/reboot-required")
            NxBadge {
                text: page.up.reboot_required ? qsTr("Yes") : qsTr("No")
                kind: page.up.reboot_required ? "warning" : "success"
            }
        }
        NxRow {
            title: qsTr("Last installation of updates")
            subtitle: Fmt.has(page.up.last_upgrade) ? ""
                : qsTr("The status tool reports when the package lists were refreshed, not when updates were last installed. NEXUS Updater shows the update history.")
            value: Fmt.has(page.up.last_upgrade) ? page.center.localTime(page.up.last_upgrade) : qsTr("not reported")
        }
        NxRow {
            visible: !page.center.updaterInstalled
            icon: "info"
            title: qsTr("NEXUS Updater is not installed")
            subtitle: qsTr("Install the package nexus-updater to install updates from the desktop, or use “sudo apt full-upgrade” in a terminal.")
        }
    }

    NxSection {
        visible: page.ready && page.packages.length > 0
        title: qsTr("Pending packages")
        description: page.up.upgradable > page.packages.length
            ? qsTr("The first %1 of %2 packages.").arg(page.packages.length).arg(page.up.upgradable) : ""
        TableCard {
            Layout.margins: Theme.spaceXs
            color: "transparent"
            border.width: 0
            maxVisibleRows: 14
            rows: page.packages
            sortRole: "security"
            sortAscending: false
            columns: [
                { title: qsTr("Package"), role: "package", fill: true, mono: true },
                { title: qsTr("Installed"), role: "installed", width: 210, mono: true },
                { title: qsTr("Available"), role: "candidate", width: 210, mono: true },
                { title: qsTr("Kind"), role: "security", width: 110,
                  format: (v) => v ? qsTr("Security") : qsTr("Regular"), color: (v) => v ? Theme.danger : Theme.textSecondary }
            ]
        }
    }
}
