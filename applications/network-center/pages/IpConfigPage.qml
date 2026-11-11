// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.networkcenter

NxPage {
    id: page
    title: qsTr("IP configuration")
    subtitle: qsTr("Addressing of each NetworkManager connection profile. Changes are validated, shown for review and then written to the profile through NetworkManager.")
    maxContentWidth: 960

    // Profiles whose settings NetworkManager returned (loopback has nothing to edit).
    readonly property var profiles: Nm.connections.filter(c => c.hasDetails && c.type !== "loopback")
    property string uuid: ""
    readonly property var connection: Nm.connections.find(c => c.uuid === uuid) || ({})
    readonly property var device: Nm.devices.find(d => d.name === connection.device) || ({})
    // What the form says now, and what applying it would do.
    property var config: ({})
    readonly property var plan: connection.uuid !== undefined ? Nm.planIpChange(connection.uuid, config)
                                                              : ({ ok: false, errors: [], warnings: [], changes: [] })
    property bool reloadAfterRefresh: false

    function select(newUuid) {
        uuid = newUuid
        load()
    }
    function load() {
        v4.load(connection.ipv4)
        v6.load(connection.ipv6)
        collect()
    }
    function collect() {
        config = { ipv4: v4.value(), ipv6: v6.value() }
    }
    function profileLabel(c) {
        return c.name + "  —  " + c.typeLabel + (c.device !== "" ? qsTr(" on %1").arg(c.device) : qsTr(", not active"))
    }

    function ensureSelection() {
        // Keep the selection while it exists; otherwise prefer an active profile.
        if (uuid !== "" && profiles.some(c => c.uuid === uuid))
            return
        const preferred = profiles.find(c => c.active) || profiles[0]
        select(preferred ? preferred.uuid : "")
    }
    onProfilesChanged: ensureSelection()
    Component.onCompleted: ensureSelection()

    Connections {
        target: Nm
        function onOperationFinished(operation, target, ok, message) {
            if (operation === "ip-config" && target === page.uuid && ok)
                page.reloadAfterRefresh = true
        }
        function onRefreshed() {
            if (page.reloadAfterRefresh) {
                page.reloadAfterRefresh = false
                page.load()
            }
        }
    }

    ChangeReviewDialog {
        id: review
        plan: page.plan
        onApply: (reactivate) => Nm.applyIpChange(page.uuid, page.config, reactivate)
    }

    NmUnavailable {
        Layout.fillWidth: true
        Layout.topMargin: Theme.spaceXl
        visible: !Nm.running
        feature: qsTr("IP settings are stored in NetworkManager connection profiles. The addresses currently configured in the kernel are on the Interfaces page.")
    }

    NxEmptyState {
        Layout.fillWidth: true
        Layout.topMargin: Theme.spaceXl
        visible: Nm.running && page.profiles.length === 0
        icon: "sliders"
        title: qsTr("No connection profiles")
        message: qsTr("NetworkManager has no connection profile to configure. A profile is created when a device is connected for the first time.")
    }

    NxSection {
        visible: Nm.running && page.profiles.length > 0
        title: qsTr("Connection")

        FormRow {
            label: qsTr("Profile")
            ComboBox {
                id: profileBox
                Layout.fillWidth: true
                model: page.profiles.map(c => ({ text: page.profileLabel(c), uuid: c.uuid }))
                textRole: "text"
                valueRole: "uuid"
                currentIndex: page.profiles.findIndex(c => c.uuid === page.uuid)
                Accessible.name: qsTr("Connection profile")
                onActivated: page.select(currentValue)
            }
        }
        NxRow {
            icon: page.connection.active ? "check-circle" : "circle"
            iconColor: page.connection.active ? Theme.success : Theme.textMuted
            title: page.connection.active ? qsTr("Active on %1").arg(page.connection.device) : qsTr("Not active")
            subtitle: page.connection.active
                ? [(page.device.addresses4 || []).concat(page.device.addresses6 || []).join("  "),
                   page.device.gateway4 ? qsTr("gateway %1").arg(page.device.gateway4) : "",
                   page.device.gateway6 ? qsTr("gateway %1").arg(page.device.gateway6) : ""].filter(s => s !== "").join(" · ")
                  || qsTr("No address assigned at the moment")
                : qsTr("Saved settings are shown; they take effect when the connection is activated.")
            NxBadge {
                visible: page.connection.readOnly === true
                kind: "warning"
                text: qsTr("Read-only")
            }
            NxButton {
                visible: page.connection.active !== true
                text: qsTr("Activate")
                busy: Nm.busyTargets.indexOf(page.uuid) >= 0
                onClicked: Nm.activateConnection(page.uuid)
            }
            NxButton {
                visible: page.connection.active === true
                text: qsTr("Deactivate")
                busy: Nm.busyTargets.indexOf(page.uuid) >= 0
                onClicked: Nm.deactivateConnection(page.uuid)
            }
        }
    }

    IpFamilyEditor {
        id: v4
        visible: Nm.running && page.profiles.length > 0
        family: 4
        onEdited: page.collect()
    }
    IpFamilyEditor {
        id: v6
        visible: Nm.running && page.profiles.length > 0
        family: 6
        onEdited: page.collect()
    }

    NxBanner {
        visible: Nm.running && page.profiles.length > 0 && (page.plan.errors || []).length > 0
        kind: "danger"
        title: qsTr("These settings cannot be applied")
        message: (page.plan.errors || []).join("\n")
    }
    NxBanner {
        visible: Nm.running && page.profiles.length > 0 && page.plan.ok === true && (page.plan.warnings || []).length > 0
        kind: "warning"
        message: (page.plan.warnings || []).join("\n")
    }

    RowLayout {
        Layout.fillWidth: true
        visible: Nm.running && page.profiles.length > 0
        spacing: Theme.spaceSm

        Label {
            Layout.fillWidth: true
            text: page.plan.ok !== true ? ""
                : (page.plan.changes || []).length === 0 ? qsTr("No changes.")
                : qsTr("%1 setting(s) will change.").arg(page.plan.changes.length)
            color: Theme.textMuted
        }
        NxButton {
            text: qsTr("Revert")
            enabled: (page.plan.changes || []).length > 0 || page.plan.ok !== true
            onClicked: page.load()
        }
        NxButton {
            text: qsTr("Review changes…")
            kind: "primary"
            busy: Nm.busyTargets.indexOf(page.uuid) >= 0
            enabled: page.plan.ok === true && (page.plan.changes || []).length > 0 && page.connection.readOnly !== true
            onClicked: review.open()
        }
    }
}
