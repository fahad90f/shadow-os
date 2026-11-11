// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.installer

StepPage {
    id: page
    required property WizardState wiz

    property bool showLog: false
    readonly property bool hasPercent: Installer.percent >= 0

    function clock(seconds) {
        const h = Math.floor(seconds / 3600), m = Math.floor(seconds / 60) % 60, s = seconds % 60
        const two = n => (n < 10 ? "0" : "") + n
        return (h > 0 ? h + ":" + two(m) : m) + ":" + two(s)
    }

    title: Installer.simulation ? qsTr("Simulated installation (test engine)") : qsTr("Installing NEXUS OS")
    subtitle: Installer.simulation
              ? qsTr("The test engine is playing the events of an installation. No program runs and no disk is touched.")
              : qsTr("Keep the computer switched on and the installation medium connected. There is no cancel button: "
                   + "stopping now would leave the target disk unusable.")
    ready: false

    NxCard {
        Layout.fillWidth: true
        implicitHeight: status.implicitHeight + padding * 2
        ColumnLayout {
            id: status
            anchors.left: parent.left
            anchors.right: parent.right
            spacing: Theme.spaceSm
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceMd
                Text {
                    Layout.fillWidth: true
                    text: Installer.stepTotal > 0
                          ? qsTr("Step %1 of %2: %3").arg(Installer.stepIndex).arg(Installer.stepTotal)
                                .arg(Installer.steps.length > 0 ? Installer.steps[Installer.steps.length - 1].title : "")
                          : qsTr("Waiting for the engine to start…")
                    color: Theme.textPrimary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBodyLarge
                    font.weight: Theme.weightSemibold
                    elide: Text.ElideRight
                }
                Text {
                    visible: page.hasPercent
                    text: qsTr("%1 %").arg(Math.floor(Installer.percent))
                    color: Theme.textPrimary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBodyLarge
                    font.weight: Theme.weightSemibold
                }
            }
            ProgressBar {
                Layout.fillWidth: true
                from: 0; to: 100
                value: page.hasPercent ? Installer.percent : 0
                indeterminate: !page.hasPercent && Installer.installState === "running"
                Accessible.name: qsTr("Progress of the installation")
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceMd
                Text {
                    Layout.fillWidth: true
                    text: Installer.progressMessage !== "" ? Installer.progressMessage
                        : page.hasPercent ? "" : qsTr("The engine has not reported a percentage yet.")
                    color: Theme.textSecondary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                    elide: Text.ElideRight
                }
                Text {
                    text: qsTr("Elapsed %1").arg(page.clock(Installer.elapsedSeconds))
                    color: Theme.textSecondary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                }
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spaceSm
        SectionTitle { text: page.showLog ? qsTr("Details") : qsTr("Steps") }
        NxButton {
            kind: "ghost"
            text: page.showLog ? qsTr("Hide details") : qsTr("Show details")
            iconName: page.showLog ? "chevron-up" : "chevron-down"
            onClicked: page.showLog = !page.showLog
        }
    }

    Rectangle {
        Layout.fillWidth: true
        Layout.fillHeight: true
        visible: !page.showLog
        color: Theme.bgRaised
        radius: Theme.radiusMd
        border.width: Theme.borderWidth
        border.color: Theme.borderSubtle
        clip: true
        ListView {
            id: stepList
            anchors.fill: parent
            anchors.margins: Theme.spaceXs
            model: Installer.steps
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { }
            onCountChanged: positionViewAtEnd()
            delegate: RowLayout {
                id: stepRow
                required property var modelData
                width: ListView.view.width
                height: Theme.rowHeight - Theme.spaceSm
                spacing: Theme.spaceMd
                Item {
                    Layout.leftMargin: Theme.spaceMd
                    implicitWidth: Theme.icon
                    implicitHeight: Theme.icon
                    NxIcon {
                        anchors.centerIn: parent
                        visible: stepRow.modelData.state !== "active"
                        name: stepRow.modelData.state === "done" ? "check-circle" : "x-circle"
                        size: Theme.icon
                        color: stepRow.modelData.state === "done" ? Theme.success : Theme.danger
                    }
                    BusyIndicator {
                        anchors.centerIn: parent
                        width: Theme.icon + 4; height: Theme.icon + 4
                        padding: 0
                        visible: stepRow.modelData.state === "active"
                        running: visible
                    }
                }
                Text {
                    Layout.fillWidth: true
                    text: stepRow.modelData.title
                    color: stepRow.modelData.state === "active" ? Theme.textPrimary : Theme.textSecondary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBody
                    font.weight: stepRow.modelData.state === "active" ? Theme.weightSemibold : Theme.weightRegular
                    elide: Text.ElideRight
                }
            }
            Text {
                anchors.centerIn: parent
                visible: stepList.count === 0
                text: qsTr("The engine announces each step as it begins.")
                color: Theme.textMuted
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
            }
        }
    }

    LogView {
        visible: page.showLog
    }
}
