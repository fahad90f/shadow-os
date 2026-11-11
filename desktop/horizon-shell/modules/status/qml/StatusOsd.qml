// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.nexus.ui

// On-screen display for the volume, microphone, brightness and media keys.
// The keys arrive as shell actions (shell.unhandledAction); the display
// shows the value the back-end reports afterwards, not the one asked for.
// It never takes the keyboard or the pointer.
ShellSurface {
    id: osd

    required property var shell

    readonly property var audio: status.audio
    readonly property var media: status.media
    // "volume" | "mic" | "brightness" | "media-play" | "media-next" | "media-previous"
    property string kind: "volume"

    readonly property bool hasOutput: audio.available && audio.outputAvailable
    readonly property bool hasInput: audio.available && audio.inputAvailable
    // The level bar is shown for a level that exists.
    readonly property bool showsLevel: (kind === "volume" && hasOutput) || (kind === "brightness" && status.brightness.available)
    readonly property real level: kind === "volume" ? audio.volume : status.brightness.level
    readonly property bool dimmed: kind === "volume" && audio.muted

    readonly property string glyph: {
        switch (kind) {
        case "volume":
            return !hasOutput || audio.muted || audio.volume <= 0 ? "volume-mute" : audio.volume <= 0.33 ? "volume-low" : "volume-high"
        case "mic": return hasInput && !audio.inputMuted ? "mic" : "mic-off"
        case "brightness": return "brightness"
        case "media-next": return "skip-forward"
        case "media-previous": return "skip-back"
        default: return !media.hasPlayer ? "music" : media.playing ? "play" : "pause"
        }
    }
    readonly property string message: {
        switch (kind) {
        case "volume":
            return !audio.available ? qsTr("Sound is not available")
                : !audio.outputAvailable ? qsTr("No sound output device") : ""
        case "mic":
            return !audio.available ? qsTr("Sound is not available")
                : !audio.inputAvailable ? qsTr("No microphone")
                : audio.inputMuted ? qsTr("Microphone muted") : qsTr("Microphone on")
        case "brightness":
            return status.brightness.available ? "" : qsTr("The screen brightness cannot be changed here")
        default:
            if (!media.hasPlayer) return qsTr("No media player is running")
            if (media.title === "") return media.playerName
            return media.artist !== "" ? media.title + "  ·  " + media.artist : media.title
        }
    }

    function present(what) {
        kind = what
        visible = true
        hideTimer.restart()
    }

    screen: shell.primaryScreen
    layer: "overlay"
    scope: "nexus-osd"
    anchorBottom: true
    keyboard: "none"
    flags: Qt.FramelessWindowHint | Qt.WindowTransparentForInput | Qt.WindowDoesNotAcceptFocus
    // As large as the card plus the distance kept from the screen edge (the dock).
    width: card.width
    height: card.height + Theme.dockIcon + Theme.spaceXxxl + Theme.spaceXl
    visible: false

    Timer { id: hideTimer; interval: 1800; onTriggered: osd.visible = false }

    Connections {
        target: osd.shell
        function onUnhandledAction(name) {
            switch (name) {
            case "volume-up": osd.audio.stepVolume(1); osd.present("volume"); break
            case "volume-down": osd.audio.stepVolume(-1); osd.present("volume"); break
            case "volume-mute": osd.audio.toggleMuted(); osd.present("volume"); break
            case "mic-mute": osd.audio.toggleInputMuted(); osd.present("mic"); break
            case "brightness-up": status.brightness.step(1); osd.present("brightness"); break
            case "brightness-down": status.brightness.step(-1); osd.present("brightness"); break
            case "media-play": osd.media.playPause(); osd.present("media-play"); break
            case "media-next": osd.media.next(); osd.present("media-next"); break
            case "media-previous": osd.media.previous(); osd.present("media-previous"); break
            }
        }
    }

    Rectangle {
        id: card
        width: Math.min(row.implicitWidth + Theme.spaceLg * 2, (osd.screen ? osd.screen.width : 1280) - Theme.spaceXl * 2)
        height: Theme.rowHeightLarge
        radius: Theme.radiusLg
        color: blur.glass ? Theme.glassPopup : Theme.bgOverlay
        border.width: Theme.borderWidth
        border.color: Theme.border
        Accessible.role: Accessible.Indicator
        Accessible.name: osd.showsLevel ? qsTr("%1 %").arg(Math.round(osd.level * 100)) : osd.message

        BlurBehind { id: blur; anchors.fill: parent; radius: card.radius; surface: osd }

        RowLayout {
            id: row
            anchors.fill: parent
            anchors.leftMargin: Theme.spaceLg
            anchors.rightMargin: Theme.spaceLg
            spacing: Theme.spaceMd

            NxIcon {
                name: osd.glyph
                size: Theme.iconLarge
                color: osd.dimmed || (!osd.showsLevel && osd.kind !== "mic" && !osd.media.hasPlayer) ? Theme.textMuted : Theme.textPrimary
            }
            Rectangle {
                visible: osd.showsLevel
                Layout.preferredWidth: Theme.sidebarWidth - Theme.spaceXxxl
                implicitHeight: Theme.spaceXs + 2
                radius: height / 2
                color: Theme.bgPressed
                Rectangle {
                    width: Math.round(parent.width * Math.max(0, Math.min(1, osd.level)))
                    height: parent.height
                    radius: parent.radius
                    color: osd.dimmed ? Theme.textMuted : Theme.accent
                    Behavior on width {
                        enabled: !Appearance.reduceMotion
                        NumberAnimation { duration: Theme.motionFast; easing.type: Easing.OutCubic }
                    }
                }
            }
            Text {
                visible: osd.showsLevel
                Layout.preferredWidth: levelMetrics.advanceWidth
                horizontalAlignment: Text.AlignRight
                text: qsTr("%1 %").arg(Math.round(osd.level * 100))
                color: osd.dimmed ? Theme.textMuted : Theme.textPrimary
                font: levelMetrics.font
            }
            Text {
                visible: !osd.showsLevel
                Layout.maximumWidth: Theme.sidebarWidth * 2
                text: osd.message
                color: Theme.textPrimary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBodyLarge
                font.weight: Theme.weightMedium
                elide: Text.ElideRight
            }
        }

        TextMetrics {
            id: levelMetrics
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeBodyLarge
            font.weight: Theme.weightSemibold
            text: qsTr("%1 %").arg(100)
        }
    }
}
