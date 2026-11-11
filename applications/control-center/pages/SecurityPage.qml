// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.controlcenter

NxPage {
    id: page
    title: qsTr("Security")
    subtitle: qsTr("What the protection mechanisms of this computer report right now. This page changes nothing.")

    function severityKind(severity) {
        return severity === "high" ? "danger" : severity === "medium" ? "warning" : "neutral"
    }
    function severityText(severity) {
        return severity === "high" ? qsTr("High") : severity === "medium" ? qsTr("Medium") : severity === "low" ? qsTr("Low") : severity
    }
    function toneColor(tone) {
        return tone === "success" ? Theme.success : tone === "warning" ? Theme.warning : tone === "danger" ? Theme.danger : Theme.textSecondary
    }
    // What a recommendation's "action" hint can lead to from here.
    function actionLabel(action) {
        if (action === "firewall-enable" && page.ApplicationWindow.window.pageFor("firewall") !== null) return qsTr("Firewall settings")
        if (action === "open-updater" && security.updaterAvailable) return qsTr("Open SHADOW Updater")
        return ""
    }
    function runAction(action) {
        if (action === "firewall-enable") page.ApplicationWindow.window.openPage("firewall")
        else if (action === "open-updater" && !security.openUpdater())
            page.ApplicationWindow.window.toaster.show(qsTr("Could not start SHADOW Updater"), "danger")
    }

    actions: [
        NxButton {
            visible: security.securityCenterAvailable
            text: qsTr("Open Security Center")
            iconName: "external-link"
            onClicked: if (!security.openSecurityCenter()) page.ApplicationWindow.window.toaster.show(qsTr("Could not start the Security Center"), "danger")
        },
        NxButton {
            text: qsTr("Check again")
            iconName: "refresh"
            busy: security.loading
            onClicked: security.refresh()
        }
    ]

    // One line of the report: name, the state the report gives, and its evidence.
    component ReportRow: Item {
        id: row
        property string icon: ""
        property string tone: "neutral"
        property string title: ""
        property string badge: ""
        property var lines: []
        property string actionText: ""
        signal actionTriggered()
        readonly property bool isFirst: parent && parent.children.length > 0 && parent.children[0] === row
        Layout.fillWidth: true
        implicitHeight: body.implicitHeight + Theme.spaceMd * 2
        Accessible.role: Accessible.StaticText
        Accessible.name: title + ": " + badge
        Accessible.description: lines.join(". ")

        Rectangle {
            visible: !row.isFirst
            anchors { top: parent.top; left: parent.left; right: parent.right; leftMargin: Theme.spaceLg }
            height: 1
            color: Theme.borderSubtle
        }
        RowLayout {
            id: body
            anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter }
            anchors.leftMargin: Theme.spaceLg
            anchors.rightMargin: Theme.spaceLg
            spacing: Theme.spaceMd

            NxIcon {
                Layout.alignment: Qt.AlignTop
                Layout.topMargin: 2
                name: row.icon
                size: Theme.icon
                color: page.toneColor(row.tone)
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceXs
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceMd
                    Label {
                        Layout.fillWidth: true
                        text: row.title
                        font.weight: Theme.weightMedium
                        wrapMode: Text.Wrap
                    }
                    NxBadge {
                        Layout.alignment: Qt.AlignTop
                        text: row.badge
                        kind: row.tone
                        dot: row.tone !== "neutral"
                    }
                }
                Repeater {
                    model: row.lines
                    Label {
                        required property string modelData
                        Layout.fillWidth: true
                        text: modelData
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSizeSmall
                        wrapMode: Text.Wrap
                    }
                }
                NxButton {
                    Layout.topMargin: Theme.spaceXs
                    visible: row.actionText !== ""
                    text: row.actionText
                    onClicked: row.actionTriggered()
                }
            }
        }
    }

    SecurityBackend { id: security }

    NxEmptyState {
        visible: security.ready && !security.available && !security.loading
        Layout.fillWidth: true
        tone: "warning"
        icon: "shield-alert"
        title: qsTr("The security state could not be read")
        message: security.statusMessage
        actionText: qsTr("Try again")
        onActionTriggered: security.refresh()
    }

    NxCard {
        visible: security.loading && !security.available
        Layout.fillWidth: true
        implicitHeight: 96
        RowLayout {
            anchors.fill: parent
            spacing: Theme.spaceLg
            BusyIndicator { running: parent.visible }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textSecondary
                text: qsTr("Examining the system. Looking for pending updates can take a minute.")
            }
        }
    }

    NxBanner {
        visible: security.available && !security.privileged
        kind: "warning"
        title: qsTr("This report is incomplete")
        message: qsTr("It was produced without administrator rights (%1), so the firewall rules, AppArmor profiles, other users' network services and the security logs could not be read.")
            .arg(security.limitedReason !== "" ? security.limitedReason : qsTr("the tool did not run as root"))
    }

    NxSection {
        visible: security.available
        title: qsTr("Recommendations")
        description: qsTr("Findings of shadow-security-status, most severe first.")

        Repeater {
            model: security.recommendations
            ReportRow {
                required property var modelData
                icon: modelData.severity === "low" ? "info" : "alert-triangle"
                tone: page.severityKind(modelData.severity)
                title: modelData.title
                badge: page.severityText(modelData.severity)
                lines: modelData.detail !== "" ? [modelData.detail] : []
                actionText: page.actionLabel(modelData.action)
                onActionTriggered: page.runAction(modelData.action)
            }
        }
        NxRow {
            visible: security.recommendations.length === 0
            icon: "check-circle"
            iconColor: security.privileged ? Theme.success : Theme.textSecondary
            title: qsTr("The report contains no recommendations")
            subtitle: security.privileged ? "" : qsTr("Checks that need administrator rights were skipped.")
        }
    }

    NxSection {
        visible: security.available
        title: qsTr("State")
        description: qsTr("Examined %1.").arg(security.generated.toLocaleString(Qt.locale(), Locale.ShortFormat))

        Repeater {
            model: security.checks
            ReportRow {
                required property var modelData
                icon: modelData.icon
                tone: modelData.tone
                title: modelData.title
                badge: modelData.state
                lines: modelData.evidence
            }
        }
    }
}
