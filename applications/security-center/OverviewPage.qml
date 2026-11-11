// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import "Fmt.js" as Fmt

// The states nexus-security-status reported, side by side. Nothing here is
// scored or weighted: a tile repeats one section's state and the evidence.
NxPage {
    id: page

    required property var center
    signal openPage(string key)
    signal actionRequested(string action)
    signal copyRequested(string format)
    signal saveRequested()

    readonly property var report: center.report
    readonly property bool waiting: !center.hasReport && center.loading
    readonly property var recommendations: Fmt.list(report.recommendations)
    // Sections whose check could not be performed, with the tool's reason.
    readonly property var undetermined: {
        const names = { firewall: qsTr("Firewall"), apparmor: qsTr("AppArmor"), encryption: qsTr("Disk encryption"),
                        secureboot: qsTr("Secure Boot"), updates: qsTr("Updates"), listening: qsTr("Listening ports"),
                        connections: qsTr("Connections"), services: qsTr("Services"), logins: qsTr("Logins"),
                        audit: qsTr("Audit"), hardening: qsTr("Hardening") }
        const out = []
        for (const key in names) {
            const s = report[key]
            if (Fmt.has(s) && s.available === false)
                out.push({ key: key, name: names[key], reason: s.reason || qsTr("The status tool gave no reason.") })
        }
        return out
    }

    function unavailable(s) { return Fmt.has(s) && s.available === false }
    function join(parts) { return parts.filter(p => p !== "").join(" · ") }

    title: qsTr("Security overview")
    subtitle: center.generated !== ""
        ? qsTr("State of this computer as reported by nexus-security-status at %1.").arg(center.localTime(center.generated))
        : qsTr("State of this computer as reported by nexus-security-status.")
    maxContentWidth: 1040

    actions: [
        NxButton {
            text: qsTr("Copy report")
            iconName: "copy"
            enabled: page.center.hasReport
            onClicked: copyMenu.popup(this, 0, height + Theme.spaceXs)
            Menu {
                id: copyMenu
                MenuItem { text: qsTr("Copy as text"); onTriggered: page.copyRequested("text") }
                MenuItem { text: qsTr("Copy as JSON"); onTriggered: page.copyRequested("json") }
            }
        },
        NxButton {
            text: qsTr("Save…")
            iconName: "save"
            enabled: page.center.hasReport
            onClicked: page.saveRequested()
        },
        NxButton {
            text: qsTr("Refresh")
            iconName: "refresh"
            kind: "primary"
            busy: page.center.loading
            onClicked: page.center.refresh()
        }
    ]

    NxBanner {
        visible: page.center.hasReport && page.center.error !== ""
        kind: "warning"
        title: qsTr("The last refresh failed — this is the earlier state")
        message: page.center.error
    }
    NxBanner {
        visible: page.center.hasReport && !page.center.privileged
        kind: "warning"
        title: qsTr("This report is incomplete")
        message: qsTr("The status tool ran without administrator rights, so it could not see other users' sockets, "
                      + "the AppArmor profile list, the firewall rules or the authentication log.")
    }
    NxEmptyState {
        Layout.fillWidth: true
        visible: !page.center.hasReport && !page.center.loading && page.center.error !== ""
        icon: "shield-off"
        tone: "danger"
        title: qsTr("The security state could not be read")
        message: page.center.error
        actionText: qsTr("Try again")
        onActionTriggered: page.center.refresh()
    }

    GridLayout {
        Layout.fillWidth: true
        visible: page.center.hasReport || page.waiting
        columns: width >= 820 ? 3 : width >= 540 ? 2 : 1
        columnSpacing: Theme.spaceLg
        rowSpacing: Theme.spaceLg

        StateTile {
            readonly property var s: page.report.firewall
            label: qsTr("Firewall")
            icon: "firewall"
            loading: page.waiting
            linkText: qsTr("Open Firewall")
            stateText: !Fmt.has(s) ? qsTr("Not checked") : page.unavailable(s) ? qsTr("Not determined")
                : s.active ? qsTr("Active") : qsTr("Not active")
            tone: !Fmt.has(s) || page.unavailable(s) ? "neutral" : s.active ? "success" : "danger"
            evidence: !Fmt.has(s) ? "" : page.unavailable(s) ? (s.reason || "")
                : s.active ? page.join([
                      qsTr("Inbound policy: %1").arg(s.input_policy || qsTr("not reported")),
                      qsTr("%n rule(s) loaded", "", s.rule_count || 0),
                      qsTr("%n opened port(s)", "", Fmt.list(s.allowed).length),
                      s.enabled_at_boot ? qsTr("starts at boot") : qsTr("does NOT start at boot")])
                : page.join([
                      qsTr("The table inet nexus_filter is not loaded: inbound connections are not filtered."),
                      s.enabled_at_boot ? qsTr("Enabled at boot") : qsTr("Not enabled at boot")])
            onClicked: page.openPage("firewall")
        }
        StateTile {
            readonly property var s: page.report.apparmor
            label: qsTr("AppArmor")
            icon: "shield-check"
            loading: page.waiting
            linkText: qsTr("Open AppArmor")
            stateText: !Fmt.has(s) ? qsTr("Not checked") : page.unavailable(s) ? qsTr("Not determined")
                : s.enabled ? qsTr("Enabled") : qsTr("Not enabled")
            tone: !Fmt.has(s) || page.unavailable(s) ? "neutral" : s.enabled ? "success" : "danger"
            evidence: !Fmt.has(s) ? "" : !s.enabled || page.unavailable(s) ? (s.reason || "")
                : Fmt.has(s.profiles) ? page.join([
                      qsTr("%n profile(s) enforcing", "", s.profiles.enforce),
                      qsTr("%1 complaining").arg(s.profiles.complain),
                      Fmt.has(s.confined_processes) ? qsTr("%n confined process(es)", "", s.confined_processes) : ""])
                : (s.reason || "")
            onClicked: page.openPage("apparmor")
        }
        StateTile {
            readonly property var s: page.report.encryption
            readonly property var root: Fmt.has(s) && Fmt.has(s.root) ? s.root : ({})
            label: qsTr("Disk encryption")
            icon: "disk-lock"
            loading: page.waiting
            linkText: qsTr("Open Encryption")
            stateText: !Fmt.has(s) ? qsTr("Not checked") : page.unavailable(s) ? qsTr("Not determined")
                : s.live_session ? qsTr("Live session")
                : Fmt.tri(root.encrypted, qsTr("System disk encrypted"), qsTr("Not encrypted"), qsTr("Not determined"))
            tone: !Fmt.has(s) || page.unavailable(s) || s.live_session ? "neutral"
                : Fmt.tri(root.encrypted, "success", "warning", "neutral")
            evidence: !Fmt.has(s) ? "" : page.unavailable(s) ? (s.reason || "")
                : page.join([
                      s.live_session ? qsTr("Started from live media; nothing is installed on a disk yet.") : "",
                      Fmt.has(root.device) ? qsTr("/ is on %1").arg(root.device)
                                           : qsTr("lsblk shows no block device mounted at /, so the tool cannot tell whether it is encrypted."),
                      qsTr("%n LUKS device(s)", "", Fmt.list(s.luks_devices).length),
                      Fmt.has(s.swap) && s.swap.present
                          ? Fmt.tri(s.swap.encrypted, qsTr("swap encrypted"), qsTr("swap NOT encrypted"), qsTr("swap encryption not determined"))
                          : qsTr("no swap")])
            onClicked: page.openPage("encryption")
        }
        StateTile {
            readonly property var s: page.report.secureboot
            label: qsTr("Secure Boot")
            icon: "key"
            loading: page.waiting
            linkText: qsTr("Open Secure Boot")
            stateText: !Fmt.has(s) ? qsTr("Not checked") : page.unavailable(s) ? qsTr("Not determined")
                : s.firmware === "bios" ? qsTr("Not supported")
                : Fmt.tri(s.enabled, qsTr("Enabled"), qsTr("Disabled"), qsTr("Not determined"))
            tone: !Fmt.has(s) || page.unavailable(s) || s.firmware === "bios" ? "neutral"
                : Fmt.tri(s.enabled, "success", "warning", "neutral")
            evidence: !Fmt.has(s) ? "" : page.unavailable(s) || s.firmware === "bios" ? (s.reason || "")
                : page.join([
                      qsTr("UEFI firmware"),
                      s.enabled === null ? qsTr("the firmware does not expose the SecureBoot variable") : "",
                      s.setup_mode === true ? qsTr("firmware is in setup mode") : "",
                      Fmt.has(s.kernel_lockdown) ? qsTr("kernel lockdown: %1").arg(s.kernel_lockdown) : "",
                      s.boot_loader_refresh_failed ? qsTr("boot loader update failed") : ""])
            onClicked: page.openPage("secureboot")
        }
        StateTile {
            readonly property var s: page.report.updates
            label: qsTr("Updates")
            icon: "arrow-up-circle"
            loading: page.waiting
            linkText: qsTr("Open Updates")
            stateText: !Fmt.has(s) ? qsTr("Not checked") : page.unavailable(s) ? qsTr("Not determined")
                : s.security > 0 ? qsTr("%n security update(s)", "", s.security)
                : s.upgradable > 0 ? qsTr("%n update(s) waiting", "", s.upgradable)
                : !s.lists_present ? qsTr("No update information") : qsTr("No updates waiting")
            tone: !Fmt.has(s) || page.unavailable(s) ? "neutral"
                : s.security > 0 ? "danger" : s.upgradable > 0 || !s.lists_present || s.reboot_required ? "warning" : "success"
            evidence: !Fmt.has(s) ? "" : page.unavailable(s) ? (s.reason || "")
                : page.join([
                      s.security > 0 ? qsTr("%n update(s) in total", "", s.upgradable) : "",
                      s.lists_present ? qsTr("package lists refreshed %1 (%2 days ago)")
                                            .arg(page.center.localTime(s.last_refresh || "")).arg(s.last_refresh_age_days)
                                      : qsTr("the package lists have never been downloaded"),
                      s.reboot_required ? qsTr("restart required") : ""])
            onClicked: page.openPage("updates")
        }
        StateTile {
            readonly property var s: page.report.listening
            label: qsTr("Network exposure")
            icon: "globe"
            loading: page.waiting
            linkText: qsTr("Open Network exposure")
            stateText: !Fmt.has(s) ? qsTr("Not checked") : page.unavailable(s) ? qsTr("Not determined")
                : qsTr("%n listening socket(s)", "", s.count)
            tone: "neutral"
            evidence: !Fmt.has(s) ? "" : page.unavailable(s) ? (s.reason || "")
                : page.join([
                      qsTr("%1 not bound to loopback").arg(s.exposed_count),
                      Fmt.has(page.report.connections) && page.report.connections.available !== false
                          ? qsTr("%n established connection(s)", "", page.report.connections.count) : ""])
            onClicked: page.openPage("network")
        }
    }

    NxSection {
        visible: page.center.hasReport
        title: qsTr("Recommendations")
        description: qsTr("Findings of nexus-security-status, most severe first.")

        NxRow {
            visible: page.recommendations.length === 0
            icon: "check-circle"
            iconColor: Theme.success
            title: qsTr("No recommendations")
            subtitle: page.undetermined.length > 0
                ? qsTr("The tool found nothing to act on in the checks it could perform. Some checks were not possible; see below.")
                : qsTr("The tool found nothing to act on.")
        }
        Repeater {
            model: page.recommendations
            NxRow {
                id: rec
                required property var modelData
                readonly property string severity: modelData.severity || ""
                readonly property string action: modelData.action || ""
                icon: severity === "high" ? "shield-alert" : severity === "medium" ? "alert-triangle" : "info"
                iconColor: severity === "high" ? Theme.danger : severity === "medium" ? Theme.warning : Theme.textSecondary
                title: modelData.title || ""
                subtitle: modelData.detail || ""

                NxBadge {
                    anchors.verticalCenter: parent.verticalCenter
                    text: rec.severity === "high" ? qsTr("High") : rec.severity === "medium" ? qsTr("Medium")
                        : rec.severity === "low" ? qsTr("Low") : rec.severity
                    kind: rec.severity === "high" ? "danger" : rec.severity === "medium" ? "warning" : "neutral"
                }
                NxButton {
                    visible: rec.action === "firewall-enable" || (rec.action === "open-updater" && page.center.updaterInstalled)
                    text: rec.action === "firewall-enable" ? qsTr("Open Firewall") : qsTr("Open Updater")
                    onClicked: page.actionRequested(rec.action)
                }
            }
        }
    }

    NxSection {
        visible: page.undetermined.length > 0
        title: qsTr("Checks that could not be performed")
        description: qsTr("These sections have no state on this system; the reason is the one the tool gave.")
        Repeater {
            model: page.undetermined
            NxRow {
                required property var modelData
                icon: "help"
                title: modelData.name
                subtitle: modelData.reason
            }
        }
    }
}
