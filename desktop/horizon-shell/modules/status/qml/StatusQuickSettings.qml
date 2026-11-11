// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui

// The quick settings popup (Meta+A, or the status area of the panel).
// Everything in it reflects a back-end of the "status" object; what a
// machine or session does not have is left out rather than shown dead.
PopupSurface {
    id: popup

    required property var shell

    readonly property var net: status.network
    readonly property var audio: status.audio
    readonly property var battery: status.battery
    readonly property var profiles: status.powerProfiles
    // The notifications module is optional.
    readonly property bool hasNotifications: typeof notifications !== "undefined"
    // Buttons that open the Control Center exist only when it does.
    property bool hasControlCenter: false

    function openSettings(page) {
        shell.closeOverlays()
        session.run("shadow-control-center", [page])
    }

    function profileLabel(id) {
        switch (id) {
        case "power-saver": return qsTr("Power saver")
        case "balanced": return qsTr("Balanced")
        case "performance": return qsTr("Performance")
        default: return id
        }
    }

    // UPower's estimates, to the minute.
    function durationText(seconds) {
        const minutes = Math.max(1, Math.round(seconds / 60))
        const h = Math.floor(minutes / 60), m = minutes % 60
        return h === 0 ? qsTr("%1 min").arg(m) : m === 0 ? qsTr("%1 h").arg(h) : qsTr("%1 h %2 min").arg(h).arg(m)
    }

    function degradedText(reason) {
        switch (reason) {
        case "lap-detected": return qsTr("Performance is reduced because the computer is on a lap.")
        case "high-operating-temperature": return qsTr("Performance is reduced because the computer is hot.")
        default: return qsTr("Performance is reduced (%1).").arg(reason)
        }
    }

    screen: shell.primaryScreen
    alignment: "right"
    popupWidth: 404
    scope: "shadow-quick-settings"

    Component.onCompleted: {
        hasControlCenter = session.programExists("shadow-control-center")
        shell.registerOverlay("quick-settings", popup)
    }
    onOpenChanged: if (open) {
        hasControlCenter = session.programExists("shadow-control-center")
        status.brightness.refresh()
    }

    // Requests a back-end could not carry out.
    Connections {
        target: status
        function onFailed(message) { popup.shell.notify(message) }
    }

    Flickable {
        id: scroller
        width: parent.width
        height: Math.min(content.implicitHeight, popup.maximumHeight - popup.padding * 2)
        contentWidth: width
        contentHeight: content.implicitHeight
        clip: contentHeight > height
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: scroller.contentHeight > scroller.height ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff }

        ColumnLayout {
            id: content
            width: scroller.width
            spacing: Theme.spaceMd

            // ---- switches ---------------------------------------------------------
            GridLayout {
                Layout.fillWidth: true
                columns: 2
                rowSpacing: Theme.spaceSm
                columnSpacing: Theme.spaceSm

                // Wi-Fi where the computer has it; otherwise the network state.
                StatusTile {
                    visible: popup.net.available && popup.net.wifiAvailable
                    icon: popup.net.wifiEnabled ? "wifi" : "wifi-off"
                    title: qsTr("Wi-Fi")
                    active: popup.net.wifiEnabled
                    switchable: popup.net.wifiHardwareEnabled
                    reason: qsTr("Wi-Fi is switched off by a hardware switch.")
                    subtitle: !popup.net.wifiHardwareEnabled ? qsTr("Hardware switch off")
                        : !popup.net.wifiEnabled ? qsTr("Off")
                        : popup.net.wifiConnected && popup.net.wifiSsid !== "" ? popup.net.wifiSsid
                        : popup.net.wifiSsid !== "" ? qsTr("Connecting to %1…").arg(popup.net.wifiSsid)
                        : qsTr("Not connected")
                    hasDetails: popup.hasControlCenter
                    detailsTooltip: qsTr("Wi-Fi networks and settings")
                    onToggled: popup.net.setWifiEnabled(!popup.net.wifiEnabled)
                    onDetailsRequested: popup.openSettings("wifi")
                }
                StatusTile {
                    visible: !(popup.net.available && popup.net.wifiAvailable)
                    icon: popup.net.available && popup.net.primaryKind === "wired" ? "ethernet" : "network"
                    title: qsTr("Network")
                    active: popup.net.available && popup.net.state === "connected"
                    switchable: false
                    reason: popup.net.available ? qsTr("This computer has no Wi-Fi adapter to switch.")
                                                : popup.net.unavailableReason
                    subtitle: !popup.net.ready ? qsTr("Checking…")
                        : !popup.net.available ? qsTr("Status unknown")
                        : popup.net.state === "connected" ? (popup.net.primaryName !== "" ? popup.net.primaryName : qsTr("Connected"))
                        : popup.net.state === "connecting" ? qsTr("Connecting…")
                        : popup.net.state === "asleep" ? qsTr("Switched off")
                        : qsTr("Not connected")
                    hasDetails: popup.hasControlCenter
                    detailsTooltip: qsTr("Network settings")
                    onDetailsRequested: popup.openSettings("network")
                }
                StatusTile {
                    readonly property var bt: status.bluetooth
                    visible: bt.available
                    icon: "bluetooth"
                    title: qsTr("Bluetooth")
                    active: bt.enabled
                    subtitle: bt.blocked && !bt.enabled ? qsTr("Blocked")
                        : !bt.enabled ? qsTr("Off")
                        : bt.connectedCount === 0 ? qsTr("On")
                        : bt.connectedCount === 1 && bt.connectedName !== "" ? bt.connectedName
                        : qsTr("%n device(s) connected", "", bt.connectedCount)
                    hasDetails: popup.hasControlCenter
                    detailsTooltip: qsTr("Bluetooth devices and settings")
                    onToggled: bt.setEnabled(!bt.enabled)
                    onDetailsRequested: popup.openSettings("bluetooth")
                }
                StatusTile {
                    visible: status.radio.available
                    icon: "airplane"
                    title: qsTr("Airplane mode")
                    active: status.radio.airplaneMode
                    subtitle: status.radio.airplaneMode ? qsTr("All radios off") : qsTr("Off")
                    onToggled: status.radio.setAirplaneMode(!status.radio.airplaneMode)
                }
                StatusTile {
                    visible: popup.hasNotifications
                    readonly property bool dnd: popup.hasNotifications && notifications.doNotDisturb
                    icon: dnd ? "bell-off" : "bell"
                    title: qsTr("Do Not Disturb")
                    active: dnd
                    subtitle: dnd ? qsTr("Notifications are silent") : qsTr("Off")
                    onToggled: notifications.doNotDisturb = !dnd
                }
                StatusTile {
                    icon: Appearance.dark ? "moon" : "sun"
                    title: qsTr("Dark mode")
                    active: Appearance.dark
                    subtitle: Appearance.dark ? qsTr("On") : qsTr("Off")
                    onToggled: Appearance.dark = !Appearance.dark
                }
                StatusTile {
                    readonly property var night: status.nightLight
                    visible: night.available
                    icon: "night-light"
                    title: qsTr("Night Light")
                    active: night.enabled
                    subtitle: !night.enabled ? qsTr("Off")
                        : night.inhibited ? qsTr("Paused by an application")
                        : night.running ? qsTr("On — %1 K").arg(night.currentTemperature)
                        : qsTr("On — waiting for the evening")
                    onToggled: night.setEnabled(!night.enabled)
                }
            }

            // ---- levels -----------------------------------------------------------
            StatusSliderRow {
                Layout.fillWidth: true
                visible: popup.audio.available && popup.audio.outputAvailable
                icon: popup.audio.muted || popup.audio.volume <= 0 ? "volume-mute"
                    : popup.audio.volume <= 0.33 ? "volume-low" : "volume-high"
                label: qsTr("Output volume")
                caption: popup.audio.outputName
                level: popup.audio.volume
                muted: popup.audio.muted
                canMute: true
                muteTooltip: popup.audio.muted ? qsTr("Unmute") : qsTr("Mute")
                onMoved: level => popup.audio.setVolume(level)
                onMuteToggled: popup.audio.toggleMuted()
            }
            RowLayout {
                Layout.fillWidth: true
                visible: popup.audio.ready && !(popup.audio.available && popup.audio.outputAvailable)
                spacing: Theme.spaceXs
                Item {
                    implicitWidth: Theme.controlHeight
                    implicitHeight: Theme.controlHeight
                    NxIcon { anchors.centerIn: parent; name: "volume-mute"; color: Theme.textDisabled }
                }
                Text {
                    Layout.fillWidth: true
                    text: popup.audio.available ? qsTr("There is no sound output device.") : popup.audio.unavailableReason
                    color: Theme.textSecondary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                    wrapMode: Text.WordWrap
                }
            }
            StatusSliderRow {
                Layout.fillWidth: true
                visible: popup.audio.available && popup.audio.inputAvailable
                icon: popup.audio.inputMuted ? "mic-off" : "mic"
                label: qsTr("Microphone level")
                caption: popup.audio.inputName
                level: popup.audio.inputVolume
                muted: popup.audio.inputMuted
                canMute: true
                muteTooltip: popup.audio.inputMuted ? qsTr("Unmute the microphone") : qsTr("Mute the microphone")
                onMoved: level => popup.audio.setInputVolume(level)
                onMuteToggled: popup.audio.toggleInputMuted()
            }
            StatusSliderRow {
                Layout.fillWidth: true
                visible: status.brightness.available
                icon: "brightness"
                label: qsTr("Screen brightness")
                caption: qsTr("Screen brightness")
                level: status.brightness.level
                onMoved: level => status.brightness.setLevel(level)
            }

            // ---- battery and power profile ----------------------------------------
            Rectangle {
                Layout.fillWidth: true
                visible: (popup.battery.available && popup.battery.present) || popup.profiles.available
                implicitHeight: power.implicitHeight + Theme.spaceMd * 2
                radius: Theme.radiusMd
                color: Theme.hoverOverlay
                border.width: Theme.borderWidth
                border.color: Theme.borderSubtle

                ColumnLayout {
                    id: power
                    anchors.fill: parent
                    anchors.margins: Theme.spaceMd
                    spacing: Theme.spaceSm

                    RowLayout {
                        Layout.fillWidth: true
                        visible: popup.battery.available && popup.battery.present
                        spacing: Theme.spaceSm
                        StatusGlyph {
                            visible: !popup.battery.charging
                            kind: "battery"
                            size: Theme.icon
                            level: popup.battery.percentage / 100
                            fillColor: popup.battery.warningLevel === "critical" || popup.battery.warningLevel === "action" ? Theme.danger
                                : popup.battery.warningLevel === "low" ? Theme.warning : color
                        }
                        NxIcon {
                            visible: popup.battery.charging
                            name: "battery-charging"
                            color: Theme.textPrimary
                        }
                        Text {
                            text: qsTr("%1 %").arg(popup.battery.percentage)
                            color: Theme.textPrimary
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeBody
                            font.weight: Theme.weightSemibold
                        }
                        Text {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignRight
                            elide: Text.ElideRight
                            color: Theme.textSecondary
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeSmall
                            text: {
                                const b = popup.battery
                                switch (b.state) {
                                case "charging":
                                    return b.secondsToFull > 0 ? qsTr("Charging — %1 until full").arg(popup.durationText(b.secondsToFull))
                                                               : qsTr("Charging")
                                case "discharging":
                                    return b.secondsToEmpty > 0 ? qsTr("%1 remaining").arg(popup.durationText(b.secondsToEmpty))
                                                                : qsTr("On battery")
                                case "full": return qsTr("Fully charged")
                                case "empty": return qsTr("Empty")
                                case "pending-charge": return qsTr("Plugged in, not charging")
                                case "pending-discharge": return qsTr("On battery")
                                default: return b.onBattery ? qsTr("On battery") : qsTr("Plugged in")
                                }
                            }
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: popup.profiles.available
                        text: qsTr("Power profile")
                        color: Theme.textSecondary
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeCaption
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        visible: popup.profiles.available
                        spacing: Theme.spaceSm
                        NxIcon {
                            name: popup.profiles.activeProfile === "power-saver" ? "leaf"
                                : popup.profiles.activeProfile === "performance" ? "zap" : "gauge"
                            color: Theme.textPrimary
                        }
                        StatusSegmented {
                            Layout.fillWidth: true
                            model: popup.profiles.profiles.map(id => popup.profileLabel(id))
                            // The daemon's profile: it moves when the daemon has switched.
                            currentIndex: popup.profiles.activeIndex
                            Accessible.name: qsTr("Power profile")
                            onActivated: index => popup.profiles.setActiveIndex(index)
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: popup.profiles.available && popup.profiles.degradedReason !== ""
                        text: popup.degradedText(popup.profiles.degradedReason)
                        color: Theme.warning
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeCaption
                        wrapMode: Text.WordWrap
                    }
                }
            }

            // ---- media ------------------------------------------------------------
            StatusMediaCard {
                Layout.fillWidth: true
                visible: status.media.available && status.media.hasPlayer
            }

            // ---- footer -----------------------------------------------------------
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceXs
                NxButton {
                    visible: popup.hasControlCenter
                    kind: "ghost"
                    iconName: "settings"
                    text: qsTr("Control Center")
                    onClicked: { popup.shell.closeOverlays(); popup.shell.trigger("settings") }
                }
                Item { Layout.fillWidth: true }
                NxIconButton {
                    visible: session.canLock
                    iconName: "lock"
                    tooltip: qsTr("Lock the screen")
                    onClicked: { popup.shell.closeOverlays(); session.lock() }
                }
                NxIconButton {
                    iconName: "power"
                    tooltip: qsTr("Log out, restart or power off")
                    onClicked: popup.shell.trigger("session-menu")
                }
            }
        }
    }
}
