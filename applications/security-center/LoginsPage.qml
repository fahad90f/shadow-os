// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.nexus.ui
import "Fmt.js" as Fmt

// Sections "logins" (sessions, authentication log of the last 7 days) and
// "audit" (auditd state and event counts per rule key).
SectionPage {
    id: page

    readonly property var lg: ready ? section : ({})
    readonly property var sessions: Fmt.list(lg.sessions)
    readonly property var journalProblem: Fmt.has(lg.journal) && lg.journal.available === false ? lg.journal : null
    readonly property var au: center.report.audit
    readonly property bool auLoaded: Fmt.has(au)
    readonly property bool auReady: auLoaded && au.available !== false
    readonly property var auService: auLoaded && Fmt.has(au.service) ? au.service : ({})
    readonly property var eventRows: {
        const out = []
        if (auLoaded && Fmt.has(au.events_by_key_7d))
            for (const key in au.events_by_key_7d) out.push({ key: key, count: au.events_by_key_7d[key] })
        return out
    }

    function withTime(rows) {
        return Fmt.list(rows).map(r => Object.assign({}, r, { when: page.center.localTime(r.time || "") }))
    }

    sectionKey: "logins"
    title: qsTr("Logins and audit")
    sectionTitle: qsTr("The login activity")
    subtitle: qsTr("Who is logged in, authentication activity of the last 7 days, and the state of the audit system.")

    NxSection {
        visible: page.ready
        title: qsTr("Current sessions")
        description: qsTr("From loginctl.")
        NxRow {
            visible: page.sessions.length === 0
            icon: "user"
            title: qsTr("No login session is reported")
            subtitle: qsTr("systemd-logind lists no session (or is not running).")
        }
        Repeater {
            model: page.sessions
            NxRow {
                required property var modelData
                icon: "user"
                title: modelData.user || qsTr("unknown user")
                subtitle: [modelData.class ? qsTr("class %1").arg(modelData.class) : "",
                           modelData.seat ? qsTr("seat %1").arg(modelData.seat) : "",
                           modelData.tty ? modelData.tty : ""].filter(p => p !== "").join(" · ")
                value: qsTr("session %1").arg(modelData.id)
            }
        }
    }

    NxBanner {
        visible: page.ready && page.journalProblem !== null
        kind: "warning"
        title: qsTr("The authentication log could not be read")
        message: page.journalProblem ? (page.journalProblem.reason || "") : ""
    }

    NxSection {
        visible: page.ready && page.journalProblem === null
        title: qsTr("Failed authentication attempts")
        description: Fmt.has(page.lg.failed_count_7d)
            ? (page.lg.failed_count_7d > Fmt.list(page.lg.failed).length
                ? qsTr("%1 in the last 7 days; the most recent %2 are listed.").arg(page.lg.failed_count_7d).arg(Fmt.list(page.lg.failed).length)
                : qsTr("%1 in the last 7 days (journal, facilities auth and authpriv).").arg(page.lg.failed_count_7d))
            : ""
        TableCard {
            Layout.margins: Theme.spaceXs
            color: "transparent"
            border.width: 0
            maxVisibleRows: 10
            rows: page.withTime(page.lg.failed)
            sortRole: "time"
            sortAscending: false
            emptyText: qsTr("No failed attempt in the last 7 days")
            columns: [
                { title: qsTr("Time"), role: "when", width: 150, mono: true, sortable: false },
                { title: qsTr("User"), role: "user", width: 130 },
                { title: qsTr("From"), role: "source", width: 150, mono: true },
                { title: qsTr("Service"), role: "service", width: 110 },
                { title: qsTr("Log message"), role: "message", fill: true }
            ]
        }
    }

    NxSection {
        visible: page.ready && page.journalProblem === null
        title: qsTr("Recent logins")
        description: qsTr("Sessions opened in the last 7 days (the most recent 50), without cron and systemd user managers.")
        TableCard {
            Layout.margins: Theme.spaceXs
            color: "transparent"
            border.width: 0
            maxVisibleRows: 10
            rows: page.withTime(page.lg.recent)
            sortRole: "time"
            sortAscending: false
            emptyText: qsTr("No session was opened in the last 7 days")
            columns: [
                { title: qsTr("Time"), role: "when", width: 170, mono: true, sortable: false },
                { title: qsTr("User"), role: "user", fill: true },
                { title: qsTr("Through"), role: "service", fill: true }
            ]
        }
    }

    NxSection {
        visible: page.auLoaded
        title: qsTr("Audit system")
        description: qsTr("auditd records security-relevant events: identity, privilege and kernel module changes.")
        NxRow {
            visible: !page.auReady
            icon: "alert-triangle"
            iconColor: Theme.warning
            title: qsTr("The audit state could not be determined")
            subtitle: page.auLoaded ? (page.au.reason || qsTr("The status tool gave no reason.")) : ""
        }
        NxRow {
            title: qsTr("auditd.service")
            value: page.auService.active === null || page.auService.active === undefined
                ? qsTr("not determined — systemd is not running")
                : (page.auService.active_state || (page.auService.active ? "active" : "inactive")) + " · "
                  + (page.auService.enabled_state || (page.auService.enabled ? "enabled" : "disabled"))
            NxBadge {
                visible: page.auService.active === true || page.auService.active === false
                text: page.auService.active ? qsTr("Running") : qsTr("Not running")
                kind: page.auService.active ? "success" : "warning"
            }
        }
        NxRow {
            visible: page.auReady
            title: qsTr("Kernel auditing")
            subtitle: qsTr("“enabled” flag of auditctl -s: 0 off, 1 on, 2 on and locked until restart.")
            value: !Fmt.has(page.au.enabled) ? qsTr("not reported")
                : page.au.enabled === 0 ? qsTr("0 — off") : page.au.enabled === 1 ? qsTr("1 — on")
                : page.au.enabled === 2 ? qsTr("2 — on, locked") : String(page.au.enabled)
        }
        NxRow {
            visible: page.auReady
            title: qsTr("Loaded audit rules")
            value: Fmt.has(page.au.rules) ? String(page.au.rules) : qsTr("not determined")
        }
        NxRow {
            visible: page.auReady
            title: qsTr("Lost events")
            subtitle: qsTr("Events the kernel dropped because the audit queue was full; backlog is the current queue length.")
            value: (Fmt.has(page.au.lost) ? String(page.au.lost) : "—") + qsTr(" lost · backlog ") + (Fmt.has(page.au.backlog) ? String(page.au.backlog) : "—")
        }
    }

    NxSection {
        visible: page.auReady
        title: qsTr("Audit events by rule")
        description: qsTr("Events of the last 7 days, counted per rule key (aureport --key --summary).")
        NxRow {
            visible: !Fmt.has(page.au ? page.au.events_by_key_7d : null)
            icon: "info"
            title: qsTr("No event summary is available")
            subtitle: qsTr("aureport is not installed or could not read the audit log (/var/log/audit).")
        }
        TableCard {
            visible: page.auReady && Fmt.has(page.au.events_by_key_7d)
            Layout.margins: Theme.spaceXs
            color: "transparent"
            border.width: 0
            maxVisibleRows: 12
            rows: page.eventRows
            sortRole: "count"
            sortAscending: false
            emptyText: qsTr("No keyed event in the last 7 days")
            columns: [
                { title: qsTr("Rule key"), role: "key", fill: true, mono: true },
                { title: qsTr("Events"), role: "count", width: 120, align: Qt.AlignRight, mono: true }
            ]
        }
    }
    Note {
        visible: page.auReady && !Array.isArray(page.au.recent_events)
        text: qsTr("The status tool reports audit events as counts per rule, not as individual events. In a terminal: sudo ausearch -ts recent -i")
    }
    NxSection {
        visible: page.auReady && Array.isArray(page.au.recent_events)
        title: qsTr("Recent audit events")
        TableCard {
            Layout.margins: Theme.spaceXs
            color: "transparent"
            border.width: 0
            rows: page.auReady ? page.withTime(page.au.recent_events) : []
            sortRole: "time"
            sortAscending: false
            columns: [
                { title: qsTr("Time"), role: "when", width: 170, mono: true, sortable: false },
                { title: qsTr("Rule key"), role: "key", width: 160, mono: true },
                { title: qsTr("Type"), role: "type", width: 160 },
                { title: qsTr("Summary"), role: "summary", fill: true }
            ]
        }
    }
}
