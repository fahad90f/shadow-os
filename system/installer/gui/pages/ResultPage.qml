// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.installer

StepPage {
    id: page
    required property WizardState wiz

    signal backToSummary()

    readonly property bool success: Installer.installState === "succeeded"
    readonly property bool wroteToDisk: Installer.steps.length > 0
    readonly property string failedStep: {
        for (const s of Installer.steps)
            if (s.state === "failed") return s.title
        return ""
    }
    property bool showLog: false

    function clock(seconds) {
        const m = Math.floor(seconds / 60), s = seconds % 60
        return m + ":" + (s < 10 ? "0" : "") + s
    }

    title: success ? (Installer.simulation ? qsTr("The simulation finished") : qsTr("NEXUS OS is installed"))
                   : (Installer.simulation ? qsTr("The simulated installation failed") : qsTr("The installation failed"))
    subtitle: success
              ? (Installer.simulation ? qsTr("The test engine reported success. Nothing was installed.")
                                      : (Installer.doneMessage !== "" ? Installer.doneMessage : qsTr("The engine reported that the installation is complete.")))
              : (page.failedStep !== "" ? qsTr("The engine stopped in the step “%1”.").arg(page.failedStep)
                                        : qsTr("The engine stopped before its first step."))
    ready: false

    ScrollColumn {
        visible: !page.showLog

        NxBanner {
            visible: page.success
            kind: "success"
            title: Installer.simulation ? qsTr("Simulation complete after %1").arg(page.clock(Installer.elapsedSeconds))
                                        : qsTr("Installation complete after %1").arg(page.clock(Installer.elapsedSeconds))
            message: Installer.simulation ? ""
                   : qsTr("Restart, and remove the installation medium when the screen goes dark. "
                        + "The installation log is on the new system in /var/log/installer/.")
        }
        Repeater {
            model: Installer.warnings
            NxBanner {
                required property string modelData
                kind: "warning"
                title: qsTr("The engine reported a warning")
                message: modelData
            }
        }

        NxBanner {
            visible: !page.success
            kind: "danger"
            title: Installer.errorMessage.split("\n")[0]
            message: Installer.errorMessage.split("\n").slice(1).join("\n")
        }
        NxBanner {
            visible: !page.success && Installer.errorHint !== ""
            kind: "info"
            message: Installer.errorHint
        }
        NxSection {
            visible: !page.success
            NxRow {
                icon: "harddrive"
                title: page.wroteToDisk ? qsTr("The target disk may be partly written") : qsTr("No installation step had begun")
                subtitle: page.wroteToDisk
                          ? qsTr("The installation stopped part-way. Do not expect the target to start; the engine's own messages (Details) say what it undid.")
                          : qsTr("The engine reported no step, so it did not start partitioning or formatting.")
            }
            NxRow {
                icon: "file-text"
                title: qsTr("Engine log")
                subtitle: Installer.engineLogPath !== ""
                          ? qsTr("Written by the engine in this live session; readable by root. After a successful installation it is copied to /var/log/installer/ on the new system.")
                          : qsTr("The engine did not name a log file. It normally writes one to /var/log/nexus-installer/ in this session (readable by root); its messages from this run are under “Details”. After a successful installation the log is kept in /var/log/installer/ on the new system.")
                value: Installer.engineLogPath
                monoValue: true
            }
        }
    }

    LogView { visible: page.showLog }

    Text {
        Layout.fillWidth: true
        visible: Installer.restartError !== ""
        text: Installer.restartError
        color: Theme.danger
        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeSmall
        wrapMode: Text.WordWrap
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spaceSm
        NxButton {
            text: page.showLog ? qsTr("Hide details") : qsTr("Show details")
            iconName: page.showLog ? "chevron-up" : "chevron-down"
            onClicked: page.showLog = !page.showLog
        }
        NxButton {
            text: qsTr("Copy log")
            iconName: "copy"
            onClicked: { Installer.copyLog(); ApplicationWindow.window.toaster.show(qsTr("The messages of this run were copied to the clipboard"), "success") }
        }
        Item { Layout.fillWidth: true }
        NxButton {
            visible: !page.success
            text: qsTr("Back to the summary")
            iconName: "arrow-left"
            onClicked: page.backToSummary()
        }
        NxButton {
            text: page.success ? (Installer.liveSession ? qsTr("Keep using the live session") : qsTr("Close the installer")) : qsTr("Close the installer")
            onClicked: Qt.quit()
        }
        NxButton {
            visible: page.success
            kind: "primary"
            text: qsTr("Restart now")
            iconName: "power"
            busy: Installer.restarting
            onClicked: Installer.restart()
        }
    }
}
