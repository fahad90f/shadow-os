// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.installer

StepPage {
    id: page
    required property WizardState wiz

    readonly property var report: Installer.probe
    readonly property bool ok: Installer.probeState === "ready"
    readonly property var checks: report.checks || []
    readonly property var failures: checks.filter(c => c.status === "fail")
    readonly property var cautions: checks.filter(c => c.status === "warn")

    title: qsTr("System check")
    subtitle: qsTr("The installation engine examines this computer. This only reads; nothing is changed.")
    ready: ok && report.can_install === true
    blockReason: Installer.probeState === "running" ? qsTr("Examining this computer…")
               : !ok ? qsTr("The system check has not succeeded.")
               : !ready ? qsTr("Blocking problems must be solved before Shadow OS can be installed.") : ""

    function entered() {
        if (Installer.probeState === "idle")
            Installer.runProbe(false)
    }

    headerActions: NxButton {
        text: qsTr("Check again")
        iconName: "refresh"
        visible: Installer.probeState === "ready" || Installer.probeState === "failed"
        onClicked: Installer.runProbe(false)
    }

    function glyph(status) { return status === "pass" ? "check-circle" : status === "warn" ? "alert-triangle" : "x-circle" }
    function tone(status) { return status === "pass" ? Theme.success : status === "warn" ? Theme.warning : Theme.danger }
    function checkIcon(id) {
        return ({ arch: "cpu", firmware: "settings", secureboot: "shield", efivars: "settings", memory: "memory",
                  disk: "harddrive", medium: "archive", power: "plug", network: "network" })[id] || "info"
    }
    function diskLine(d) {
        const name = [d.vendor, d.model].filter(x => x).join(" ") || qsTr("Unknown model")
        return name + " · " + Theme.formatBytes(d.size) + (d.transport ? " · " + d.transport.toUpperCase() : "")
    }

    ColumnLayout {
        Layout.fillWidth: true
        Layout.fillHeight: true
        visible: Installer.probeState === "running" || Installer.probeState === "idle"
        spacing: Theme.spaceMd
        Item { Layout.fillHeight: true }
        BusyIndicator { Layout.alignment: Qt.AlignHCenter; running: parent.visible }
        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("Examining this computer…")
            color: Theme.textPrimary
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeTitle
        }
        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: Installer.testMode ? qsTr("Reading the synthetic hardware description.")
                : Installer.liveSession ? qsTr("Reading firmware, memory, disks, power and network.")
                : qsTr("The engine needs administrator rights; an authentication dialog may appear.")
            color: Theme.textMuted
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeBody
        }
        Item { Layout.fillHeight: true }
    }

    ColumnLayout {
        Layout.fillWidth: true
        Layout.fillHeight: true
        visible: Installer.probeState === "failed"
        spacing: Theme.spaceMd
        NxEmptyState {
            Layout.fillWidth: true
            Layout.fillHeight: true
            icon: "x-circle"
            tone: "danger"
            title: qsTr("This computer could not be examined")
            message: Installer.probeError
            actionText: qsTr("Try again")
            onActionTriggered: Installer.runProbe(false)
        }
        Rectangle {
            Layout.fillWidth: true
            visible: Installer.probeErrorDetail !== ""
            implicitHeight: Math.min(detail.implicitHeight + Theme.spaceMd * 2, 150)
            radius: Theme.radiusMd
            color: Theme.bgInput
            border.width: Theme.borderWidth
            border.color: Theme.borderSubtle
            clip: true
            TextEdit {
                id: detail
                anchors.fill: parent
                anchors.margins: Theme.spaceMd
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.Wrap
                text: Installer.probeErrorDetail
                color: Theme.textSecondary
                font.family: Theme.fontMono
                font.pixelSize: Theme.fontSizeSmall
                Accessible.name: qsTr("Details reported by the engine")
            }
        }
    }

    ScrollColumn {
        visible: page.ok

        NxBanner {
            kind: page.failures.length > 0 ? "danger" : page.cautions.length > 0 ? "warning" : "success"
            title: page.failures.length > 0 ? qsTr("Shadow OS cannot be installed on this computer as it is")
                 : page.cautions.length > 0 ? (page.cautions.length === 1 ? qsTr("This computer can run the installation — one point to note") : qsTr("This computer can run the installation — %1 points to note").arg(page.cautions.length))
                 : qsTr("This computer meets all requirements")
            message: page.failures.length > 0
                     ? (page.failures.length === 1 ? qsTr("One blocking problem below. Solve it and choose “Check again”.") : qsTr("%1 blocking problems below. Solve them and choose “Check again”.").arg(page.failures.length))
                     : page.cautions.length > 0 ? qsTr("Warnings do not prevent the installation.") : ""
        }

        NxSection {
            title: qsTr("Requirements")
            Repeater {
                model: page.checks
                NxRow {
                    required property var modelData
                    icon: page.checkIcon(modelData.id)
                    title: modelData.title
                    subtitle: modelData.detail
                    NxBadge {
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.status === "pass" ? qsTr("OK") : modelData.status === "warn" ? qsTr("Warning") : qsTr("Blocking")
                        kind: modelData.status === "pass" ? "success" : modelData.status === "warn" ? "warning" : "danger"
                    }
                }
            }
        }

        NxSection {
            title: qsTr("Disks")
            description: qsTr("The target is chosen in a later step.")
            Repeater {
                model: page.report.disks || []
                NxRow {
                    required property var modelData
                    icon: modelData.transport === "usb" ? "usb" : "harddrive"
                    title: page.diskLine(modelData)
                    subtitle: modelData.usable ? "" : modelData.unusable_reason
                    value: modelData.path
                    monoValue: true
                    NxBadge {
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.is_live_medium ? qsTr("Installation medium") : modelData.usable ? qsTr("Usable") : qsTr("Not usable")
                        kind: modelData.usable ? "success" : "neutral"
                    }
                }
            }
            NxRow {
                visible: page.ok && (page.report.disks || []).length === 0
                icon: "harddrive"
                title: qsTr("The engine found no disk")
            }
        }
    }
}
