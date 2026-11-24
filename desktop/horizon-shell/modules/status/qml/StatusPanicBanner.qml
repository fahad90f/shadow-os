// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.shadow.ui

// The panic banner: a full-width, high-contrast strip at the top of the
// screen from the moment the panic shortcut is pressed until the panic is
// released. It shows what is TRUE, not what was asked for:
//
//   * "cut" only when shadow-panic's own status (/run/shadow/panic.json,
//     status.killSwitch.panic*) says engaged, loaded and no error;
//   * a failure as a failure: the panic service could not be started (and
//     what the shell did instead), or the status reports an error, or the
//     service has not confirmed the cut 15 s after it was started;
//   * what happened to the screen lock and the clipboard (the shell's own
//     part of the panic, the core object "panic", src/panic.h).
//
// "Restore network…" runs pkexec /usr/sbin/shadow-panic release: an
// administrator must authenticate, every time. A release made elsewhere (the
// Control Center, sudo shadow-panic release) ends the panic too: the banner
// passes panic.json's state on to the core (panic.setServiceState()), which
// then forgets its request, and the banner goes.
//
// This surface gets the keyboard only when it is clicked; its buttons are
// also in quick settings (StatusQuickSettings.qml), which the keyboard
// reaches (Meta+A), and the banner says so.
//
// Layer "overlay" with an exclusive zone: it stays above full-screen windows,
// and the panel and maximised windows move below it instead of being
// covered. The lock screen covers it while the session is locked.
ShellSurface {
    id: banner

    required property var shell

    readonly property var ks: status.killSwitch
    readonly property bool hasCore: typeof panic !== "undefined"
    readonly property string phase: hasCore ? panic.phase : "idle"
    readonly property string fileState: ks.panicState          // none | released | engaged | error
    readonly property bool fallbackOff: hasCore && panic.fallback === "networking-off"
    readonly property bool cut: ks.panicCut
    // Asked for in this session but not confirmed within 15 s.
    property bool overdue: false
    readonly property bool shown: fileState === "engaged" || fileState === "error" || phase !== "idle" || fallbackOff
    readonly property bool problem: !cut && (fileState === "error" || phase === "failed" || overdue)
    // Something to release: the service recorded a panic (or its table is
    // still loaded), or the shell's fallback turned networking off.
    readonly property bool releasable: fileState === "engaged" || fileState === "error" || ks.panicEngaged || fallbackOff

    // The buttons, shared with quick settings (shell.panicBanner).
    readonly property bool canRestore: releasable && hasCore
    readonly property bool canDismiss: hasCore && !releasable && (phase === "failed" || overdue)
    readonly property bool restoreBusy: hasCore && panic.releasing
    readonly property string restoreText: restoreBusy ? qsTr("Waiting for an administrator…") : qsTr("Restore network…")
    function restoreNetwork() { panic.release(fileState === "engaged" || fileState === "error" || ks.panicEngaged) }
    function dismissPanic() { panic.dismiss() }

    // The key that opens quick settings (users can rebind it).
    readonly property string quickSettingsKey: {
        if (typeof shortcuts === "undefined") return ""
        const entry = shortcuts.list.find(s => s.name === "quick-settings")
        return entry && entry.keys.length > 0 ? entry.keys[0] : ""
    }
    readonly property string keyboardHint: !(canRestore || canDismiss) ? ""
        : quickSettingsKey !== "" ? qsTr("Keyboard: %1 opens quick settings, which has this button too.").arg(quickSettingsKey)
        : qsTr("Quick settings (the status area of the panel) has this button too.")

    // High contrast, independent of the light/dark theme: white on deep red
    // (about 8:1) while cut or cutting; near-black on amber (about 11:1)
    // when the panic is NOT what was asked for.
    readonly property color background: problem ? "#FFC23D" : "#B00020"
    readonly property color foreground: problem ? "#1A1200" : "#FFFFFF"

    readonly property string title: {
        if (cut) return qsTr("PANIC — this computer is cut off from every network")
        if (phase === "failed" && fallbackOff)
            return qsTr("PANIC — the panic service did not start: only NetworkManager networking was turned off")
        if (phase === "failed") return qsTr("PANIC FAILED — the network is NOT cut")
        if (fileState === "error") return qsTr("PANIC — problem: the network may NOT be cut")
        if (overdue) return qsTr("PANIC — the panic service has not confirmed the cut")
        return qsTr("PANIC — cutting every network connection…")
    }

    function timeOf(iso) {
        if (iso === "") return ""
        const d = new Date(iso)
        return isNaN(d.getTime()) ? "" : d.toLocaleTimeString(Qt.locale(), Locale.ShortFormat)
    }

    readonly property string networkLine: {
        if (cut) {
            const at = timeOf(ks.panicSince)
            return at !== "" ? qsTr("Every connection is blocked since %1; only this computer's own loopback works.").arg(at)
                             : qsTr("Every connection is blocked; only this computer's own loopback works.")
        }
        if (phase === "failed") {
            const what = panic.error
            if (fallbackOff) return qsTr("%1 Programs or links NetworkManager does not manage can still reach the network.").arg(what)
            if (hasCore && panic.fallback === "failed")
                return qsTr("%1 Turning NetworkManager networking off failed too: %2").arg(what).arg(panic.fallbackError)
            return what
        }
        if (fileState === "error") return ks.panicErrorText
        if (overdue) return qsTr("Check with: sudo shadow-panic status")
        return qsTr("Waiting for the panic service…")
    }

    readonly property string sessionLine: {
        if (!hasCore) return ""
        const parts = []
        switch (panic.lockResult) {
        case "locked": parts.push(qsTr("Screen locked.")); break
        case "requested": parts.push(qsTr("Screen lock requested.")); break
        case "pending": parts.push(qsTr("Locking the screen…")); break
        case "failed": parts.push(qsTr("The screen was NOT locked (%1).").arg(panic.lockError)); break
        }
        switch (panic.clipboardResult) {
        case "cleared":
            parts.push(panic.clipboardError === "" ? qsTr("Clipboard cleared.") : qsTr("Clipboard cleared (%1).").arg(panic.clipboardError))
            break
        case "pending": parts.push(qsTr("Clearing the clipboard…")); break
        case "failed": parts.push(qsTr("The clipboard was NOT cleared: %1.").arg(panic.clipboardError)); break
        }
        if (ks.panicWarnings.length === 1)
            parts.push(qsTr("Warning from the panic service: %1").arg(ks.panicWarnings[0]))
        else if (ks.panicWarnings.length > 1)
            parts.push(qsTr("%1 warnings from the panic service, the first: %2").arg(ks.panicWarnings.length).arg(ks.panicWarnings[0]))
        return parts.join("  ")
    }

    screen: shell.primaryScreen
    layer: "overlay"
    scope: "shadow-panic-banner"
    anchorTop: true; anchorLeft: true; anchorRight: true
    exclusiveZone: height
    keyboard: "ondemand"
    width: screen ? screen.width : 1280
    height: Math.max(Theme.rowHeightLarge + Theme.spaceMd, content.implicitHeight + Theme.spaceMd * 2)
    visible: shown

    Component.onCompleted: {
        shell.panicBanner = banner
        if (hasCore) panic.setServiceState(fileState)
    }
    // A release made elsewhere ends the core's request (src/panic.h).
    onFileStateChanged: if (hasCore) panic.setServiceState(fileState)

    Timer {
        // Re-evaluated every second while waiting for the service.
        running: banner.shown && !banner.cut && banner.hasCore && panic.requestedAt > 0
        interval: 1000
        repeat: true
        triggeredOnStart: true
        onTriggered: banner.overdue = Date.now() - panic.requestedAt > 15000
        onRunningChanged: if (!running) banner.overdue = false
    }

    Rectangle {
        anchors.fill: parent
        color: banner.background
        Accessible.role: Accessible.AlertMessage
        Accessible.name: banner.title
        Accessible.description: [banner.networkLine, banner.sessionLine, banner.keyboardHint].filter(t => t !== "").join(" ")

        RowLayout {
            id: content
            anchors.fill: parent
            anchors.leftMargin: Theme.spaceXl
            anchors.rightMargin: Theme.spaceXl
            spacing: Theme.spaceLg

            NxIcon {
                Layout.alignment: Qt.AlignVCenter
                name: banner.problem ? "alert-triangle" : "shield-off"
                size: Theme.iconLarge + 8
                color: banner.foreground
                SequentialAnimation on opacity {
                    running: !banner.cut && !banner.problem && !Appearance.reduceMotion
                    loops: Animation.Infinite
                    alwaysRunToEnd: true
                    NumberAnimation { to: 0.35; duration: Theme.motionSlow * 2 }
                    NumberAnimation { to: 1; duration: Theme.motionSlow * 2 }
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spaceSm
                Layout.bottomMargin: Theme.spaceSm
                spacing: 2
                Text {
                    Layout.fillWidth: true
                    text: banner.title
                    color: banner.foreground
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeTitle
                    font.weight: Theme.weightBold
                    wrapMode: Text.WordWrap
                }
                Text {
                    Layout.fillWidth: true
                    visible: text !== ""
                    text: banner.networkLine
                    color: banner.foreground
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBody
                    wrapMode: Text.WordWrap
                }
                Text {
                    Layout.fillWidth: true
                    visible: text !== ""
                    text: banner.sessionLine
                    color: banner.foreground
                    opacity: 0.9
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                    wrapMode: Text.WordWrap
                }
                Text {
                    Layout.fillWidth: true
                    visible: text !== ""
                    text: banner.keyboardHint
                    color: banner.foreground
                    opacity: 0.9
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                    wrapMode: Text.WordWrap
                }
                Text {
                    Layout.fillWidth: true
                    visible: banner.hasCore && panic.releaseError !== ""
                    text: banner.hasCore ? panic.releaseError : ""
                    color: banner.foreground
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                    font.weight: Theme.weightSemibold
                    wrapMode: Text.WordWrap
                }
            }
            BannerButton {
                objectName: "panicRestoreButton"
                visible: banner.canRestore
                text: banner.restoreText
                busy: banner.restoreBusy
                description: qsTr("Release the panic: an administrator must authenticate")
                onActivated: banner.restoreNetwork()
            }
            BannerButton {
                objectName: "panicDismissButton"
                visible: banner.canDismiss
                text: qsTr("Dismiss")
                description: qsTr("Hide this banner; nothing was cut")
                onActivated: banner.dismissPanic()
            }
        }
    }

    // A plain, high-contrast button in the banner's own colours (the
    // theme's buttons are made for the theme's surfaces).
    component BannerButton: Rectangle {
        id: bannerButton
        property string text: ""
        property string description: ""
        property bool busy: false
        signal activated()

        Layout.alignment: Qt.AlignVCenter
        implicitWidth: label.implicitWidth + Theme.spaceXl * 2
        implicitHeight: Theme.controlHeight
        radius: Theme.radiusMd
        color: tap.pressed ? Qt.darker(banner.foreground, 1.25) : hover.hovered ? Qt.darker(banner.foreground, 1.1) : banner.foreground
        border.width: activeFocus ? Theme.focusRing : 0
        border.color: banner.background
        opacity: busy ? 0.75 : 1
        activeFocusOnTab: true
        Accessible.role: Accessible.Button
        Accessible.name: text
        Accessible.description: description
        Accessible.onPressAction: if (!busy) activated()
        Keys.onReturnPressed: if (!busy) activated()
        Keys.onSpacePressed: if (!busy) activated()

        Text {
            id: label
            anchors.centerIn: parent
            text: bannerButton.text
            color: banner.background
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeBody
            font.weight: Theme.weightBold
        }
        HoverHandler { id: hover; cursorShape: bannerButton.busy ? Qt.BusyCursor : Qt.PointingHandCursor }
        TapHandler {
            id: tap
            enabled: !bannerButton.busy
            gesturePolicy: TapHandler.ReleaseWithinBounds
            onTapped: bannerButton.activated()
        }
    }
}
