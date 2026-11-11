// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.updater

NxWindow {
    id: window
    title: qsTr("SHADOW Updater")
    width: 1080
    height: 760

    Updater { id: backend }

    property string currentKey: "updates"

    readonly property var pages: [
        { key: "updates",  title: qsTr("Updates"),          icon: "arrow-up-circle", section: qsTr("System updates"),
          badge: backend.updates.count > 0 ? String(backend.updates.count) : "" },
        { key: "history",  title: qsTr("History"),          icon: "clock",           section: qsTr("System updates"), badge: "" },
        { key: "log",      title: qsTr("Last transaction"), icon: "file-text",       section: qsTr("System updates"), badge: "" },
        { key: "recovery", title: qsTr("Recovery"),         icon: "life-buoy",       section: qsTr("Maintenance"),
          badge: backend.healthKnown && !backend.healthy ? "!" : "" },
        { key: "sources",  title: qsTr("Software sources"), icon: "shield-check",    section: qsTr("Maintenance"), badge: "" },
        { key: "settings", title: qsTr("Settings"),         icon: "settings",        section: qsTr("Maintenance"), badge: "" },
    ]

    function indexOfPage(key) {
        for (let i = 0; i < pages.length; i++) if (pages[i].key === key) return i
        return -1
    }
    function openPage(key) {
        if (indexOfPage(key) < 0) return false
        currentKey = key
        return true
    }
    function copy(text, what) {
        backend.copyToClipboard(text)
        toaster.show(qsTr("%1 copied to the clipboard").arg(what), "success")
    }

    Component.onCompleted: {
        const args = shadowApp.arguments
        if (args.length > 0) openPage(args[0])
        backend.initialize()
    }

    Connections {
        target: shadowApp
        function onActivationRequested(args) {
            if (args.length > 0) window.openPage(args[0])
            window.raise()
            window.requestActivate()
        }
    }

    Connections {
        target: backend
        function onRestartFailed(message) { window.toaster.show(message, "danger") }
        function onJobFinished(success, kind) {
            if (success && kind === "update") window.toaster.show(qsTr("Updates installed"), "success")
            else if (success && kind === "repair") window.toaster.show(qsTr("Package system repaired"), "success")
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        NxSidebar {
            Layout.fillHeight: true
            title: qsTr("Updater")
            model: window.pages
            currentKey: window.currentKey
            onActivated: (key) => window.openPage(key)
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: window.indexOfPage(window.currentKey)

            UpdatesPage {
                updater: backend
                onOpenPage: (key) => window.openPage(key)
                onRestartRequested: restartDialog.open()
                onCopyRequested: (text) => window.copy(text, qsTr("Details"))
            }
            HistoryPage { updater: backend }
            LogPage {
                updater: backend
                onCopyRequested: (text, what) => window.copy(text, what)
            }
            RecoveryPage {
                updater: backend
                onCopyRequested: (text) => window.copy(text, qsTr("Details"))
            }
            SourcesPage { updater: backend }
            SettingsPage { updater: backend }
        }
    }

    // Questions PackageKit raises during a transaction (changes, untrusted
    // packages, licence agreements, signing keys, media).
    PkJobDialogs {
        anchors.fill: parent
        job: backend.job
    }

    NxConfirmDialog {
        id: restartDialog
        title: qsTr("Restart the computer now?")
        message: qsTr("All applications are closed and unsaved work is lost. %1").arg(backend.restartReason)
        confirmText: qsTr("Restart now")
        destructive: true
        onConfirmed: backend.restartNow()
    }
}
