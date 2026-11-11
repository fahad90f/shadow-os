// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.updater

PkListPage {
    id: page

    // The Updater back-end
    required property QtObject updater
    readonly property QtObject checker: updater.updates
    readonly property QtObject service: updater.service
    readonly property bool working: updater.busy || checker.busy
    readonly property bool canRestart: updater.canReboot === "yes" || updater.canReboot === "challenge"

    signal openPage(string key)
    signal restartRequested()
    signal copyRequested(string text)

    function ago(date) {
        if (!date || isNaN(date.getTime())) return ""
        const s = Math.max(0, Math.round((Date.now() - date.getTime()) / 1000))
        if (s < 90) return qsTr("just now")
        if (s < 3600) return qsTr("%1 min ago").arg(Math.round(s / 60))
        if (s < 86400) return qsTr("%1 h ago").arg(Math.round(s / 3600))
        return qsTr("%1 d ago").arg(Math.round(s / 86400))
    }

    title: qsTr("Updates")
    subtitle: checker.state === "refreshing" ? qsTr("Refreshing the software lists…")
        : checker.state === "loading" ? qsTr("Reading the list of updates…")
        : checker.state === "ready" && checker.count === 0 ? qsTr("No updates are available for this system.")
        : checker.state === "ready" && checker.count === 1 ? qsTr("1 update is available.")
        : checker.state === "ready" ? qsTr("%1 updates are available.").arg(checker.count)
        : checker.state === "error" ? qsTr("The list of updates is not available.")
        : qsTr("Updates have not been checked yet.")
    actions: [
        NxButton {
            text: qsTr("Check for updates")
            iconName: "refresh"
            kind: page.checker.count > 0 ? "secondary" : "primary"
            busy: page.checker.busy
            enabled: page.service.available && !page.working
            onClicked: page.updater.checkForUpdates()
        }
    ]

    model: checker.model
    sectionProperty: "group"
    sectionDelegate: PkUpdateSection {
        width: page.contentWidth
        checker: page.checker
    }
    delegate: PkUpdateRow {
        width: page.contentWidth
        checker: page.checker
        selectable: !page.working
        // The list is being read again: what is shown may be about to change.
        opacity: page.checker.busy ? 0.45 : 1
    }

    // ---- header: state, figures, actions --------------------------------------
    PkServiceBanner {
        service: page.service
        consequence: qsTr("Updates cannot be checked or installed until it is available.")
    }

    NxBanner {
        visible: page.checker.refreshError !== ""
        kind: "danger"
        title: qsTr("The software lists could not be refreshed")
        message: qsTr("The updates shown are from the last successful refresh and may be out of date. PackageKit reported:")
                 + "\n" + page.checker.refreshError
        actionText: page.working ? "" : qsTr("Try again")
        onActionTriggered: page.updater.checkForUpdates()
    }

    NxBanner {
        visible: page.service.available && page.checker.state === "error"
        kind: "danger"
        title: qsTr("The list of updates could not be read")
        message: qsTr("PackageKit reported:") + "\n" + page.checker.error
    }

    NxBanner {
        visible: page.updater.healthKnown && !page.updater.healthy
        kind: "danger"
        title: qsTr("The package system needs repair")
        message: qsTr("An earlier package operation was interrupted or left packages half-installed. Updates can fail until this is repaired.")
        actionText: qsTr("Open recovery")
        onActionTriggered: page.openPage("recovery")
    }

    NxBanner {
        visible: page.updater.restartNeeded
        kind: "warning"
        title: page.updater.restartLevel === "system" ? qsTr("Restart the computer to finish updating")
             : page.updater.restartLevel === "session" ? qsTr("Log out and back in to finish updating")
             : qsTr("Restart the updated applications")
        message: page.updater.restartLevel === "system" && !page.canRestart && page.updater.canRebootError !== ""
            ? qsTr("%1 Restarting from here is not possible:").arg(page.updater.restartReason) + "\n" + page.updater.canRebootError
            : page.updater.restartReason
        actionText: page.updater.restartLevel === "system" && page.canRestart && !page.updater.busy ? qsTr("Restart now…") : ""
        onActionTriggered: page.restartRequested()
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spaceLg

        NxStatTile {
            Layout.fillWidth: true
            label: qsTr("Available updates")
            icon: "arrow-up-circle"
            loading: page.checker.busy && page.checker.state !== "ready"
            value: page.checker.state === "ready" ? String(page.checker.count) : "—"
            tone: page.checker.state === "ready" && page.checker.count > 0 ? "accent" : "neutral"
            detail: page.checker.state === "ready"
                ? (page.checker.blockedCount > 0 ? qsTr("%1 more held back").arg(page.checker.blockedCount)
                   : page.checker.count === 0 ? qsTr("Up to date") : qsTr("Ready to install"))
                : page.checker.state === "error" ? qsTr("Not available") : qsTr("Not checked yet")
        }
        NxStatTile {
            Layout.fillWidth: true
            label: qsTr("Security updates")
            icon: page.checker.securityCount > 0 ? "shield-alert" : "shield-check"
            loading: page.checker.busy && page.checker.state !== "ready"
            value: page.checker.state === "ready" ? String(page.checker.securityCount) : "—"
            tone: page.checker.state !== "ready" ? "neutral" : page.checker.securityCount > 0 ? "danger" : "success"
            detail: page.checker.state !== "ready" ? qsTr("Unknown")
                : page.checker.securityCount > 0 ? qsTr("Install these first") : qsTr("None pending")
        }
        NxStatTile {
            Layout.fillWidth: true
            label: qsTr("Download size")
            icon: "download"
            loading: page.checker.busy && page.checker.state !== "ready"
            value: page.checker.state === "ready" && page.checker.sizesKnown ? Theme.formatBytes(page.checker.totalSize) : "—"
            detail: page.checker.state !== "ready" ? qsTr("Unknown")
                : page.checker.sizesKnown ? qsTr("For all updates") : qsTr("Not reported by PackageKit")
        }
        NxStatTile {
            Layout.fillWidth: true
            label: qsTr("Last checked")
            icon: "clock"
            value: page.checker.lastChecked.getTime() > 0 ? page.ago(page.checker.lastChecked) : "—"
            detail: page.checker.lastRefreshed.getTime() > 0
                ? qsTr("Lists refreshed %1").arg(page.ago(page.checker.lastRefreshed))
                : page.checker.state === "ready" ? qsTr("Refresh time not known") : qsTr("Not checked yet")
        }
    }

    RowLayout {
        Layout.fillWidth: true
        visible: page.checker.busy && page.updater.job === null
        spacing: Theme.spaceMd
        ProgressBar {
            Layout.fillWidth: true
            indeterminate: page.checker.percentage < 0
            value: page.checker.percentage >= 0 ? page.checker.percentage / 100 : 0
        }
        PkText {
            text: page.checker.statusText
            color: Theme.textSecondary
            font.pixelSize: Theme.fontSizeSmall
        }
        NxButton {
            text: qsTr("Cancel")
            kind: "ghost"
            onClicked: page.checker.cancel()
        }
    }

    PkJobPanel {
        job: page.updater.job
        onCloseRequested: page.updater.dismissJob()
        onCopyRequested: (text) => page.copyRequested(text)
        onRepairRequested: page.openPage("recovery")
    }

    NxCard {
        Layout.fillWidth: true
        visible: page.checker.state === "ready" && page.checker.count > 0
        ColumnLayout {
            width: parent.width
            spacing: Theme.spaceMd
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceMd
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("%1 of %2 selected").arg(page.checker.model.selectedCount).arg(page.checker.count)
                        color: Theme.textPrimary
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeBody
                        font.weight: Theme.weightSemibold
                        elide: Text.ElideRight
                    }
                    Text {
                        Layout.fillWidth: true
                        text: page.checker.sizesKnown
                            ? qsTr("Download for the selection: %1").arg(Theme.formatBytes(page.checker.model.selectedSize))
                            : qsTr("Download size not reported by PackageKit")
                        color: Theme.textMuted
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeSmall
                        elide: Text.ElideRight
                    }
                }
                NxButton {
                    text: page.checker.model.selectedCount === page.checker.count ? qsTr("Select none") : qsTr("Select all")
                    kind: "ghost"
                    enabled: !page.working
                    onClicked: page.checker.model.selectAll(page.checker.model.selectedCount !== page.checker.count)
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceSm
                Text {
                    Layout.fillWidth: true
                    text: page.working ? qsTr("An operation is in progress.") : qsTr("Changes are simulated first; you are asked before anything is removed.")
                    color: Theme.textMuted
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                    wrapMode: Text.WordWrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                }
                NxButton {
                    text: qsTr("Install selected (%1)").arg(page.checker.model.selectedCount)
                    enabled: !page.working && page.service.available && page.checker.model.selectedCount > 0
                             && page.checker.model.selectedCount !== page.checker.count
                    onClicked: page.updater.installSelected()
                }
                NxButton {
                    text: qsTr("Security only (%1)").arg(page.checker.securityCount)
                    iconName: "shield-check"
                    enabled: !page.working && page.service.available && page.checker.securityCount > 0
                             && page.checker.securityCount !== page.checker.count
                    onClicked: page.updater.installSecurity()
                }
                NxButton {
                    text: qsTr("Install all (%1)").arg(page.checker.count)
                    kind: "primary"
                    iconName: "download"
                    enabled: !page.working && page.service.available
                    onClicked: page.updater.installAll()
                }
            }
        }
    }

    footer: Item {
        width: page.contentWidth
        height: empty.visible ? empty.implicitHeight : 0
        NxEmptyState {
            id: empty
            anchors.fill: parent
            visible: page.count === 0 && !page.checker.busy && page.checker.state !== "idle"
            icon: page.checker.state === "ready" ? "check-circle" : "x-circle"
            tone: page.checker.state === "ready" ? "neutral" : "danger"
            title: page.checker.state === "ready" ? qsTr("This system is up to date") : qsTr("Updates cannot be listed")
            message: page.checker.state === "ready"
                ? (page.checker.refreshed ? qsTr("The software lists were refreshed and contain no newer packages.")
                   : qsTr("The software lists on this computer contain no newer packages. Choose “Check for updates” to refresh them."))
                : (page.service.available ? qsTr("PackageKit could not read the list of updates. The reason is shown above.")
                   : qsTr("The PackageKit service is not reachable. The reason is shown above."))
        }
    }
}
