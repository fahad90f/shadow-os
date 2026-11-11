// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.nexus.ui
import "Fmt.js" as Fmt

// Section "hardening": kernel settings compared with the NEXUS baseline.
SectionPage {
    id: page

    readonly property var hd: ready ? section : ({})
    readonly property var sysctl: Fmt.list(hd.sysctl)
    readonly property var cmdline: Fmt.list(hd.cmdline)
    readonly property var mitigations: Fmt.list(hd.mitigations)
    readonly property var baselineMissing: Fmt.has(hd.sysctl_baseline) && hd.sysctl_baseline.available === false ? hd.sysctl_baseline : null
    readonly property int sysctlDrift: sysctl.filter(s => !s.ok).length
    readonly property int cmdlineMissing: cmdline.filter(c => !c.ok).length
    readonly property int vulnerable: mitigations.filter(m => m.vulnerable).length

    sectionKey: "hardening"
    title: qsTr("Hardening")
    sectionTitle: qsTr("The hardening state")
    subtitle: qsTr("Kernel settings of the running system next to the values the NEXUS baseline expects.")

    NxSection {
        visible: page.ready
        title: qsTr("Kernel")
        NxRow {
            icon: "terminal"
            title: qsTr("Running kernel")
            value: page.hd.kernel || qsTr("not reported")
            monoValue: true
        }
        NxRow {
            title: qsTr("Taint flags")
            subtitle: page.hd.tainted > 0
                ? qsTr("Non-zero: an out-of-tree, unsigned or proprietary module was loaded, or the kernel reported a fault.")
                : qsTr("/proc/sys/kernel/tainted")
            value: Fmt.has(page.hd.tainted) ? String(page.hd.tainted) : qsTr("not determined")
            monoValue: true
        }
    }

    NxSection {
        visible: page.ready
        title: qsTr("Kernel parameters (sysctl)")
        description: page.baselineMissing ? ""
            : page.sysctlDrift > 0 ? qsTr("%1 of %2 settings differ from the baseline in /usr/lib/sysctl.d/60-nexus-hardening.conf.").arg(page.sysctlDrift).arg(page.sysctl.length)
            : qsTr("All %1 settings match the baseline in /usr/lib/sysctl.d/60-nexus-hardening.conf.").arg(page.sysctl.length)
        NxRow {
            visible: page.baselineMissing !== null
            icon: "alert-triangle"
            iconColor: Theme.warning
            title: qsTr("There is no baseline to compare with")
            subtitle: page.baselineMissing ? (page.baselineMissing.reason || "") : ""
        }
        TableCard {
            visible: page.baselineMissing === null
            Layout.margins: Theme.spaceXs
            color: "transparent"
            border.width: 0
            maxVisibleRows: 16
            // "actual" is null when the kernel does not have the setting.
            rows: page.sysctl.map(s => Object.assign({}, s, { actualText: Fmt.has(s.actual) ? s.actual : qsTr("not present") }))
            sortRole: "ok"
            emptyText: qsTr("The baseline lists no settings")
            columns: [
                { title: qsTr("Setting"), role: "key", fill: true, mono: true },
                { title: qsTr("Expected"), role: "expected", width: 150, mono: true },
                { title: qsTr("Actual"), role: "actualText", width: 150, mono: true,
                  color: (v, row) => row.ok ? Theme.textSecondary : Theme.danger },
                { title: qsTr("Result"), role: "ok", width: 120,
                  format: (v) => v ? qsTr("Matches") : qsTr("Differs"), color: (v) => v ? Theme.success : Theme.danger }
            ]
        }
    }

    NxSection {
        visible: page.ready
        title: qsTr("Boot parameters")
        description: page.cmdline.length === 0
            ? "" : page.cmdlineMissing > 0
                ? qsTr("%1 of %2 hardening parameters are missing from the running kernel's command line.").arg(page.cmdlineMissing).arg(page.cmdline.length)
                : qsTr("All %1 hardening parameters are in effect.").arg(page.cmdline.length)
        NxRow {
            visible: page.cmdline.length === 0
            icon: "info"
            title: qsTr("No boot-parameter baseline was found")
            subtitle: qsTr("The tool reads the expected parameters from /etc/default/grub.d/20-nexus-hardening.cfg (package nexus-hardening); that file is missing or lists none.")
        }
        TableCard {
            visible: page.cmdline.length > 0
            Layout.margins: Theme.spaceXs
            color: "transparent"
            border.width: 0
            maxVisibleRows: 16
            rows: page.cmdline
            sortRole: "ok"
            columns: [
                { title: qsTr("Expected parameter"), role: "parameter", fill: true, mono: true },
                { title: qsTr("On the running kernel's command line"), role: "ok", width: 300,
                  format: (v) => v ? qsTr("Present") : qsTr("Missing"), color: (v) => v ? Theme.success : Theme.danger }
            ]
        }
    }

    NxSection {
        visible: page.ready
        title: qsTr("CPU vulnerability mitigations")
        description: page.mitigations.length === 0 ? ""
            : page.vulnerable > 0 ? qsTr("The kernel reports %1 vulnerability(ies) without mitigation.").arg(page.vulnerable)
            : qsTr("The kernel reports no unmitigated vulnerability (/sys/devices/system/cpu/vulnerabilities).")
        NxRow {
            visible: page.mitigations.length === 0
            icon: "info"
            title: qsTr("The kernel does not report CPU vulnerabilities")
            subtitle: qsTr("/sys/devices/system/cpu/vulnerabilities is empty or missing.")
        }
        TableCard {
            visible: page.mitigations.length > 0
            Layout.margins: Theme.spaceXs
            color: "transparent"
            border.width: 0
            maxVisibleRows: 30
            // Unmitigated ones first.
            rows: page.mitigations.filter(m => m.vulnerable).concat(page.mitigations.filter(m => !m.vulnerable))
            columns: [
                { title: qsTr("Vulnerability"), role: "name", width: 240, mono: true },
                { title: qsTr("Kernel status"), role: "status", fill: true,
                  color: (v, row) => row.vulnerable ? Theme.danger : v === "Not affected" ? Theme.textMuted : Theme.textSecondary }
            ]
        }
    }

    NxSection {
        visible: page.ready && Array.isArray(page.hd.modules)
        title: qsTr("Kernel modules")
        TableCard {
            Layout.margins: Theme.spaceXs
            color: "transparent"
            border.width: 0
            rows: Fmt.list(page.hd.modules)
            columns: [
                { title: qsTr("Module"), role: "module", fill: true, mono: true },
                { title: qsTr("Expected"), role: "expected", width: 180 },
                { title: qsTr("Actual"), role: "actual", width: 180 },
                { title: qsTr("Result"), role: "ok", width: 120,
                  format: (v) => v ? qsTr("Matches") : qsTr("Differs"), color: (v) => v ? Theme.success : Theme.danger }
            ]
        }
    }
    Note {
        visible: page.ready && !Array.isArray(page.hd.modules)
        text: qsTr("The module blacklist of the baseline (/etc/modprobe.d) is not checked by the status tool, so it is not shown here.")
    }
}
