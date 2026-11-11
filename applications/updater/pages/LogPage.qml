// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.updater

// The detailed log of the most recent transaction this application ran: the
// one in progress, the last one of this session, or the one saved from an
// earlier session.
NxPage {
    id: page

    required property QtObject updater
    readonly property QtObject job: updater.job
    readonly property bool live: job !== null
    readonly property string logText: live ? job.logText : updater.lastLog
    readonly property string logTitle: live ? job.title : updater.lastLogTitle

    signal copyRequested(string text, string what)

    title: qsTr("Last transaction")
    subtitle: logText === "" ? qsTr("No transaction has been run from NEXUS Updater yet.")
        : live && job.active ? qsTr("In progress: %1").arg(logTitle)
        : live ? qsTr("%1 — result: %2").arg(logTitle).arg(job.result)
        : qsTr("Saved from an earlier session: %1").arg(logTitle)
    maxContentWidth: 980
    actions: [
        NxButton {
            text: qsTr("Copy log")
            iconName: "copy"
            enabled: page.logText !== ""
            onClicked: page.copyRequested(page.logText, qsTr("Log"))
        },
        NxButton {
            text: qsTr("Copy details for a bug report")
            iconName: "bug"
            kind: "primary"
            onClicked: page.copyRequested(page.updater.report(), qsTr("Report"))
        }
    ]

    NxBanner {
        visible: page.live && page.job.result === "failed"
        kind: "danger"
        title: page.live ? page.job.errorTitle : ""
        message: page.live ? qsTr("PackageKit error code: %1").arg(page.job.errorKey) + "\n" + page.job.errorDetails : ""
    }

    NxCard {
        Layout.fillWidth: true
        visible: page.logText !== ""
        implicitHeight: Math.max(240, logEdit.implicitHeight + Theme.spaceLg * 2)
        TextEdit {
            id: logEdit
            width: parent.width
            text: page.logText
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

    NxCard {
        Layout.fillWidth: true
        visible: page.logText === ""
        implicitHeight: 260
        NxEmptyState {
            anchors.fill: parent
            icon: "file-text"
            title: qsTr("No transaction log yet")
            message: qsTr("When updates are installed or the package system is repaired from here, every step PackageKit reports is recorded on this page and kept in %1.").arg(page.updater.lastLogPath)
        }
    }

    NxSection {
        title: qsTr("Where else to look")
        NxRow { icon: "file-text"; title: qsTr("APT history"); subtitle: qsTr("What was installed, upgraded and removed — shown on the History page"); value: "/var/log/apt/history.log"; monoValue: true }
        NxRow { icon: "terminal"; title: qsTr("Package script output"); subtitle: qsTr("Output of dpkg and package maintainer scripts; readable by administrators"); value: "/var/log/apt/term.log"; monoValue: true }
        NxRow { icon: "list"; title: qsTr("dpkg log"); subtitle: qsTr("Every state change of every package"); value: "/var/log/dpkg.log"; monoValue: true }
    }
}
