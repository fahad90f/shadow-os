// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.nexus.ui
import "Fmt.js" as Fmt

// Section "services": running system services and failed units (systemd).
SectionPage {
    id: page

    readonly property var sv: ready ? section : ({})
    readonly property var running: Fmt.list(sv.running)
    readonly property var failed: Fmt.list(sv.failed)

    sectionKey: "services"
    title: qsTr("Services")
    sectionTitle: qsTr("The running services")
    subtitle: qsTr("System services that are running now, and units that failed.")

    NxSection {
        visible: page.ready
        title: qsTr("Failed units")
        description: page.failed.length > 0
            ? qsTr("These units ended with an error. “systemctl status <unit>” in a terminal shows why.") : ""
        NxRow {
            visible: page.failed.length === 0
            icon: "check-circle"
            iconColor: Theme.success
            title: qsTr("No unit has failed")
            subtitle: qsTr("systemctl list-units --state=failed is empty.")
        }
        Repeater {
            model: page.failed
            NxRow {
                required property var modelData
                required property int index
                Layout.topMargin: index === 0 ? -1 : 0      // hides NxRow's divider on the first visible row
                icon: "x-circle"
                iconColor: Theme.danger
                title: String(modelData)
                NxBadge { text: qsTr("Failed"); kind: "danger" }
            }
        }
    }

    NxSection {
        visible: page.ready
        title: qsTr("Running services")
        description: qsTr("%1 service(s) are running.").arg(Fmt.has(page.sv.running_count) ? page.sv.running_count : page.running.length)
        headerActions: [
            NxSearchField {
                id: search
                width: 260
                placeholderText: qsTr("Filter services")
            }
        ]
        TableCard {
            Layout.margins: Theme.spaceXs
            color: "transparent"
            border.width: 0
            maxVisibleRows: 16
            rows: page.running
            filterText: search.text
            filterRoles: ["unit", "description"]
            sortRole: "unit"
            emptyText: qsTr("No service is running")
            columns: [
                { title: qsTr("Unit"), role: "unit", width: 340, mono: true },
                { title: qsTr("Description"), role: "description", fill: true }
            ]
        }
    }
}
