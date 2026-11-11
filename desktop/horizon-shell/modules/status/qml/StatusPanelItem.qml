// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.nexus.ui

// The status area of the panel: network, volume and battery at a glance, in
// one button that opens the quick settings.
Item {
    id: root

    required property var shell
    required property var panel

    readonly property var net: status.network
    readonly property var audio: status.audio
    readonly property var battery: status.battery

    // ---- network --------------------------------------------------------------
    readonly property bool netConnecting: net.available && net.state === "connecting"
    readonly property bool netConnected: net.available && net.state === "connected"
    readonly property bool netLimited: netConnected && net.connectivity !== "full" && net.connectivity !== "unknown"
    readonly property bool wifiCarries: (netConnected || netConnecting) && net.primaryKind === "wifi"
    readonly property int wifiBars: net.wifiStrength >= 75 ? 3 : net.wifiStrength >= 50 ? 2 : net.wifiStrength >= 25 ? 1 : 0
    readonly property string netIcon: {
        if (!net.available) return "network"
        if (!netConnected && !netConnecting) return net.wifiAvailable ? "wifi-off" : "ethernet"
        switch (net.primaryKind) {
        case "wired": return "ethernet"
        case "mobile": return "signal"
        case "bluetooth": return "bluetooth"
        default: return "network"
        }
    }
    readonly property color netColor: !net.available || !(netConnected || netConnecting) ? Theme.textDisabled
        : netLimited ? Theme.warning : Theme.textPrimary
    readonly property string netText: {
        if (!net.ready) return qsTr("Network: checking…")
        if (!net.available) return qsTr("Network: status unknown — %1").arg(net.unavailableReason)
        if (net.state === "asleep") return qsTr("Network: switched off")
        if (!netConnected && !netConnecting) return qsTr("Network: not connected")
        let text
        const name = net.primaryName
        if (net.primaryKind === "wifi")
            text = qsTr("Wi-Fi: %1 (signal %2 %)").arg(net.wifiSsid !== "" ? net.wifiSsid : name).arg(net.wifiStrength)
        else if (net.primaryKind === "wired")
            text = name !== "" ? qsTr("Wired network: %1").arg(name) : qsTr("Wired network")
        else if (net.primaryKind === "mobile")
            text = name !== "" ? qsTr("Mobile broadband: %1").arg(name) : qsTr("Mobile broadband")
        else
            text = name !== "" ? qsTr("Network: %1").arg(name) : qsTr("Network: connected")
        if (netConnecting) text += qsTr(" — connecting…")
        else if (net.connectivity === "portal") text += qsTr(" — sign-in required")
        else if (net.connectivity === "limited" || net.connectivity === "none") text += qsTr(" — no internet access")
        return text
    }

    // ---- sound ----------------------------------------------------------------
    readonly property bool hasOutput: audio.available && audio.outputAvailable
    readonly property int volumePercent: Math.round(audio.volume * 100)
    readonly property string volumeIcon: !hasOutput || audio.muted || volumePercent === 0 ? "volume-mute"
        : volumePercent <= 33 ? "volume-low" : "volume-high"
    readonly property string volumeText: !audio.ready ? qsTr("Sound: checking…")
        : !audio.available ? qsTr("Sound: not available — %1").arg(audio.unavailableReason)
        : !audio.outputAvailable ? qsTr("Sound: no output device")
        : audio.muted ? qsTr("Volume: muted (%1 %)").arg(volumePercent)
        : qsTr("Volume: %1 %").arg(volumePercent)

    // ---- battery --------------------------------------------------------------
    readonly property bool hasBattery: battery.available && battery.present
    readonly property bool batteryLow: battery.warningLevel !== "none" || (battery.percentage <= 15 && !battery.charging)
    readonly property string batteryText: {
        if (!hasBattery) return ""
        const percent = qsTr("Battery: %1 %").arg(battery.percentage)
        if (battery.charging) return percent + qsTr(", charging")
        if (battery.state === "full") return percent + qsTr(", fully charged")
        if (battery.onBattery) return percent + qsTr(", on battery")
        return percent + qsTr(", plugged in")
    }

    readonly property string summary: [netText, net.available && net.vpnActive ? qsTr("VPN: %1").arg(net.vpnName) : "",
                                       volumeText, batteryText].filter(line => line !== "").join("\n")

    implicitWidth: indicators.implicitWidth + Theme.spaceMd * 2
    implicitHeight: Theme.panelHeight
    width: implicitWidth
    height: implicitHeight

    PanelButton {
        id: button
        anchors.fill: parent
        checked: root.shell.openOverlayName === "quick-settings"
        // A button only while the popup it opens exists.
        interactive: root.shell.overlayNames.indexOf("quick-settings") >= 0
        // No tooltip: the panel surface is only as high as the panel, so a
        // tooltip (several lines here) would cover the panel itself. The
        // summary is the accessible name; sighted users get it in the popup.
        Accessible.name: qsTr("Quick settings")
        Accessible.description: root.summary
        onClicked: root.shell.trigger("quick-settings")
    }

    Row {
        id: indicators
        anchors.centerIn: parent
        spacing: Theme.spaceSm

        Item {
            id: netIndicator
            width: Theme.iconSmall
            height: Theme.iconSmall
            anchors.verticalCenter: parent.verticalCenter
            StatusGlyph {
                visible: root.wifiCarries
                kind: "wifi"
                level: root.wifiBars
                color: button.checked && !root.netLimited ? Theme.accent : root.netColor
            }
            NxIcon {
                visible: !root.wifiCarries
                name: root.netIcon
                size: Theme.iconSmall
                color: button.checked && root.netConnected && !root.netLimited ? Theme.accent : root.netColor
            }
            SequentialAnimation on opacity {
                running: root.netConnecting && !Appearance.reduceMotion
                loops: Animation.Infinite
                alwaysRunToEnd: true
                NumberAnimation { to: 0.35; duration: Theme.motionSlow * 2 }
                NumberAnimation { to: 1; duration: Theme.motionSlow * 2 }
            }
        }
        NxIcon {
            visible: root.net.available && root.net.vpnActive
            anchors.verticalCenter: parent.verticalCenter
            name: "shield-check"
            size: Theme.iconSmall
            color: button.checked ? Theme.accent : Theme.textPrimary
        }
        NxIcon {
            anchors.verticalCenter: parent.verticalCenter
            name: root.volumeIcon
            size: Theme.iconSmall
            color: !root.hasOutput ? Theme.textDisabled : button.checked ? Theme.accent : Theme.textPrimary
        }
        Row {
            visible: root.hasBattery
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.spaceXs
            StatusGlyph {
                visible: !root.battery.charging
                anchors.verticalCenter: parent.verticalCenter
                kind: "battery"
                level: root.battery.percentage / 100
                color: button.checked ? Theme.accent : Theme.textPrimary
                fillColor: root.battery.warningLevel === "critical" || root.battery.warningLevel === "action" ? Theme.danger
                    : root.batteryLow ? Theme.warning : color
            }
            NxIcon {
                visible: root.battery.charging
                anchors.verticalCenter: parent.verticalCenter
                name: "battery-charging"
                size: Theme.iconSmall
                color: button.checked ? Theme.accent : Theme.textPrimary
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("%1 %").arg(root.battery.percentage)
                color: Theme.textPrimary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                font.weight: Theme.weightMedium
            }
        }
    }
}
