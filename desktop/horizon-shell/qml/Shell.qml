// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.kde.layershell as LayerShell
import org.shadow.ui

// Root of the Shadow Horizon shell: creates the surfaces of every screen and
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
        // Without an exit: a closing overlay stays mapped and keeps the
        // keyboard, and two keyboard overlays must never be mapped together.
        if (activeOverlay && activeOverlay !== overlay) activeOverlay.closeNow()
        activeOverlay = overlay
        // A popup under the panel hangs from the button that belongs to it.
        if (overlay.originButton !== undefined)
            overlay.originButton = overlayButton(overlay.overlayName, overlay.screen)
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

    // Panel buttons that open a named overlay, one per panel (screen). A
    // button is a PanelButton: it tells its screen and its centre.
    property var overlayButtons: ({})
    function registerOverlayButton(name, button) {
        const list = (overlayButtons[name] || []).filter(b => b !== button)
        list.push(button)
        overlayButtons[name] = list
    }
    function unregisterOverlayButton(name, button) {
        overlayButtons[name] = (overlayButtons[name] || []).filter(b => b !== button)
    }
    function overlayButton(name, screen) {
        const list = (overlayButtons[name] || []).filter(b => b !== null)
        // Screens are compared by name: the overlay's screen and the one a
        // panel window reports are different objects for the same screen.
        for (let i = 0; i < list.length; i++)
            if (list[i].hostScreen && screen && list[i].hostScreen.name === screen.name) return list[i]
        return list.length === 1 ? list[0] : null
    }

    // What a named action (global shortcut, panel button) does. Actions the
    // core does not know open the overlay registered under that name, or are
    // passed to the modules through unhandledAction().
    function trigger(name) {
        switch (name) {
        case "terminal": session.run("shadow-terminal"); break
        case "files": session.run("shadow-files"); break
        case "settings": session.run("shadow-control-center"); break
        case "task-manager": session.run("shadow-monitor", ["--page", "processes"]); break
        case "screenshot": session.run("shadow-screenshot"); break
        case "screenshot-region": session.run("shadow-screenshot", ["--region"]); break
        // Cut every network connection, lock, clear the clipboard (src/panic.h).
        // The banner that shows the outcome is the status module's.
        case "panic": panic.engage(); break
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

    // Set by a module that shows the panic state in a banner of its own (the
    // status module: the banner, whose buttons quick settings offers too);
    // without one, panic failures are shown as toasts.
    property var panicBanner: null
    readonly property bool panicBannerLoaded: panicBanner !== null

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
        // A panic key that silently does nothing is worse than none.
        if (shortcuts.unbound.indexOf("panic") >= 0)
            toast.show(qsTr("The panic shortcut has no key: Ctrl+Alt+Shift+Escape is used by another program."))
        const request = session.takeAutostartRequest()
        if (request === "installer") session.run("shadow-installer")
        else if (request === "recovery") session.run("shadow-recovery")
    }

    property Connections shortcutConnection: Connections {
        target: shortcuts
        function onTriggered(name) { shell.trigger(name) }
    }

    // Third-party Qt/KDE and GTK applications follow the SHADOW appearance.
    property Connections appearanceSync: Connections {
        target: Appearance
        function onDarkChanged() { session.run("shadow-apply-appearance") }
        function onAccentChanged() { session.run("shadow-apply-appearance") }
        // Also carries Reduce motion to the lock screen and to KWin.
        function onReduceMotionChanged() { session.run("shadow-apply-appearance") }
    }

    property Connections panicErrors: Connections {
        target: panic
        function onFailed(message) { if (!shell.panicBannerLoaded) toast.show(message) }
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
        // Named overlays (launcher, calendar, session menu, module popups)
        // belong to the panel: a hidden panel comes back while one is open.
        holdOpen: shell.openOverlayName !== ""
        onLauncherRequested: shell.trigger("launcher")
        onSessionMenuRequested: shell.trigger("session-menu")
        onClockRequested: shell.trigger("calendar")
    }

    property Dock dockBar: Dock {
        screen: shell.primaryScreen
        holdOpen: shell.dockMenu.open
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
    // A strip under the panel. It drops 8 px into place while it fades in
    // and leaves the same way (Theme.motionFast); the surface stays mapped
    // until it has gone.
    property ShellSurface toast: ShellSurface {
        id: toast
        screen: shell.primaryScreen
        layer: "overlay"
        scope: "shadow-shell-toast"
        anchorTop: true
        // Under the panel also when the panel reserves no space for itself
        // (it does only when it never hides).
        LayerShell.Window.margins.top: panel.exclusiveZone > 0 ? 0 : Theme.panelHeight
        width: Math.min(560, (screen ? screen.width : 1280) - 64)
        height: toastText.implicitHeight + Theme.spaceMd * 2 + Theme.spaceSm
        visible: wanted || shown > 0

        property bool wanted: false
        property real shown: wanted ? 1 : 0
        Behavior on shown { NumberAnimation { duration: Theme.motionFast } }

        function show(message) {
            toastText.text = message
            wanted = true
            toastTimer.restart()
        }

        Timer { id: toastTimer; interval: 6000; onTriggered: toast.wanted = false }

        Rectangle {
            anchors.fill: parent
            anchors.topMargin: Theme.spaceSm
            radius: Theme.radiusMd
            color: Theme.bgOverlay
            border.width: Theme.borderWidth
            border.color: Theme.alpha(Theme.danger, 0.6)
            opacity: toast.shown
            transform: Translate { y: -8 * (1 - toast.shown) }
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
            TapHandler { onTapped: toast.wanted = false }
        }
    }
}
