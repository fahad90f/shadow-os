// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.nexus.ui

// Root of the NEXUS HORIZON shell: creates the surfaces of every screen and
// routes global shortcuts.
QtObject {
    id: shell

    readonly property var primaryScreen: Qt.application.screens.length > 0 ? Qt.application.screens[0] : null

    function toggleLauncher() {
        if (launcher.open) launcher.hide()
        else { sessionMenu.hide(); dockMenu.hide(); launcher.show() }
    }

    function toggleSessionMenu() {
        if (sessionMenu.open) sessionMenu.hide()
        else { launcher.hide(); dockMenu.hide(); sessionMenu.show() }
    }

    // What a named action (global shortcut, panel button) does.
    function trigger(name) {
        switch (name) {
        case "launcher": toggleLauncher(); break
        case "session-menu": toggleSessionMenu(); break
        case "terminal": session.run("nexus-terminal"); break
        case "files": session.run("nexus-files"); break
        case "settings": session.run("nexus-control-center"); break
        case "task-manager": session.run("nexus-monitor", ["--page", "processes"]); break
        case "screenshot": session.run("nexus-screenshot"); break
        case "screenshot-region": session.run("nexus-screenshot", ["--region"]); break
        default: shell.unhandledAction(name)
        }
    }

    // Emitted for actions owned by optional modules (volume, brightness,
    // media keys, quick settings, notifications).
    signal unhandledAction(string name)

    Component.onCompleted: {
        const request = session.takeAutostartRequest()
        if (request === "installer") session.run("nexus-installer")
        else if (request === "recovery") session.run("nexus-recovery")
    }

    property Connections shortcutConnection: Connections {
        target: shortcuts
        function onTriggered(name) { shell.trigger(name) }
    }

    property Connections sessionErrors: Connections {
        target: session
        function onActionFailed(action, error) { toast.show(error) }
    }
    property Connections launchErrors: Connections {
        target: applications
        function onLaunchFailed(name, error) { toast.show(qsTr("%1 could not be started: %2").arg(name).arg(error)) }
    }

    // ---- per-screen wallpaper -------------------------------------------------
    property Instantiator wallpapers: Instantiator {
        model: Qt.application.screens
        delegate: Wallpaper {
            required property var modelData
            screen: modelData
        }
    }

    // ---- primary screen: panel and dock ------------------------------------------
    property Panel panel: Panel {
        screen: shell.primaryScreen
        launcherOpen: launcher.open
        sessionMenuOpen: sessionMenu.open
        onLauncherRequested: shell.toggleLauncher()
        onSessionMenuRequested: shell.toggleSessionMenu()
    }

    property Dock dockBar: Dock {
        screen: shell.primaryScreen
        onMenuRequested: (index, centerX) => dockMenu.openFor(index, centerX)
    }

    // ---- overlays ---------------------------------------------------------------------
    property Launcher launcher: Launcher {
        screen: shell.primaryScreen
        onSessionMenuRequested: shell.toggleSessionMenu()
    }

    property SessionMenu sessionMenu: SessionMenu {
        screen: shell.primaryScreen
    }

    property DockMenu dockMenu: DockMenu {
        screen: shell.primaryScreen
        bottomOffset: dockBar.barHeight + dockBar.bottomGap + Theme.spaceSm
    }

    property AuthDialog authDialog: AuthDialog {
        screen: shell.primaryScreen
    }

    // ---- transient messages from the shell itself ------------------------------------
    property ShellSurface toast: ShellSurface {
        id: toast
        screen: shell.primaryScreen
        layer: "overlay"
        scope: "nexus-shell-toast"
        anchorTop: true
        width: Math.min(560, (screen ? screen.width : 1280) - 64)
        height: toastText.implicitHeight + Theme.spaceMd * 2 + Theme.spaceLg
        visible: false

        function show(message) {
            toastText.text = message
            visible = true
            toastTimer.restart()
        }

        Timer { id: toastTimer; interval: 6000; onTriggered: toast.visible = false }

        Rectangle {
            anchors.fill: parent
            anchors.topMargin: Theme.spaceLg
            radius: Theme.radiusMd
            color: Theme.bgOverlay
            border.width: Theme.borderWidth
            border.color: Theme.alpha(Theme.danger, 0.6)
            Text {
                id: toastText
                anchors.fill: parent
                anchors.margins: Theme.spaceMd
                color: Theme.textPrimary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
                wrapMode: Text.WordWrap
                verticalAlignment: Text.AlignVCenter
            }
            TapHandler { onTapped: toast.visible = false }
        }
    }
}
