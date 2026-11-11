// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui

// The running PackageKit operation: what will change (confirmation), real
// progress with cancellation, and the result or the error PackageKit reported.
Dialog {
    id: root

    readonly property var job: arsenal.job
    readonly property var changes: job.changes
    readonly property var installList: changes.install || []
    readonly property var removeList: changes.remove || []
    readonly property var updateList: changes.update || []

    // Packages a removal takes along that were not asked for by name.
    readonly property var collateral: removeList.map(entry => entry.name).filter(name => job.requested.indexOf(name) < 0)

    signal followUp(string kind, string target)

    modal: true
    title: job.title
    width: Math.min(600, parent ? parent.width - Theme.spaceXxl : 600)
    anchors.centerIn: parent
    closePolicy: Popup.NoAutoClose
    footer: null
    visible: job.phase !== "idle"

    function names(list) {
        return list.map(entry => entry.name + " " + entry.version).join("\n")
    }

    contentItem: ColumnLayout {
        spacing: Theme.spaceLg

        // ---- working -------------------------------------------------------------
        ColumnLayout {
            Layout.fillWidth: true
            visible: root.job.phase === "resolving" || root.job.phase === "simulating" || root.job.phase === "running"
            spacing: Theme.spaceSm

            Text {
                Layout.fillWidth: true
                text: root.job.statusText
                color: Theme.textPrimary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBodyLarge
                wrapMode: Text.WordWrap
            }
            ProgressBar {
                Layout.fillWidth: true
                indeterminate: root.job.percentage < 0
                from: 0; to: 100
                value: Math.max(0, root.job.percentage)
                Accessible.name: qsTr("Progress")
            }
            RowLayout {
                Layout.fillWidth: true
                Text {
                    Layout.fillWidth: true
                    text: root.job.currentItem
                    color: Theme.textMuted
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                    elide: Text.ElideRight
                }
                Text {
                    visible: root.job.phase === "running" && root.job.percentage >= 0
                    text: qsTr("%1 %").arg(root.job.percentage)
                    color: Theme.textMuted
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                }
            }
            Text {
                Layout.fillWidth: true
                visible: root.job.phase === "running" && root.job.downloadRemaining > 0
                text: qsTr("%1 left to download").arg(Theme.formatBytes(root.job.downloadRemaining))
                color: Theme.textMuted
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
            }
            Text {
                Layout.fillWidth: true
                visible: root.job.phase === "running" && !root.job.cancellable
                text: qsTr("Packages are being changed; this step cannot be interrupted safely.")
                color: Theme.textMuted
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WordWrap
            }
        }

        // ---- confirmation --------------------------------------------------------
        ColumnLayout {
            Layout.fillWidth: true
            visible: root.job.phase === "confirm"
            spacing: Theme.spaceMd

            Text {
                Layout.fillWidth: true
                text: root.job.kind === "install"
                    ? (root.installList.length === 1 ? qsTr("1 package will be installed.") : qsTr("%1 packages will be installed, dependencies included.").arg(root.installList.length))
                      + (root.changes.downloadBytes > 0 ? " " + qsTr("Download: %1.").arg(Theme.formatBytes(root.changes.downloadBytes)) : "")
                    : (root.removeList.length === 1 ? qsTr("1 package will be removed.") : qsTr("%1 packages will be removed.").arg(root.removeList.length))
                color: Theme.textPrimary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBodyLarge
                wrapMode: Text.WordWrap
            }
            NxBanner {
                Layout.fillWidth: true
                visible: root.job.kind === "install" && root.removeList.length > 0
                kind: "danger"
                title: (root.removeList.length === 1 ? qsTr("1 package will be removed to make this possible") : qsTr("%1 packages will be removed to make this possible").arg(root.removeList.length))
                message: root.removeList.map(entry => entry.name).join(", ")
            }
            NxBanner {
                Layout.fillWidth: true
                visible: root.job.kind === "remove" && root.collateral.length > 0
                kind: "danger"
                title: root.collateral.length === 1 ? qsTr("1 other package will be removed with it")
                                                    : qsTr("%1 other packages will be removed with it").arg(root.collateral.length)
                message: qsTr("They depend on what you are removing, or were installed only for it: %1").arg(
                             root.collateral.slice(0, 12).join(", ") + (root.collateral.length > 12 ? qsTr(" and more — see the list below") : ""))
            }
            NxBanner {
                Layout.fillWidth: true
                visible: root.job.unavailable.length > 0
                kind: "warning"
                title: qsTr("Not available from the configured repositories")
                message: root.job.unavailable.join(", ")
            }
            NxBanner {
                Layout.fillWidth: true
                visible: root.job.kind === "install"
                kind: "info"
                message: qsTr("Some tools install a system service that starts right away. Use everything here only on systems you own or are authorised in writing to test.")
            }
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(220, listText.implicitHeight + Theme.spaceMd * 2)
                radius: Theme.radiusSm
                color: Theme.bgInput
                border.width: Theme.borderWidth
                border.color: Theme.borderSubtle
                clip: true
                Flickable {
                    anchors.fill: parent
                    anchors.margins: Theme.spaceMd
                    contentHeight: listText.implicitHeight
                    boundsBehavior: Flickable.StopAtBounds
                    clip: true
                    ScrollBar.vertical: ScrollBar { }
                    TextEdit {
                        id: listText
                        width: parent.width - Theme.spaceMd
                        readOnly: true
                        selectByMouse: true
                        color: Theme.textSecondary
                        selectionColor: Theme.accentFill
                        selectedTextColor: Theme.textOnAccent
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fontSizeSmall
                        wrapMode: TextEdit.Wrap
                        Accessible.name: qsTr("Packages affected")
                        text: {
                            let parts = []
                            if (root.installList.length > 0)
                                parts.push(qsTr("Install:") + "\n" + root.names(root.installList))
                            if (root.updateList.length > 0)
                                parts.push(qsTr("Update:") + "\n" + root.names(root.updateList))
                            if (root.removeList.length > 0)
                                parts.push(qsTr("Remove:") + "\n" + root.names(root.removeList))
                            return parts.join("\n\n")
                        }
                    }
                }
            }
        }

        // ---- result --------------------------------------------------------------
        ColumnLayout {
            Layout.fillWidth: true
            visible: root.job.phase === "finished"
            spacing: Theme.spaceMd

            NxBanner {
                Layout.fillWidth: true
                kind: root.job.result === "success" ? "success" : root.job.result === "cancelled" ? "info" : "danger"
                title: root.job.result === "success" ? (root.job.kind === "install" ? qsTr("Installed") : qsTr("Removed"))
                     : root.job.result === "cancelled" ? qsTr("Cancelled") : root.job.errorTitle
                message: root.job.result === "failed" ? root.job.errorDetails
                       : root.job.note !== "" ? root.job.note
                       : root.job.result === "cancelled" ? qsTr("The operation was cancelled.") : qsTr("The operation completed.")
            }
            Text {
                Layout.fillWidth: true
                visible: root.job.result === "failed" && root.job.errorKey !== ""
                text: qsTr("PackageKit error: %1").arg(root.job.errorKey)
                color: Theme.textMuted
                font.family: Theme.fontMono
                font.pixelSize: Theme.fontSizeCaption
            }
            // The metapackage is not in the repositories: the tools still are.
            ColumnLayout {
                Layout.fillWidth: true
                visible: root.offerTools
                spacing: Theme.spaceSm
                Text {
                    Layout.fillWidth: true
                    text: qsTr("The profile's metapackage comes from the NEXUS security repository. Its tools are in Debian and can be installed directly instead.")
                    color: Theme.textSecondary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBody
                    wrapMode: Text.WordWrap
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceSm
            Item { Layout.fillWidth: true }
            NxButton {
                visible: root.job.active
                text: root.job.phase === "confirm" ? qsTr("Cancel") : qsTr("Cancel operation")
                enabled: root.job.cancellable
                onClicked: root.job.cancel()
            }
            NxButton {
                visible: root.job.phase === "confirm"
                text: root.job.kind === "install" ? qsTr("Install") : qsTr("Remove")
                kind: root.job.kind === "install" && root.removeList.length === 0 ? "primary" : "danger"
                iconName: root.job.kind === "install" ? "download" : "trash"
                onClicked: root.job.confirm()
            }
            NxButton {
                visible: root.offerTools
                text: qsTr("Install the tools directly…")
                kind: "primary"
                onClicked: {
                    const profileId = root.job.context.substring(8)
                    root.job.dismiss()
                    root.followUp("install-profile-tools", profileId)
                }
            }
            NxButton {
                visible: root.job.phase === "finished"
                text: qsTr("Close")
                onClicked: root.job.dismiss()
            }
        }
    }

    readonly property bool offerTools: job.phase === "finished" && job.result === "failed"
        && job.errorKey === "unavailable" && job.context.startsWith("profile:") && job.kind === "install"
}
