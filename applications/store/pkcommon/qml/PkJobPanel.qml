// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui

// Live view of one ShadowPk::Job: what PackageKit is doing right now (status,
// percentage, per-package progress, download figures), a Cancel button for as
// long as PackageKit allows cancelling, and — once it is over — the outcome
// with PackageKit's error code and details.
NxCard {
    id: root

    // ShadowPk::Job; the panel hides itself while this is null.
    property QtObject job: null
    property bool showItems: false
    property bool showLog: false

    signal closeRequested()
    signal copyRequested(string text)
    signal repairRequested()

    readonly property bool active: job !== null && job.active
    readonly property bool failed: job !== null && job.result === "failed"
    readonly property bool succeeded: job !== null && job.result === "success"
    readonly property bool cancelled: job !== null && job.result === "cancelled"
    readonly property color tone: failed ? Theme.danger : succeeded ? Theme.success : cancelled ? Theme.textSecondary : Theme.accent

    function errorReport() {
        if (!job) return ""
        var lines = [job.title, qsTr("Result: %1").arg(job.result)]
        if (job.errorKey !== "") {
            lines.push(qsTr("PackageKit error code: %1").arg(job.errorKey))
            lines.push(qsTr("Error: %1").arg(job.errorTitle))
            lines.push(qsTr("Details: %1").arg(job.errorDetails))
        }
        lines.push("")
        lines.push(job.logText)
        return lines.join("\n")
    }

    visible: job !== null
    Layout.fillWidth: true
    border.color: failed ? Theme.alpha(Theme.danger, 0.55)
        : succeeded ? Theme.alpha(Theme.success, 0.45)
        : active ? Theme.alpha(Theme.accent, 0.55) : Theme.borderSubtle
    Accessible.role: Accessible.Grouping
    Accessible.name: job ? job.title + ": " + job.statusText : ""

    ColumnLayout {
        width: parent.width
        spacing: Theme.spaceMd

        // ---- title line ----------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceMd

            BusyIndicator {
                visible: root.active
                running: root.active
                implicitWidth: 24
                implicitHeight: 24
                padding: 0
            }
            NxIcon {
                visible: !root.active
                name: root.failed ? "x-circle" : root.succeeded ? "check-circle" : "info"
                size: Theme.iconLarge
                color: root.tone
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                Text {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: root.job ? root.job.title : ""
                    color: Theme.textPrimary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBodyLarge
                    font.weight: Theme.weightSemibold
                    elide: Text.ElideRight
                }
                Text {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: {
                        if (!root.job) return ""
                        if (root.succeeded) return qsTr("Finished successfully.")
                        if (root.cancelled) return qsTr("Cancelled — PackageKit made no further changes.")
                        if (root.failed) return qsTr("Failed: %1").arg(root.job.errorTitle)
                        var s = root.job.statusText
                        if (root.job.phase === "running" && root.job.currentItem !== "") s += " · " + root.job.currentItem
                        return s
                    }
                    color: root.failed ? Theme.danger : Theme.textSecondary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBody
                    elide: Text.ElideRight
                }
            }
            Text {
                textFormat: Text.PlainText
                visible: root.active && root.job.percentage >= 0
                text: root.job && root.job.percentage >= 0 ? root.job.percentage + " %" : ""
                color: Theme.textPrimary
                font.family: Theme.fontMono
                font.pixelSize: Theme.fontSizeBodyLarge
                font.weight: Theme.weightSemibold
            }
            NxIconButton {
                visible: root.job !== null
                iconName: "file-text"
                tooltip: root.showLog ? qsTr("Hide the log") : qsTr("Show the log of this operation")
                checkable: true
                checked: root.showLog
                onClicked: root.showLog = !root.showLog
            }
            NxButton {
                visible: root.active
                text: qsTr("Cancel")
                iconName: "close"
                enabled: root.job !== null && root.job.cancellable
                onClicked: root.job.cancel()
            }
            NxButton {
                visible: root.failed
                text: qsTr("Copy details")
                iconName: "copy"
                onClicked: root.copyRequested(root.errorReport())
            }
            NxButton {
                visible: root.job !== null && !root.active
                text: qsTr("Close")
                onClicked: root.closeRequested()
            }
        }

        // ---- progress ----------------------------------------------------------
        ProgressBar {
            Layout.fillWidth: true
            visible: root.active
            indeterminate: !root.job || root.job.percentage < 0
            value: root.job && root.job.percentage >= 0 ? root.job.percentage / 100 : 0
        }

        RowLayout {
            Layout.fillWidth: true
            visible: root.active && (progressNote.text !== "" || itemList.count > 0)
            spacing: Theme.spaceMd
            Text {
                id: progressNote
                textFormat: Text.PlainText
                Layout.fillWidth: true
                text: {
                    if (!root.job) return ""
                    var parts = []
                    // The back-end keeps the last figure after the download ended; show it only while downloading.
                    if (root.job.status === "download" && root.job.downloadSizeRemaining > 0)
                        parts.push(qsTr("%1 left to download").arg(Theme.formatBytes(root.job.downloadSizeRemaining)))
                    if (root.job.status === "download" && root.job.downloadRate > 0)
                        parts.push(Theme.formatRate(root.job.downloadRate))
                    if (root.job.remainingTime > 0)
                        parts.push(qsTr("about %1 remaining").arg(Theme.formatDuration(root.job.remainingTime)))
                    if (root.job.phase === "running" && !root.job.cancellable)
                        parts.push(qsTr("cannot be cancelled at this stage"))
                    return parts.join(" · ")
                }
                color: Theme.textMuted
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                elide: Text.ElideRight
            }
            NxButton {
                kind: "ghost"
                visible: itemList.count > 0
                text: root.showItems ? qsTr("Hide packages") : qsTr("Show packages (%1)").arg(itemList.count)
                iconName: root.showItems ? "chevron-up" : "chevron-down"
                onClicked: root.showItems = !root.showItems
            }
        }

        // ---- outcome details -----------------------------------------------------
        ColumnLayout {
            Layout.fillWidth: true
            visible: root.failed
            spacing: Theme.spaceXs
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                text: root.job ? qsTr("PackageKit error code: %1").arg(root.job.errorKey) : ""
                color: Theme.textSecondary
                font.family: Theme.fontMono
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WrapAnywhere
            }
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: Math.min(180, errorText.implicitHeight + Theme.spaceMd * 2)
                visible: root.job !== null && root.job.errorDetails !== ""
                radius: Theme.radiusSm
                color: Theme.bgInput
                border.width: Theme.borderWidth
                border.color: Theme.border
                clip: true
                Flickable {
                    anchors.fill: parent
                    anchors.margins: Theme.spaceMd
                    contentHeight: errorText.implicitHeight
                    boundsBehavior: Flickable.StopAtBounds
                    clip: true
                    ScrollBar.vertical: ScrollBar { }
                    TextEdit {
                        id: errorText
                        width: parent.width
                        text: root.job ? root.job.errorDetails : ""
                        textFormat: TextEdit.PlainText
                        readOnly: true
                        selectByMouse: true
                        wrapMode: TextEdit.Wrap
                        color: Theme.textPrimary
                        selectionColor: Theme.accentFillStrong
                        selectedTextColor: Theme.textPrimary
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fontSizeSmall
                        Accessible.name: qsTr("Error details from PackageKit")
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                visible: root.job !== null && root.job.repairRequired
                spacing: Theme.spaceMd
                Text {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: qsTr("An earlier package operation was interrupted. The package system must be repaired before this can work.")
                    color: Theme.warning
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBody
                    wrapMode: Text.WordWrap
                }
                NxButton {
                    text: qsTr("Open recovery")
                    iconName: "life-buoy"
                    onClicked: root.repairRequested()
                }
            }
        }

        Text {
            textFormat: Text.PlainText
            Layout.fillWidth: true
            visible: root.job !== null && !root.active && root.job.restart !== "none"
            text: {
                if (!root.job) return ""
                var who = root.job.restartPackages.length > 0 ? root.job.restartPackages.join(", ") : qsTr("the changed packages")
                if (root.job.restart === "system") return qsTr("A restart of the computer is needed for: %1.").arg(who)
                if (root.job.restart === "session") return qsTr("Log out and back in to finish updating: %1.").arg(who)
                return qsTr("Restart the affected applications to use the new version of: %1.").arg(who)
            }
            color: Theme.warning
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeBody
            wrapMode: Text.WordWrap
        }

        // ---- per-package progress ----------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            visible: root.showItems && itemList.count > 0
            implicitHeight: Math.min(220, itemList.contentHeight + 2)
            radius: Theme.radiusSm
            color: Theme.bgInput
            border.width: Theme.borderWidth
            border.color: Theme.border
            clip: true

            ListView {
                id: itemList
                anchors.fill: parent
                anchors.margins: 1
                model: root.job ? root.job.items : null
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { }
                delegate: Item {
                    id: itemRow
                    required property string name
                    required property string version
                    required property string status
                    required property int percentage
                    width: ListView.view.width
                    height: 30
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spaceMd
                        anchors.rightMargin: Theme.spaceMd
                        spacing: Theme.spaceMd
                        Text {
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            Layout.preferredWidth: 200
                            text: itemRow.name
                            color: Theme.textPrimary
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeSmall
                            elide: Text.ElideRight
                        }
                        Text {
                            textFormat: Text.PlainText
                            Layout.preferredWidth: 150
                            text: itemRow.version
                            color: Theme.textMuted
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.fontSizeCaption
                            elide: Text.ElideRight
                        }
                        Text {
                            textFormat: Text.PlainText
                            Layout.preferredWidth: 110
                            text: itemRow.status
                            color: Theme.textSecondary
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeSmall
                            elide: Text.ElideRight
                        }
                        ProgressBar {
                            Layout.preferredWidth: 90
                            opacity: itemRow.percentage >= 0 ? 1 : 0
                            value: itemRow.percentage >= 0 ? itemRow.percentage / 100 : 0
                        }
                    }
                }
            }
        }

        // ---- log ------------------------------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            visible: root.showLog && root.job !== null
            implicitHeight: 200
            radius: Theme.radiusSm
            color: Theme.bgInput
            border.width: Theme.borderWidth
            border.color: Theme.border
            clip: true
            Flickable {
                id: logFlick
                anchors.fill: parent
                anchors.margins: Theme.spaceMd
                contentHeight: logText.implicitHeight
                boundsBehavior: Flickable.StopAtBounds
                clip: true
                ScrollBar.vertical: ScrollBar { }
                // Follow the log while it grows.
                onContentHeightChanged: if (contentHeight > height) contentY = contentHeight - height
                TextEdit {
                    id: logText
                    width: parent.width
                    text: root.job ? root.job.logText : ""
                    textFormat: TextEdit.PlainText
                    readOnly: true
                    selectByMouse: true
                    wrapMode: TextEdit.Wrap
                    color: Theme.textSecondary
                    selectionColor: Theme.accentFillStrong
                    selectedTextColor: Theme.textPrimary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fontSizeSmall
                    Accessible.name: qsTr("Transaction log")
                }
            }
        }
    }
}
