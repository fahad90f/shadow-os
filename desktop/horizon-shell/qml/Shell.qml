// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.nexus.ui

// Root of the NEXUS HORIZON shell: creates the surfaces of every screen and
// routes global shortcuts.
QtObject {
    id: shell

    readonly property var primaryScreen: Qt.application.screens.length > 0 ? Qt.application.screens[0] : null

    // ---- overlays: one at a time ------------------------------------------------
    // The overlay shown last (launcher, a menu, a module's popup). Presenting
    // another one hides it.
    property var activeOverlay: null
    // Overlays that can be opened by name: by a global shortcut, a panel
    // button or another module. Modules register theirs with registerOverlay().
    property var namedOverlays: ({})
    property var overlayNames: []
    readonly property string openOverlayName: activeOverlay && activeOverlay.open ? activeOverlay.overlayName : ""

    function present(overlay) {
        if (activeOverlay && activeOverlay !== overlay) activeOverlay.hide()
        activeOverlay = overlay
        overlay.show()
    }
    function toggle(overlay) {
        if (overlay.open) overlay.hide()
        else present(overlay)
    }
    function closeOverlays() {
        if (activeOverlay) activeOverlay.hide()
    }
    function registerOverlay(name, overlay) {
        namedOverlays[name] = overlay
        overlay.overlayName = name
        overlayNames = Object.keys(namedOverlays)
    }
    function hasOverlay(name) { return overlayNames.indexOf(name) >= 0 }

    // What a named action (global shortcut, panel button) does. Actions the
    // core does not know open the overlay registered under that name, or are
    // passed to the modules through unhandledAction().
    function trigger(name) {
        switch (name) {
        case "terminal": session.run("nexus-terminal"); break
        case "files": session.run("nexus-files"); break
        case "settings": session.run("nexus-control-center"); break
        case "task-manager": session.run("nexus-monitor", ["--page", "processes"]); break
        case "screenshot": session.run("nexus-screenshot"); break
        case "screenshot-region": session.run("nexus-screenshot", ["--region"]); break
        default:
            if (namedOverlays[name]) toggle(namedOverlays[name])
            else shell.unhandledAction(name)
        }
    }

    // Emitted for actions owned by optional modules (volume, brightness and
    // media keys).
    signal unhandledAction(string name)

    // Shows a short message from the shell itself (errors of shell actions).
    function notify(message) { toast.show(message) }

    // ---- optional modules ------------------------------------------------------------
    property var moduleObjects: []
    function loadModules() {
        const created = []
        const instantiate = (source, parent, properties) => {
            const component = Qt.createComponent(source)
            if (component.status !== Component.Ready) {
                console.error("horizon-shell: cannot load module component " + source + ": " + component.errorString())
                return
            }
            const object = component.createObject(parent, properties)
            if (object) created.push(object)
            else console.error("horizon-shell: cannot create module component " + source)
        }
        for (const item of shellModules.panelItems)
            instantiate(item.source, panel.moduleArea, { shell: shell, panel: panel })
        for (const source of shellModules.surfaces)
            instantiate(source, shell, { shell: shell })
        moduleObjects = created
    }

    Component.onCompleted: {
        registerOverlay("launcher", launcher)
        registerOverlay("session-menu", sessionMenu)
        loadModules()
        const request = session.takeAutostartRequest()
        if (request === "installer") session.run("nexus-installer")
        else if (request === "recovery") session.run("nexus-recovery")
    }

    property Connections shortcutConnection: Connections {
        target: shortcuts
        function onTriggered(name) { shell.trigger(name) }
    }

    // Third-party Qt/KDE and GTK applications follow the NEXUS appearance.
    property Connections appearanceSync: Connections {
        target: Appearance
        function onDarkChanged() { session.run("nexus-apply-appearance") }
        function onAccentChanged() { session.run("nexus-apply-appearance") }
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
        clockOpen: shell.openOverlayName === "calendar"
        clockEnabled: shell.overlayNames.indexOf("calendar") >= 0
        onLauncherRequested: shell.trigger("launcher")
        onSessionMenuRequested: shell.trigger("session-menu")
        onClockRequested: shell.trigger("calendar")
    }

    property Dock dockBar: Dock {
        screen: shell.primaryScreen
        onMenuRequested: (index, centerX) => { dockMenu.prepare(index, centerX); shell.present(dockMenu) }
    }

    // ---- overlays ---------------------------------------------------------------------
    property Launcher launcher: Launcher {
        screen: shell.primaryScreen
        onSessionMenuRequested: shell.trigger("session-menu")
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
