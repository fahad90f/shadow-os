// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.installer

// The engine's plan for the choices made — its words, not a paraphrase — and
// the confirmation without which nothing is written.
StepPage {
    id: page
    required property WizardState wiz

    readonly property bool planned: Installer.planState === "ready" && Installer.plan.steps !== undefined
    readonly property var plan: Installer.plan
    readonly property var steps: plan.steps || []
    readonly property bool destructive: steps.some(s => s.destructive === true)
    // The name the user has to type: the disk that is erased, or — when
    // existing partitions are formatted — the first of those partitions.
    readonly property string deviceToType: {
        if (!planned || !destructive) return ""
        if (plan.mode !== "erase-disk") {
            for (const p of (plan.partitions || []))
                if (p.format && !p.create && p.device) return p.device
        }
        return plan.target_disk || ""
    }
    readonly property bool mayInstall: Installer.simulation || Installer.installBlockedReason === ""
    property bool acknowledged: false

    title: qsTr("Summary")
    subtitle: qsTr("This is exactly what the installation engine will do. Nothing has been written so far.")
    ready: planned && mayInstall && acknowledged && (deviceToType === "" || typed.text.trim() === deviceToType)
    blockReason: Installer.planState === "running" ? qsTr("The engine is working out the plan…")
               : !planned ? qsTr("There is no valid plan.")
               : !mayInstall ? qsTr("Installing is not possible in this session (see above).")
               : !acknowledged ? qsTr("Tick the acknowledgement to continue.")
               : !ready ? qsTr("Type %1 to confirm.").arg(deviceToType) : ""

    function refresh() {
        acknowledged = false
        typed.clear()
        Installer.requestPlan(wiz.answers())
    }
    function entered() { refresh() }

    Connections {
        target: Installer
        function onPlanChanged() { page.acknowledged = false; typed.clear() }
    }

    ScrollColumn {
        id: scroll

        NxBanner {
            visible: page.wiz.planChangedNotice !== ""
            kind: "warning"
            title: qsTr("The installation did not start: this computer changed after the plan was shown")
            message: page.wiz.planChangedNotice
        }
        NxBanner {
            visible: !page.mayInstall
            kind: Installer.testMode ? "info" : "danger"
            title: Installer.testMode ? qsTr("Test mode — this plan cannot be carried out") : qsTr("Installing is refused in this session")
            message: Installer.installBlockedReason
        }
        NxBanner {
            visible: Installer.simulation
            kind: "info"
            title: qsTr("Test mode — “Install” starts a simulation")
            message: qsTr("The test engine plays the events of an installation. No program is run and no disk is touched.")
        }

        ColumnLayout {
            Layout.fillWidth: true
            visible: Installer.planState === "running" || Installer.planState === "idle"
            spacing: Theme.spaceMd
            Item { implicitHeight: Theme.spaceXxl }
            BusyIndicator { Layout.alignment: Qt.AlignHCenter; running: parent.visible }
            Text {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("The engine is working out the plan…")
                color: Theme.textSecondary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBodyLarge
            }
        }

        NxBanner {
            visible: Installer.planState === "failed"
            kind: "danger"
            title: Installer.planError
            message: Installer.planHint
            actionText: qsTr("Try again")
            onActionTriggered: page.refresh()
        }
        Text {
            Layout.fillWidth: true
            visible: Installer.planState === "failed"
            wrapMode: Text.WordWrap
            text: qsTr("No plan could be made, so nothing can be installed with these choices. Go back, change the step concerned and return here.")
            color: Theme.textSecondary
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeBody
        }

        // ------------------------------------------------------------ the plan
        ColumnLayout {
            Layout.fillWidth: true
            visible: page.planned
            spacing: Theme.spaceSm
            SectionTitle {
                text: qsTr("Operations, in this order")
                detail: page.destructive ? qsTr("Steps marked “Destroys data” cannot be undone.")
                                         : qsTr("No step of this plan destroys existing data.")
            }
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: stepColumn.implicitHeight
                color: Theme.bgRaised
                radius: Theme.radiusMd
                border.width: Theme.borderWidth
                border.color: Theme.borderSubtle
                ColumnLayout {
                    id: stepColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    spacing: 0
                    Repeater {
                        model: page.steps
                        Item {
                            id: stepItem
                            required property var modelData
                            required property int index
                            Layout.fillWidth: true
                            implicitHeight: stepBody.implicitHeight + Theme.spaceMd * 2
                            Rectangle {
                                visible: stepItem.index > 0
                                anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
                                anchors.leftMargin: Theme.spaceLg
                                height: 1; color: Theme.borderSubtle
                            }
                            RowLayout {
                                id: stepBody
                                anchors.left: parent.left; anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.leftMargin: Theme.spaceLg; anchors.rightMargin: Theme.spaceLg
                                spacing: Theme.spaceMd
                                Text {
                                    Layout.alignment: Qt.AlignTop
                                    Layout.preferredWidth: 22
                                    Layout.topMargin: 2
                                    horizontalAlignment: Text.AlignRight
                                    text: (stepItem.index + 1) + "."
                                    color: Theme.textMuted
                                    font.family: Theme.fontUi
                                    font.pixelSize: Theme.fontSizeBody
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.spaceXs
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: Theme.spaceSm
                                        Text {
                                            Layout.fillWidth: true
                                            text: stepItem.modelData.title
                                            color: Theme.textPrimary
                                            font.family: Theme.fontUi
                                            font.pixelSize: Theme.fontSizeBody
                                            font.weight: Theme.weightSemibold
                                            wrapMode: Text.WordWrap
                                        }
                                        NxBadge {
                                            visible: stepItem.modelData.destructive === true
                                            text: qsTr("Destroys data")
                                            kind: "danger"
                                        }
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        visible: stepItem.modelData.details.length > 0
                                        text: stepItem.modelData.details.join("\n")
                                        color: stepItem.modelData.destructive === true ? Theme.textPrimary : Theme.textSecondary
                                        font.family: Theme.fontUi
                                        font.pixelSize: Theme.fontSizeSmall
                                        wrapMode: Text.WordWrap
                                        lineHeight: 1.15
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        Repeater {
            model: page.plan.warnings || []
            NxBanner {
                required property string modelData
                kind: "warning"
                message: modelData
            }
        }

        // ----------------------------------------------------------- confirm
        NxCard {
            Layout.fillWidth: true
            visible: page.planned
            accentBorder: !page.destructive
            border.color: page.destructive ? Theme.alpha(Theme.danger, 0.6) : Theme.alpha(Theme.accent, 0.55)
            implicitHeight: confirm.implicitHeight + padding * 2
            ColumnLayout {
                id: confirm
                anchors.left: parent.left
                anchors.right: parent.right
                spacing: Theme.spaceMd
                CheckBox {
                    id: ack
                    Layout.fillWidth: true
                    checked: page.acknowledged
                    enabled: page.mayInstall
                    text: page.destructive
                          ? qsTr("I have read the plan above and understand that the data it destroys cannot be recovered.")
                          : qsTr("I have read the plan above and want it carried out.")
                    onToggled: page.acknowledged = checked
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: page.deviceToType !== ""
                    spacing: Theme.spaceMd
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("To confirm, type the name of the device that loses its data: %1").arg(page.deviceToType)
                        color: Theme.textSecondary
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeBody
                        wrapMode: Text.WordWrap
                    }
                    TextField {
                        id: typed
                        Layout.preferredWidth: 220
                        enabled: page.mayInstall
                        font.family: Theme.fontMono
                        placeholderText: page.deviceToType
                        Accessible.name: qsTr("Device name to confirm")
                        inputMethodHints: Qt.ImhNoAutoUppercase | Qt.ImhNoPredictiveText
                    }
                }
            }
        }

        Text {
            Layout.fillWidth: true
            visible: page.planned
            wrapMode: Text.WrapAnywhere
            color: Theme.textMuted
            font.family: Theme.fontMono
            font.pixelSize: Theme.fontSizeCaption
            text: qsTr("Plan %1").arg(page.plan.hash || "")
                  + (Installer.answersFile !== "" ? "\n" + qsTr("Choices without the passwords: %1").arg(Installer.answersFile) : "")
        }
    }
}
