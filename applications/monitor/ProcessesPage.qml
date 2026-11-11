// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.monitor

Item {
    id: page

    required property SystemMonitor monitor

    // The process the open confirmation refers to (ProcessModel.describe()).
    property var target: ({})
    // What is to be done to it: "end", "kill", "stop", "continue" or "renice".
    property string targetAction: "end"
    property int targetNice: 0
    // True while the table's current row is being set from the model, as
    // opposed to by the user.
    property bool syncing: false

    function toast(text, kind) { ApplicationWindow.window.toaster.show(text, kind) }

    function focusSearch() {
        search.forceActiveFocus()
        search.selectAll()
    }

    // The selected process as ProcessModel.describe() reports it, or null
    // (with an explanation shown) if nothing may be done to it.
    function selectedTarget() {
        if (processes.selectedRow < 0) {
            toast(qsTr("Select a process first"), "info")
            return null
        }
        const info = processes.describe(processes.selectedRow)
        if (info.pid === undefined)
            return null
        if (info.protected) {
            toast(info.protectedReason, "warning")
            return null
        }
        return info
    }

    // Ask before sending a signal to the selected process:
    // "end" (SIGTERM), "kill" (SIGKILL), "stop" (SIGSTOP), "continue" (SIGCONT).
    function requestAction(action) {
        const info = selectedTarget()
        if (!info)
            return
        if (action === "stop" && info.self) {
            toast(qsTr("System Monitor cannot suspend itself: nothing could resume it from here."), "warning")
            return
        }
        target = info
        targetAction = action
        confirm.open()
    }

    // Open the priority dialog for the selected process.
    function requestPriority() {
        const info = selectedTarget()
        if (!info)
            return
        target = info
        targetAction = "renice"
        niceSlider.value = info.nice
        priorityDialog.open()
    }

    function perform(asAdministrator) {
        if (targetAction === "renice")
            processes.setPriority(target.pid, target.token, targetNice, asAdministrator)
        else
            processes.sendSignal(target.pid, target.token, targetAction, asAdministrator)
    }

    readonly property bool dialogOpen: confirm.visible || administratorConfirm.visible || priorityDialog.visible
    readonly property bool selectedStopped: processes.selectedStopped

    ProcessModel {
        id: processes
        active: page.visible && page.monitor.active
        interval: page.monitor.interval
        filter: search.text
    }

    Connections {
        target: processes
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
            table.currentRow = processes.selectedRow
            page.syncing = false
        }
        function onActionFinished(pid, name, action, outcome, message) {
            if (outcome === "needs-administrator") {
                if (page.target.pid === pid) administratorConfirm.open()
                else page.toast(message, "danger")
            } else {
                page.toast(message, outcome === "done" ? "success"
                                  : outcome === "gone" || outcome === "cancelled" ? "info"
                                  : outcome === "refused" ? "warning" : "danger")
            }
        }
    }

    Shortcut {
        sequences: [StandardKey.Find]
        enabled: page.visible
        onActivated: page.focusSearch()
    }
    Shortcut {
        sequence: "Delete"
        enabled: page.visible && !page.dialogOpen
        onActivated: page.requestAction("end")
    }
    Shortcut {
        sequence: "Shift+Delete"
        enabled: page.visible && !page.dialogOpen
        onActivated: page.requestAction("kill")
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
                    text: qsTr("Processes")
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
                    text: {
                        if (!processes.loaded)
                            return qsTr("Reading the process table…")
                        const parts = [qsTr("%1 processes").arg(processes.processCount),
                                       qsTr("%1 running").arg(processes.runningCount),
                                       qsTr("%1 kernel threads").arg(processes.kernelThreadCount)]
                        if (processes.filter !== "" || processes.onlyOwnProcesses)
                            parts.push(qsTr("%1 shown").arg(processes.count))
                        return parts.join(" · ")
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: header.stacked
                spacing: Theme.spaceSm

                NxSearchField {
                    id: search
                    Layout.fillWidth: header.stacked
                    Layout.preferredWidth: 216
                    placeholderText: qsTr("Search processes")
                    ToolTip.visible: hovered && !activeFocus && text === ""
                    ToolTip.text: qsTr("Search by name, user, PID or command line (Ctrl+F)")
                }
                NxIconButton {
                    iconName: "filter"
                    tooltip: qsTr("View options")
                    checked: processes.onlyOwnProcesses || processes.showKernelThreads || processes.tree
                    ToolTip.visible: hovered && !optionsMenu.visible
                    onClicked: optionsMenu.open()

                    Menu {
                        id: optionsMenu
                        y: parent.height + Theme.spaceXs
                        MenuItem {
                            text: qsTr("Only my processes")
                            checkable: true
                            checked: processes.onlyOwnProcesses
                            onToggled: processes.onlyOwnProcesses = checked
                        }
                        MenuItem {
                            text: qsTr("Show kernel threads")
                            checkable: true
                            checked: processes.showKernelThreads
                            onToggled: processes.showKernelThreads = checked
                        }
                        MenuItem {
                            text: qsTr("Show as tree")
                            checkable: true
                            checked: processes.tree
                            onToggled: processes.tree = checked
                        }
                    }
                }
                NxButton {
                    text: qsTr("End process")
                    iconName: "x-circle"
                    enabled: processes.selectedRow >= 0
                    onClicked: page.requestAction("end")
                }
                NxButton {
                    text: qsTr("Force kill")
                    kind: enabled ? "danger" : "secondary"
                    iconName: "zap"
                    enabled: processes.selectedRow >= 0
                    onClicked: page.requestAction("kill")
                }
                NxIconButton {
                    iconName: "more-horizontal"
                    tooltip: processes.selectedRow >= 0 ? qsTr("Suspend, continue or change priority")
                                                        : qsTr("Select a process to suspend it, continue it or change its priority")
                    enabled: processes.selectedRow >= 0
                    ToolTip.visible: hovered && !moreMenu.visible
                    onClicked: moreMenu.open()

                    Menu {
                        id: moreMenu
                        y: parent.height + Theme.spaceXs
                        x: parent.width - width
                        MenuItem {
                            text: page.selectedStopped ? qsTr("Continue") : qsTr("Suspend")
                            icon.source: page.selectedStopped ? "image://nxicon/play" : "image://nxicon/pause"
                            onTriggered: page.requestAction(page.selectedStopped ? "continue" : "stop")
                        }
                        MenuItem {
                            text: qsTr("Change priority…")
                            icon.source: "image://nxicon/sliders"
                            onTriggered: page.requestPriority()
                        }
                    }
                }
            }
        }

        NxCard {
            Layout.fillWidth: true
            Layout.fillHeight: true
            padding: Theme.spaceSm

            NxDataTable {
                id: table
                anchors.fill: parent
                model: processes
                sortRole: "cpu"
                sortAscending: false
                emptyText: !processes.loaded ? qsTr("Reading the process table…")
                    : processes.filter !== "" ? qsTr("No process matches “%1”").arg(processes.filter)
                    : qsTr("No processes to show")
                // In a narrow window the command line gives way to the other
                // columns; it stays searchable and can be copied from the row's menu.
                readonly property bool wide: width >= 860
                columns: [
                    { title: qsTr("Name"), role: "name", fill: !wide, width: processes.tree ? 260 : 200,
                      // Tree view: children are indented below their parent.
                      format: (v, row) => row.depth > 0 ? "\u2002\u2002".repeat(row.depth - 1) + "\u2514\u2002" + v : v },
                    { title: qsTr("PID"), role: "pid", width: 76, align: Qt.AlignRight, mono: true },
                    { title: qsTr("User"), role: "user", width: 108 },
                    { title: qsTr("CPU"), role: "cpu", width: 84, align: Qt.AlignRight, mono: true,
                      format: (v) => v.toFixed(1) + " %",
                      color: (v) => v >= 50 ? Theme.warning : v >= 0.05 ? Theme.textPrimary : Theme.textMuted },
                    { title: qsTr("Memory"), role: "memory", width: 104, align: Qt.AlignRight, mono: true,
                      format: (v) => Theme.formatBytes(v),
                      color: (v) => v > 0 ? Theme.textSecondary : Theme.textMuted },
                    { title: qsTr("State"), role: "state", width: 116,
                      color: (v, row) => row.stateCode === "R" ? Theme.success
                          : row.stateCode === "Z" ? Theme.danger
                          : row.stateCode === "T" || row.stateCode === "t" ? Theme.warning : Theme.textSecondary },
                    { title: qsTr("Nice"), role: "nice", width: 64, align: Qt.AlignRight, mono: true,
                      color: (v) => v < 0 ? Theme.warning : v > 0 ? Theme.textSecondary : Theme.textMuted },
                ].concat(wide ? [{ title: qsTr("Command line"), role: "command", fill: true, mono: true,
                                   color: () => Theme.textMuted }] : [])
                onSortRequested: (role, ascending) => processes.sortBy(role, ascending)
                onCurrentRowChanged: {
                    if (page.syncing)
                        return
                    // Rows move as their values change, so the view must not
                    // chase the selection on its own; it is brought into view
                    // only when the user moves it.
                    if (processes.selectRow(currentRow) && currentRow >= 0)
                        view.positionViewAtIndex(currentRow, ListView.Contain)
                }
                onContextMenuRequested: rowMenu.popup()
                Component.onCompleted: view.highlightFollowsCurrentItem = false
            }
        }
    }

    Menu {
        id: rowMenu
        MenuItem {
            text: qsTr("End process")
            icon.source: "image://nxicon/x-circle"
            onTriggered: page.requestAction("end")
        }
        MenuItem {
            text: qsTr("Force kill")
            icon.source: "image://nxicon/zap"
            onTriggered: page.requestAction("kill")
        }
        MenuSeparator { }
        MenuItem {
            text: page.selectedStopped ? qsTr("Continue") : qsTr("Suspend")
            icon.source: page.selectedStopped ? "image://nxicon/play" : "image://nxicon/pause"
            onTriggered: page.requestAction(page.selectedStopped ? "continue" : "stop")
        }
        MenuItem {
            text: qsTr("Change priority…")
            icon.source: "image://nxicon/sliders"
            onTriggered: page.requestPriority()
        }
        MenuSeparator { }
        MenuItem {
            text: qsTr("Copy command line")
            icon.source: "image://nxicon/copy"
            onTriggered: {
                const info = processes.describe(processes.selectedRow)
                if (info.pid !== undefined) {
                    processes.copyToClipboard(info.command)
                    page.toast(qsTr("Command line copied"), "success")
                }
            }
        }
        MenuItem {
            text: qsTr("Copy PID")
            icon.source: "image://nxicon/hash"
            onTriggered: {
                if (processes.selectedPid > 0) {
                    processes.copyToClipboard(String(processes.selectedPid))
                    page.toast(qsTr("PID copied"), "success")
                }
            }
        }
    }

    NxConfirmDialog {
        id: confirm
        focus: true
        readonly property string who: qsTr("“%1” (PID %2, user %3)")
            .arg(page.target.name ?? "").arg(page.target.pid ?? 0).arg(page.target.user ?? "")
        readonly property string action: page.targetAction
        title: action === "kill" ? qsTr("Force this process to quit?")
            : action === "stop" ? qsTr("Suspend this process?")
            : action === "continue" ? qsTr("Continue this process?")
            : qsTr("End this process?")
        message: (action === "kill"
                  ? qsTr("%1 will be killed immediately (SIGKILL). It gets no chance to save data or clean up.").arg(who)
                  : action === "stop"
                  ? qsTr("%1 will be suspended (SIGSTOP): it stops running and stops responding, but keeps its memory and data until it is continued.").arg(who)
                  : action === "continue"
                  ? qsTr("%1 will be allowed to run again (SIGCONT).").arg(who)
                  : qsTr("%1 will be asked to quit (SIGTERM). Unsaved data in it may be lost.").arg(who))
            + (page.target.otherUser
               ? "\n\n" + qsTr("The process belongs to another user, so you will be asked to authenticate as an administrator.")
               : "")
        confirmText: {
            const label = action === "kill" ? qsTr("Force kill") : action === "stop" ? qsTr("Suspend")
                : action === "continue" ? qsTr("Continue") : qsTr("End process")
            return page.target.otherUser ? qsTr("%1 as administrator").arg(label) : label
        }
        destructive: action === "end" || action === "kill"
        onConfirmed: page.perform(page.target.otherUser === true)
    }

    // Reached when the kernel refused a request for a process that looked
    // like the user's own (for example a program running with elevated rights).
    NxConfirmDialog {
        id: administratorConfirm
        focus: true
        title: qsTr("Administrator rights needed")
        message: (page.targetAction === "renice"
                  ? qsTr("You are not permitted to give “%1” (PID %2) the nice value %3.").arg(page.target.name ?? "").arg(page.target.pid ?? 0).arg(page.targetNice)
                  : qsTr("You are not permitted to signal “%1” (PID %2).").arg(page.target.name ?? "").arg(page.target.pid ?? 0))
            + " " + qsTr("It can be done as an administrator; you will be asked to authenticate.")
        confirmText: qsTr("Continue as administrator")
        destructive: page.targetAction === "end" || page.targetAction === "kill"
        onConfirmed: page.perform(true)
    }

    // The nice value: -20 (most processor time) … 19 (least), as renice(1).
    Dialog {
        id: priorityDialog
        focus: true
        readonly property int chosen: Math.round(niceSlider.value)
        readonly property int current: page.target.nice ?? 0
        // Without privileges the kernel only lets a process be made "nicer",
        // and only one's own.
        readonly property bool needsAdministrator: page.target.privileged !== true
            && (page.target.otherUser === true || chosen < current)

        title: qsTr("Change priority")
        onOpened: niceSlider.forceActiveFocus()
        width: Math.min(460, parent ? parent.width - Theme.spaceXxl : 460)
        closePolicy: Popup.CloseOnEscape
        footer: null

        contentItem: ColumnLayout {
            spacing: Theme.spaceLg

            Text {
                Layout.fillWidth: true
                text: qsTr("“%1” (PID %2, user %3) runs with nice value %4. A lower value gets more processor time when the system is busy, a higher value less.")
                    .arg(page.target.name ?? "").arg(page.target.pid ?? 0).arg(page.target.user ?? "").arg(priorityDialog.current)
                color: Theme.textSecondary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceMd
                Slider {
                    id: niceSlider
                    Layout.fillWidth: true
                    from: -20
                    to: 19
                    stepSize: 1
                    snapMode: Slider.SnapAlways
                    Accessible.name: qsTr("Nice value")
                }
                Text {
                    Layout.preferredWidth: 36
                    horizontalAlignment: Text.AlignRight
                    text: priorityDialog.chosen
                    color: Theme.textPrimary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fontSizeBodyLarge
                }
            }
            Text {
                Layout.fillWidth: true
                text: priorityDialog.chosen === priorityDialog.current ? qsTr("Unchanged.")
                    : priorityDialog.needsAdministrator
                      ? (page.target.otherUser === true
                         ? qsTr("The process belongs to another user, so you will be asked to authenticate as an administrator.")
                         : qsTr("Raising a priority needs administrator rights; you will be asked to authenticate."))
                    : priorityDialog.chosen < priorityDialog.current ? qsTr("The process will get a higher priority.")
                    : qsTr("The process will get a lower priority. Raising it again later needs administrator rights.")
                color: priorityDialog.needsAdministrator && priorityDialog.chosen !== priorityDialog.current
                    ? Theme.warning : Theme.textMuted
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spaceXs
                spacing: Theme.spaceSm
                Item { Layout.fillWidth: true }
                NxButton {
                    text: qsTr("Cancel")
                    onClicked: priorityDialog.reject()
                }
                NxButton {
                    text: priorityDialog.needsAdministrator && priorityDialog.chosen !== priorityDialog.current
                        ? qsTr("Apply as administrator") : qsTr("Apply")
                    kind: "primary"
                    enabled: priorityDialog.chosen !== priorityDialog.current
                    onClicked: {
                        page.targetNice = priorityDialog.chosen
                        page.perform(priorityDialog.needsAdministrator)
                        priorityDialog.accept()
                    }
                }
            }
        }
    }
}
