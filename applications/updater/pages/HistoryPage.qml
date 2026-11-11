// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.updater

// Dated record of package changes: APT's log (every apt, apt-get and
// PackageKit run that changed packages), completed with who started each
// PackageKit run, plus PackageKit transactions that failed before APT logged
// anything.
PkListPage {
    id: page

    required property QtObject updater

    function actionTitle(action, count) {
        const names = {
            "install": qsTr("Installed"), "upgrade": qsTr("Upgraded"), "downgrade": qsTr("Downgraded"),
            "reinstall": qsTr("Reinstalled"), "remove": qsTr("Removed"), "purge": qsTr("Purged (removed with configuration)")
        }
        return (names[action] || action) + " (" + count + ")"
    }

    function details(entry) {
        let out = []
        if (entry.kind === "packagekit-failed") {
            for (let i = 0; i < entry.packages.length; i++) {
                const p = entry.packages[i]
                out.push(p.info + "  " + p.name + "  " + p.version + "  (" + p.arch + (p.repo !== "" ? ", " + p.repo : "") + ")")
            }
            return out.join("\n")
        }
        for (let a = 0; a < entry.actions.length; a++) {
            const act = entry.actions[a]
            out.push(actionTitle(act.action, act.packages.length))
            for (let i = 0; i < act.packages.length; i++) {
                const p = act.packages[i]
                const name = p.arch !== "" ? p.name + ":" + p.arch : p.name
                if (p.from !== undefined) out.push("    " + name + "  " + p.from + " → " + p.to)
                else out.push("    " + name + "  " + p.version + (p.automatic ? "  " + qsTr("(dependency)") : ""))
            }
            out.push("")
        }
        return out.join("\n").trim()
    }

    function startedText(entry) {
        if ((entry.startedBy || "") !== "")
            return qsTr("Started by %1 with %2").arg(entry.startedBy).arg((entry.client || "") !== "" ? entry.client : qsTr("an unnamed program"))
        if ((entry.requestedBy || "") !== "")
            return qsTr("Requested by %1").arg(entry.requestedBy)
        if (entry.viaPackageKit === true)
            return qsTr("Started through PackageKit — the user is not recorded")
        return qsTr("Started as root — the log names no requesting user")
    }

    title: qsTr("Update history")
    subtitle: qsTr("Package changes recorded by APT in /var/log/apt/history.log, newest first: every apt, apt-get and PackageKit run that changed packages.")
    actions: [
        NxIconButton {
            iconName: "refresh"
            tooltip: qsTr("Read the history again")
            onClicked: { page.updater.refreshHistory(); page.updater.refreshTransactions() }
        }
    ]

    model: updater.history

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spaceMd
        Text {
            Layout.fillWidth: true
            text: page.updater.transactionsError !== ""
                ? qsTr("PackageKit's own records are not available, so entries do not show who started a PackageKit transaction.")
                : qsTr("Who started a PackageKit transaction is taken from PackageKit's own records.")
            color: Theme.textMuted
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
            wrapMode: Text.WordWrap
        }
        Text {
            text: page.updater.historyLoading ? qsTr("Reading…") : qsTr("%1 entries").arg(page.updater.history.length)
            color: Theme.textMuted
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
        }
    }

    NxBanner {
        visible: page.updater.historyTruncated
        kind: "info"
        message: qsTr("Only the newest entries are shown; older ones remain in the rotated log files in /var/log/apt.")
    }

    delegate: Rectangle {
        id: entryRow
        required property var modelData
        required property int index
        property bool expanded: false
        readonly property bool pkFailure: modelData.kind === "packagekit-failed"
        readonly property bool failed: pkFailure || (modelData.error || "") !== ""
        readonly property int packageCount: modelData.packageCount || 0

        width: page.contentWidth
        implicitHeight: entryColumn.implicitHeight + Theme.spaceMd * 2
        radius: Theme.radiusMd
        color: Theme.bgRaised
        border.width: Theme.borderWidth
        border.color: failed ? Theme.alpha(Theme.danger, 0.45) : Theme.borderSubtle

        ColumnLayout {
            id: entryColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Theme.spaceMd
            spacing: Theme.spaceXs

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceMd
                NxIcon {
                    name: entryRow.failed ? "x-circle" : "clock"
                    size: Theme.icon
                    color: entryRow.failed ? Theme.danger : Theme.textSecondary
                }
                PkText {
                    text: entryRow.modelData.startText || ""
                    font.weight: Theme.weightSemibold
                }
                PkText {
                    Layout.fillWidth: true
                    text: entryRow.pkFailure
                        ? qsTr("%1 — failed before any package was changed").arg(entryRow.modelData.summary || "")
                        : (entryRow.modelData.summary || "")
                    color: Theme.textSecondary
                    elide: Text.ElideRight
                }
                NxBadge { visible: entryRow.failed; text: qsTr("Failed"); kind: "danger" }
                NxBadge {
                    visible: entryRow.modelData.viaPackageKit === true
                    text: qsTr("PackageKit")
                    kind: "accent"
                    dot: false
                }
                NxIconButton {
                    visible: entryRow.packageCount > 0
                    iconName: entryRow.expanded ? "chevron-up" : "chevron-down"
                    tooltip: entryRow.expanded ? qsTr("Hide the packages") : qsTr("Show the packages and versions")
                    onClicked: entryRow.expanded = !entryRow.expanded
                }
            }
            PkText {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.icon + Theme.spaceMd
                text: (entryRow.modelData.commandLine || "") !== "" ? entryRow.modelData.commandLine : qsTr("(command line not recorded)")
                color: Theme.textMuted
                font.family: Theme.fontMono
                font.pixelSize: Theme.fontSizeSmall
                elide: Text.ElideRight
            }
            PkText {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.icon + Theme.spaceMd
                text: page.startedText(entryRow.modelData)
                color: Theme.textMuted
                font.pixelSize: Theme.fontSizeSmall
                elide: Text.ElideRight
            }
            PkText {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.icon + Theme.spaceMd
                visible: (entryRow.modelData.error || "") !== ""
                text: qsTr("Error: %1").arg(entryRow.modelData.error || "")
                color: Theme.danger
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WordWrap
            }

            Loader {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.icon + Theme.spaceMd
                Layout.topMargin: Theme.spaceXs
                active: entryRow.expanded
                visible: active
                sourceComponent: Rectangle {
                    implicitHeight: Math.min(260, detailText.implicitHeight + Theme.spaceMd * 2)
                    radius: Theme.radiusSm
                    color: Theme.bgInput
                    border.width: Theme.borderWidth
                    border.color: Theme.border
                    clip: true
                    Flickable {
                        anchors.fill: parent
                        anchors.margins: Theme.spaceMd
                        contentHeight: detailText.implicitHeight
                        boundsBehavior: Flickable.StopAtBounds
                        clip: true
                        ScrollBar.vertical: ScrollBar { }
                        TextEdit {
                            id: detailText
                            width: parent.width
                            text: page.details(entryRow.modelData)
                            textFormat: TextEdit.PlainText
                            readOnly: true
                            selectByMouse: true
                            wrapMode: TextEdit.Wrap
                            color: Theme.textSecondary
                            selectionColor: Theme.accentFillStrong
                            selectedTextColor: Theme.textPrimary
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.fontSizeSmall
                            Accessible.name: qsTr("Packages changed")
                        }
                    }
                }
            }
        }
    }

    footer: Item {
        width: page.contentWidth
        height: emptyState.visible ? emptyState.implicitHeight : 0
        NxEmptyState {
            id: emptyState
            anchors.fill: parent
            visible: page.count === 0 && !page.updater.historyLoading
            icon: page.updater.historyError !== "" ? "x-circle" : "clock"
            tone: page.updater.historyError !== "" ? "warning" : "neutral"
            title: page.updater.historyError !== "" ? qsTr("The APT history could not be read") : qsTr("No package changes recorded")
            message: page.updater.historyError !== ""
                ? qsTr("Reason:") + "\n" + page.updater.historyError
                : qsTr("/var/log/apt/history.log contains no entries.")
        }
    }
}
