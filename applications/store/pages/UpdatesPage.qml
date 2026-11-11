// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.store

// Updates for installed software, from PackageKit. The same list NEXUS
// Updater shows; history, restart handling and recovery live there.
PkListPage {
    id: page

    required property QtObject store
    readonly property QtObject checker: store.updates
    readonly property QtObject service: store.service
    readonly property bool working: store.busy || checker.busy

    signal openPackage(string name)

    title: qsTr("Updates")
    subtitle: checker.state === "refreshing" ? qsTr("Refreshing the software lists…")
        : checker.state === "loading" ? qsTr("Reading the list of updates…")
        : checker.state === "ready" && checker.count === 0 ? qsTr("No updates are available.")
        : checker.state === "ready" && checker.count === 1 ? qsTr("1 update is available.")
        : checker.state === "ready" ? qsTr("%1 updates are available, %2 of them security updates.").arg(checker.count).arg(checker.securityCount)
        : checker.state === "error" ? qsTr("The list of updates is not available.")
        : qsTr("Updates have not been checked yet.")
    actions: [
        NxButton {
            visible: page.store.updaterAvailable
            text: qsTr("Open NEXUS Updater")
            iconName: "external-link"
            onClicked: page.store.openUpdater("updates")
        },
        NxButton {
            text: qsTr("Check for updates")
            iconName: "refresh"
            busy: page.checker.busy
            enabled: page.service.available && !page.working
            onClicked: page.checker.check(true)
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
        opacity: page.checker.busy ? 0.45 : 1
    }

    PkServiceBanner {
        service: page.service
        consequence: qsTr("Updates cannot be listed or installed until it is available.")
    }
    NxBanner {
        visible: page.checker.refreshError !== ""
        kind: "danger"
        title: qsTr("The software lists could not be refreshed")
        message: qsTr("The updates shown are from the last successful refresh and may be out of date. PackageKit reported:")
                 + "\n" + page.checker.refreshError
    }
    NxBanner {
        visible: page.service.available && page.checker.state === "error"
        kind: "danger"
        title: qsTr("The list of updates could not be read")
        message: qsTr("PackageKit reported:") + "\n" + page.checker.error
    }

    NxCard {
        Layout.fillWidth: true
        visible: page.checker.state === "ready" && page.checker.count > 0
        RowLayout {
            width: parent.width
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
            NxButton {
                text: qsTr("Update selected (%1)").arg(page.checker.model.selectedCount)
                enabled: !page.working && page.service.available && page.checker.model.selectedCount > 0
                         && page.checker.model.selectedCount !== page.checker.count
                onClicked: page.store.installUpdates(true)
            }
            NxButton {
                text: qsTr("Update all (%1)").arg(page.checker.count)
                kind: "primary"
                iconName: "download"
                enabled: !page.working && page.service.available
                onClicked: page.store.installUpdates(false)
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
            title: page.checker.state === "ready" ? qsTr("Everything is up to date") : qsTr("Updates cannot be listed")
            message: page.checker.state === "ready"
                ? (page.checker.refreshed ? qsTr("The software lists were refreshed and contain no newer packages.")
                   : qsTr("The software lists on this computer contain no newer packages. Choose “Check for updates” to refresh them."))
                : (page.service.available ? qsTr("PackageKit could not read the list of updates. The reason is shown above.")
                   : qsTr("The PackageKit service is not reachable. The reason is shown above."))
        }
    }
}
