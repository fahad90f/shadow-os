// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import "Fmt.js" as Fmt

// Section "apparmor": kernel module state, profiles by mode, confined processes.
SectionPage {
    id: page

    readonly property var aa: ready ? section : ({})
    readonly property bool enabled_: aa.enabled === true
    readonly property var counts: Fmt.has(aa.profiles) ? aa.profiles : null
    // [{name, mode}] from profile_names (the tool lists the names per mode).
    readonly property var profileRows: {
        const out = []
        const names = Fmt.has(aa.profile_names) ? aa.profile_names : ({})
        for (const mode in names) {
            const list = Fmt.list(names[mode])
            for (let i = 0; i < list.length; i++) out.push({ name: list[i], mode: mode })
        }
        return out
    }
    readonly property int unnamed: {
        if (!counts) return 0
        let total = 0
        for (const mode in counts) total += counts[mode]
        return total - profileRows.length
    }
    readonly property var service: Fmt.has(aa.service) ? aa.service : null

    sectionKey: "apparmor"
    title: qsTr("AppArmor")
    sectionTitle: qsTr("The AppArmor state")
    subtitle: qsTr("Mandatory access control: a profile limits what a program may read, write, execute and connect to.")

    NxBanner {
        visible: page.ready && !page.enabled_
        kind: "danger"
        title: qsTr("AppArmor is not enabled")
        message: page.aa.reason || qsTr("The kernel reports /sys/module/apparmor/parameters/enabled = N.")
    }

    GridLayout {
        Layout.fillWidth: true
        visible: page.ready && page.enabled_ && page.counts !== null
        columns: width >= 760 ? 5 : 3
        columnSpacing: Theme.spaceMd
        rowSpacing: Theme.spaceMd
        NxStatTile {
            Layout.fillWidth: true
            label: qsTr("Enforcing")
            value: page.counts ? String(page.counts.enforce) : ""
            detail: qsTr("violations are blocked")
            icon: "shield-check"
            tone: page.counts && page.counts.enforce > 0 ? "success" : "neutral"
        }
        NxStatTile {
            Layout.fillWidth: true
            label: qsTr("Complaining")
            value: page.counts ? String(page.counts.complain) : ""
            detail: qsTr("violations are only logged")
            icon: "eye"
        }
        NxStatTile {
            Layout.fillWidth: true
            label: qsTr("Kill")
            value: page.counts ? String(page.counts.kill) : ""
            detail: qsTr("violating process is killed")
            icon: "x-circle"
        }
        NxStatTile {
            Layout.fillWidth: true
            label: qsTr("Unconfined mode")
            value: page.counts ? String(page.counts.unconfined) : ""
            detail: qsTr("profile loaded, not restricting")
            icon: "lock-open"
        }
        NxStatTile {
            Layout.fillWidth: true
            label: qsTr("Confined processes")
            value: Fmt.has(page.aa.confined_processes) ? String(page.aa.confined_processes) : qsTr("—")
            detail: Fmt.has(page.aa.confined_processes) ? qsTr("running under a profile now") : qsTr("not determined")
            icon: "cpu"
        }
    }

    NxSection {
        visible: page.ready
        title: qsTr("State")
        NxRow {
            icon: page.enabled_ ? "shield-check" : "shield-off"
            iconColor: page.enabled_ ? Theme.success : Theme.danger
            title: qsTr("Kernel module")
            subtitle: qsTr("/sys/module/apparmor/parameters/enabled")
            NxBadge {
                text: page.enabled_ ? qsTr("Enabled") : qsTr("Not enabled")
                kind: page.enabled_ ? "success" : "danger"
            }
        }
        NxRow {
            visible: Fmt.has(page.aa.lsm)
            title: qsTr("Active security modules")
            subtitle: qsTr("/sys/kernel/security/lsm")
            value: Fmt.list(page.aa.lsm).filter(n => n !== "").join(", ") || qsTr("none reported")
            monoValue: true
        }
        NxRow {
            visible: page.enabled_
            title: qsTr("apparmor.service")
            subtitle: qsTr("Loads the profiles at boot.")
            value: !page.service ? qsTr("not reported")
                : page.service.active === null || page.service.active === undefined
                    ? qsTr("not determined — systemd is not running")
                : (page.service.active_state || (page.service.active ? "active" : "inactive")) + " · "
                  + (page.service.enabled_state || (page.service.enabled ? "enabled" : "disabled"))
        }
        NxRow {
            visible: page.enabled_ && page.counts === null
            icon: "alert-triangle"
            iconColor: Theme.warning
            title: qsTr("The profile list could not be read")
            subtitle: page.aa.reason || ""
        }
        NxRow {
            visible: page.enabled_ && page.counts !== null && page.counts.other > 0
            title: qsTr("Profiles in another mode")
            subtitle: qsTr("Modes other than enforce, complain, kill and unconfined.")
            value: page.counts ? String(page.counts.other) : ""
        }
    }

    NxSection {
        visible: page.ready && page.enabled_ && page.counts !== null
        title: qsTr("Profiles")
        description: page.unnamed > 0
            ? qsTr("%1 loaded profile(s) in enforce or complain mode. The tool does not list the names of the %2 profile(s) in other modes.")
                  .arg(page.profileRows.length).arg(page.unnamed)
            : qsTr("%1 loaded profile(s).").arg(page.profileRows.length)
        headerActions: [
            NxSearchField {
                id: profileSearch
                width: 240
                placeholderText: qsTr("Filter profiles")
            }
        ]
        TableCard {
            Layout.margins: Theme.spaceXs
            color: "transparent"
            border.width: 0
            maxVisibleRows: 14
            rows: page.profileRows
            filterText: profileSearch.text
            filterRoles: ["name", "mode"]
            sortRole: "name"
            emptyText: qsTr("No profile is loaded")
            columns: [
                { title: qsTr("Profile"), role: "name", fill: true, mono: true },
                { title: qsTr("Mode"), role: "mode", width: 140,
                  color: (v) => v === "enforce" ? Theme.success : v === "complain" ? Theme.warning : Theme.textSecondary }
            ]
        }
    }

    NxSection {
        visible: page.ready && page.enabled_ && Array.isArray(page.aa.processes)
        title: qsTr("Processes")
        description: qsTr("Processes and the profile each runs under.")
        TableCard {
            Layout.margins: Theme.spaceXs
            color: "transparent"
            border.width: 0
            rows: Fmt.list(page.aa.processes)
            sortRole: "profile"
            columns: [
                { title: qsTr("Process"), role: "process", fill: true },
                { title: qsTr("PID"), role: "pid", width: 90, align: Qt.AlignRight, mono: true },
                { title: qsTr("Profile"), role: "profile", fill: true, mono: true },
                { title: qsTr("Mode"), role: "mode", width: 120 }
            ]
        }
    }
    Note {
        visible: page.ready && page.enabled_ && !Array.isArray(page.aa.processes)
        text: qsTr("The status tool reports how many processes are confined, not which ones. "
                   + "In a terminal, “sudo aa-status” lists them and “sudo aa-unconfined” lists network-facing processes without a profile.")
    }
}
