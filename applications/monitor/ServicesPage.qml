// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.monitor

Item {
    id: page

    required property SystemMonitor monitor

    // The action waiting for confirmation.
    property string pendingAction: ""
    property string pendingUnit: ""
    property bool syncing: false

    readonly property bool hasSelection: services.selectedRow >= 0
    readonly property var statusLabels: ({
        "running": qsTr("Running"), "exited": qsTr("Finished"), "inactive": qsTr("Inactive"),
        "failed": qsTr("Failed"), "activating": qsTr("Starting"), "deactivating": qsTr("Stopping"),
        "reloading": qsTr("Reloading"), "active": qsTr("Active"),
    })
    readonly property var bootLabels: ({
        "enabled": qsTr("Enabled"), "enabled-runtime": qsTr("Enabled (this boot)"), "disabled": qsTr("Disabled"),
        "static": qsTr("Static"), "masked": qsTr("Masked"), "masked-runtime": qsTr("Masked (this boot)"),
        "indirect": qsTr("Indirect"), "generated": qsTr("Generated"), "transient": qsTr("Transient"),
        "alias": qsTr("Alias"), "linked": qsTr("Linked"), "linked-runtime": qsTr("Linked (this boot)"),
    })

    function toast(text, kind) { ApplicationWindow.window.toaster.show(text, kind) }

    // Starting and enabling happen at once; anything that interrupts a
    // service asks first.
    function request(action) {
        if (!hasSelection || services.busy)
            return
        if (action === "start" || action === "enable") {
            services.perform(action, services.selectedUnit)
            return
        }
        pendingAction = action
        pendingUnit = services.selectedUnit
        confirm.open()
    }

    ServiceModel {
        id: services
        active: page.visible && page.monitor.active
        filter: search.text
    }

    Connections {
        target: services
        // The selection belongs to the model. While rows come and go the
        // view has no current row, so it cannot move one on its own; the
        // pending changes are applied in that state, then the row of the
        // selected item is set again.
        function onAboutToRefresh() {
            page.syncing = true
            table.currentRow = -1
        }
        function onRefreshed() {
            page.syncing = true
            table.view.forceLayout()
            table.currentRow = services.selectedRow
            page.syncing = false
        }
        function onActionFinished(unit, action, ok, message) {
            page.toast(message, ok ? "success" : "danger")
        }
    }

    Shortcut {
        sequences: [StandardKey.Find]
        enabled: page.visible && services.available
        onActivated: { search.forceActiveFocus(); search.selectAll() }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spaceXl
        spacing: Theme.spaceLg

        // Title and tool bar side by side; stacked when the window is narrow.
        GridLayout {
            id: header
            readonly property bool stacked: page.width < 900
            Layout.fillWidth: true
            columns: stacked ? 1 : 2
            columnSpacing: Theme.spaceSm
            rowSpacing: Theme.spaceMd

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceXs
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Services")
                    color: Theme.textPrimary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeHeading
                    font.weight: Theme.weightSemibold
                    elide: Text.ElideRight
                }
                Text {
                    Layout.fillWidth: true
                    color: Theme.textSecondary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBody
                    elide: Text.ElideRight
                    text: !services.available ? qsTr("System services managed by systemd")
                        : !services.loaded ? qsTr("Asking systemd for its services…")
                        : services.error !== "" && services.totalCount === 0 ? qsTr("The list of services is not available")
                        : [qsTr("%1 services").arg(services.totalCount), qsTr("%1 running").arg(services.runningCount),
                           qsTr("%1 failed").arg(services.failedCount)].join(" · ")
                }
            }

            RowLayout {
                Layout.fillWidth: header.stacked
                visible: services.available
                spacing: Theme.spaceSm

                NxSearchField {
                    id: search
                    Layout.fillWidth: header.stacked
                    Layout.preferredWidth: 240
                    placeholderText: qsTr("Search services")
                }
                NxSegmented {
                    model: [qsTr("All"), qsTr("Running"), qsTr("Failed")]
                    onActivated: (index) => services.stateFilter = ["all", "running", "failed"][index]
                }
                NxIconButton {
                    iconName: "refresh"
                    tooltip: qsTr("Refresh the list")
                    onClicked: services.refresh()
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: services.available
            spacing: Theme.spaceSm

            Text {
                Layout.fillWidth: true
                color: Theme.textMuted
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WordWrap
                maximumLineCount: 2
                elide: Text.ElideRight
                text: !page.hasSelection ? qsTr("Select a service to start, stop or restart it, or to change whether it starts at boot.")
                    : services.enablementHint !== "" ? services.enablementHint
                    : qsTr("Changing a service asks for administrator authentication.")
            }
            NxButton {
                text: services.canStop ? qsTr("Stop") : qsTr("Start")
                iconName: services.canStop ? "stop" : "play"
                busy: services.busy && (services.busyAction === "start" || services.busyAction === "stop")
                enabled: page.hasSelection && !services.busy && (services.canStart || services.canStop)
                onClicked: page.request(services.canStop ? "stop" : "start")
            }
            NxButton {
                text: qsTr("Restart")
                iconName: "refresh"
                busy: services.busy && services.busyAction === "restart"
                enabled: page.hasSelection && !services.busy && services.canRestart
                onClicked: page.request("restart")
            }
            NxButton {
                text: services.canDisable ? qsTr("Disable at boot") : qsTr("Enable at boot")
                iconName: "power"
                busy: services.busy && (services.busyAction === "enable" || services.busyAction === "disable")
                enabled: page.hasSelection && !services.busy && (services.canEnable || services.canDisable)
                onClicked: page.request(services.canDisable ? "disable" : "enable")
            }
        }

        NxBanner {
            visible: services.available && services.error !== ""
            kind: "danger"
            title: qsTr("The services could not be listed")
            message: services.error
            actionText: qsTr("Try again")
            onActionTriggered: services.refresh()
        }

        NxCard {
            Layout.fillWidth: true
            Layout.fillHeight: true
            padding: Theme.spaceSm

            NxDataTable {
                id: table
                anchors.fill: parent
                visible: services.available
                model: services
                sortRole: "name"
                sortAscending: true
                emptyText: !services.loaded ? qsTr("Asking systemd for its services…")
                    : services.error !== "" ? qsTr("Nothing to show")
                    : services.filter !== "" ? qsTr("No service matches “%1”").arg(services.filter)
                    : services.stateFilter === "failed" ? qsTr("No service has failed")
                    : qsTr("No services to show")
                columns: [
                    { title: qsTr("Service"), role: "name", width: 250 },
                    { title: qsTr("Description"), role: "description", fill: true,
                      format: (v) => v !== "" ? v : "—", color: () => Theme.textSecondary },
                    { title: qsTr("State"), role: "status", width: 120,
                      format: (v) => page.statusLabels[v] ?? v,
                      color: (v) => v === "running" ? Theme.success : v === "failed" ? Theme.danger
                          : v === "inactive" ? Theme.textMuted : Theme.textSecondary },
                    { title: qsTr("At boot"), role: "fileState", width: 150,
                      format: (v) => v === "" ? "—" : (page.bootLabels[v] ?? v),
                      color: (v) => v === "enabled" || v === "enabled-runtime" ? Theme.textPrimary
                          : v.startsWith("masked") ? Theme.warning : Theme.textMuted },
                ]
                onSortRequested: (role, ascending) => services.sortBy(role, ascending)
                onCurrentRowChanged: {
                    if (page.syncing)
                        return
                    if (services.selectRow(currentRow) && currentRow >= 0)
                        view.positionViewAtIndex(currentRow, ListView.Contain)
                }
                Component.onCompleted: view.highlightFollowsCurrentItem = false
            }

            NxEmptyState {
                anchors.fill: parent
                visible: !services.available
                icon: "layers"
                title: services.unavailableTitle
                message: services.unavailableReason
            }
        }
    }

    NxConfirmDialog {
        id: confirm
        focus: true
        readonly property string name: page.pendingUnit.endsWith(".service") ? page.pendingUnit.slice(0, -8) : page.pendingUnit
        title: page.pendingAction === "stop" ? qsTr("Stop this service?")
            : page.pendingAction === "restart" ? qsTr("Restart this service?")
            : qsTr("Disable this service at boot?")
        message: page.pendingAction === "stop"
            ? qsTr("“%1” will be stopped. Programs and connections that depend on it stop working until it is started again.").arg(name)
            : page.pendingAction === "restart"
            ? qsTr("“%1” will be stopped and started again. Programs and connections that depend on it are interrupted.").arg(name)
            : qsTr("“%1” will no longer be started automatically when the computer boots. It keeps running until it is stopped.").arg(name)
        confirmText: page.pendingAction === "stop" ? qsTr("Stop")
            : page.pendingAction === "restart" ? qsTr("Restart") : qsTr("Disable")
        destructive: page.pendingAction !== "restart"
        onConfirmed: services.perform(page.pendingAction, page.pendingUnit)
    }
}
