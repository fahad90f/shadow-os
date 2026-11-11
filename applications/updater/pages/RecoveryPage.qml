// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.updater

// Recovery after a failed or interrupted update: what state dpkg's database
// is in, and PackageKit's "repair system" operation to fix it.
NxPage {
    id: page

    required property QtObject updater
    readonly property var health: updater.health
    readonly property bool known: updater.healthKnown
    readonly property var audit: health.audit || ({})
    readonly property var pendingUpdates: health.interruptedUpdates || []
    readonly property var broken: health.brokenPackages || []
    readonly property bool repairJobShown: updater.job !== null && updater.job.kind === "repair"

    signal copyRequested(string text)

    title: qsTr("Recovery")
    subtitle: qsTr("Checks whether an earlier package operation was interrupted and repairs the package system if it was.")
    actions: [
        NxButton {
            text: qsTr("Check again")
            iconName: "refresh"
            busy: page.updater.healthChecking
            onClicked: page.updater.refreshHealth()
        }
    ]

    NxBanner {
        visible: !page.known && page.updater.healthChecking
        kind: "info"
        message: qsTr("Checking the package database with dpkg…")
    }
    NxBanner {
        visible: page.known && page.health.dpkgAvailable === false
        kind: "danger"
        title: qsTr("The package database cannot be checked")
        message: page.health.error || ""
    }
    NxBanner {
        visible: page.known && page.updater.healthy
        kind: "success"
        title: qsTr("The package system is consistent")
        message: qsTr("dpkg reports no interrupted operation and no half-installed package. Nothing needs to be repaired.")
    }
    NxBanner {
        visible: page.known && page.health.dpkgAvailable !== false && !page.updater.healthy
        kind: "danger"
        title: qsTr("The package system needs repair")
        message: qsTr("A package operation did not finish. Installing software and updates can fail until this is repaired — use “Repair package system” below.")
    }

    NxSection {
        title: qsTr("Checks")
        description: page.known && page.health.checkedAt ? qsTr("Last run: %1").arg(Qt.formatDateTime(new Date(page.health.checkedAt), Qt.DefaultLocaleShortDate)) : ""
        NxRow {
            icon: "file-search"
            iconColor: !page.known ? Theme.textSecondary : (page.audit.output || "") === "" ? Theme.success : Theme.danger
            title: qsTr("dpkg audit")
            subtitle: qsTr("dpkg --audit — packages that are only partly installed or have inconsistent control data")
            NxBadge {
                text: !page.known ? qsTr("Checking") : (page.audit.output || "") === "" ? qsTr("No problems reported") : qsTr("Problems reported")
                kind: !page.known ? "neutral" : (page.audit.output || "") === "" ? "success" : "danger"
            }
        }
        NxRow {
            icon: "archive"
            iconColor: !page.known ? Theme.textSecondary : page.pendingUpdates.length === 0 ? Theme.success : Theme.danger
            title: qsTr("Interrupted dpkg run")
            subtitle: qsTr("/var/lib/dpkg/updates — journal entries left behind when dpkg was stopped in the middle of an operation")
            NxBadge {
                text: !page.known ? qsTr("Checking") : page.pendingUpdates.length === 0 ? qsTr("None") : qsTr("%1 pending").arg(page.pendingUpdates.length)
                kind: !page.known ? "neutral" : page.pendingUpdates.length === 0 ? "success" : "danger"
            }
        }
        NxRow {
            icon: "package"
            iconColor: !page.known ? Theme.textSecondary : page.broken.length === 0 ? Theme.success : Theme.danger
            title: qsTr("Half-installed packages")
            subtitle: (page.health.queryError || "") !== ""
                ? qsTr("dpkg-query could not list the packages:") + "\n" + page.health.queryError
                : qsTr("dpkg-query — packages unpacked but not configured, half-installed, or waiting for triggers")
            NxBadge {
                text: !page.known ? qsTr("Checking") : page.broken.length === 0 ? qsTr("None") : String(page.broken.length)
                kind: !page.known ? "neutral" : page.broken.length === 0 ? "success" : "danger"
            }
        }
    }

    NxSection {
        title: qsTr("Report from dpkg --audit")
        visible: (page.audit.output || "") !== ""
        Item {
            Layout.fillWidth: true
            implicitHeight: auditText.implicitHeight + Theme.spaceLg * 2
            TextEdit {
                id: auditText
                anchors.fill: parent
                anchors.margins: Theme.spaceLg
                text: page.audit.output || ""
                textFormat: TextEdit.PlainText
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.Wrap
                color: Theme.textPrimary
                selectionColor: Theme.accentFillStrong
                selectedTextColor: Theme.textPrimary
                font.family: Theme.fontMono
                font.pixelSize: Theme.fontSizeSmall
                Accessible.name: qsTr("Report from dpkg --audit")
            }
        }
    }

    NxSection {
        title: qsTr("Packages in an intermediate state")
        visible: page.broken.length > 0
        Repeater {
            model: page.broken
            NxRow {
                id: brokenRow
                required property var modelData
                icon: "alert-triangle"
                iconColor: Theme.warning
                title: modelData.package + (modelData.architecture ? ":" + modelData.architecture : "")
                subtitle: modelData.meaning
                value: modelData.version
                monoValue: true
                NxBadge { text: brokenRow.modelData.state; kind: "warning" }
            }
        }
    }

    PkJobPanel {
        job: page.repairJobShown ? page.updater.job : null
        onCloseRequested: page.updater.dismissJob()
        onCopyRequested: (text) => page.copyRequested(text)
    }

    NxSection {
        title: qsTr("Repair package system")
        description: qsTr("What the repair does, before you start it:")
        NxRow {
            icon: "wrench"
            title: qsTr("Uses PackageKit's “repair system” operation")
            subtitle: qsTr("PackageKit asks the APT back-end to bring the package system back to a consistent state: packages whose installation was interrupted are completed and broken dependencies are corrected.")
        }
        NxRow {
            icon: "list"
            title: qsTr("Shows the changes first")
            subtitle: qsTr("The repair is simulated before anything happens. If packages would have to be removed, you see the list and can cancel.")
        }
        NxRow {
            icon: "key"
            title: qsTr("Needs administrator authorisation")
            subtitle: qsTr("PackageKit asks for it through polkit. SHADOW Updater itself never runs as root.")
        }
        NxRow {
            icon: "life-buoy"
            title: qsTr("Repair package system")
            subtitle: !page.updater.service.available ? qsTr("Not possible right now: the PackageKit service is not reachable.")
                : page.updater.busy ? qsTr("Not possible right now: another operation is in progress.")
                : page.known && page.updater.healthy ? qsTr("No problem was found; running the repair is not necessary.")
                : qsTr("Start the repair. You can follow every step and copy the log afterwards.")
            NxButton {
                text: qsTr("Repair package system…")
                kind: page.known && !page.updater.healthy ? "primary" : "secondary"
                iconName: "wrench"
                enabled: page.updater.service.available && !page.updater.busy
                onClicked: repairDialog.open()
            }
        }
    }

    NxConfirmDialog {
        id: repairDialog
        title: qsTr("Repair the package system?")
        message: qsTr("PackageKit will complete interrupted package installations and correct broken dependencies. This can install or remove packages; the changes are simulated first and you are asked again before anything is removed. Administrator authorisation is required.")
        confirmText: qsTr("Start repair")
        onConfirmed: page.updater.repair()
    }
}
