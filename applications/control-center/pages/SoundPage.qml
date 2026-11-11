// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.controlcenter

NxPage {
    id: page
    title: qsTr("Sound")
    subtitle: sound.serverName !== "" ? qsTr("Output and input devices, application volumes and device profiles — served by %1.").arg(sound.serverName)
                                      : qsTr("Output and input devices, application volumes and device profiles.")

    // Bumped when the sound server refuses a change, so controls fall back to the real value.
    property int revision: 0
    readonly property real sliderWidth: Math.max(110, Math.min(220, width * 0.24))

    SoundBackend {
        id: sound
        // The level meter records the default input: only while this page is in front of the user.
        meterEnabled: Qt.application.state === Qt.ApplicationActive
        onFailed: (message) => {
            page.revision++
            page.ApplicationWindow.window.toaster.show(message, "danger")
        }
    }

    // A line of text inside a section that has nothing to list.
    component EmptyRow: Item {
        property alias text: emptyLabel.text
        Layout.fillWidth: true
        implicitHeight: Theme.rowHeight
        Label {
            id: emptyLabel
            anchors.fill: parent
            anchors.leftMargin: Theme.spaceLg
            anchors.rightMargin: Theme.spaceLg
            verticalAlignment: Text.AlignVCenter
            color: Theme.textMuted
            elide: Text.ElideRight
        }
    }

    // Mute button, volume slider and percentage — shared by devices and streams.
    component VolumeControl: Row {
        id: control
        property int volume: 0
        property bool muted: false
        property bool writable: true
        property int maximum: 100
        property real sliderWidth: 200
        property string subject: ""
        property string mutedIcon: "volume-mute"
        property string unmutedIcon: "volume-high"
        signal volumeRequested(int percent)
        signal muteRequested(bool muted)

        spacing: Theme.spaceSm
        NxIconButton {
            anchors.verticalCenter: parent.verticalCenter
            iconName: control.muted ? control.mutedIcon : control.unmutedIcon
            tooltip: control.muted ? qsTr("Unmute %1").arg(control.subject) : qsTr("Mute %1").arg(control.subject)
            onClicked: control.muteRequested(!control.muted)
        }
        Slider {
            id: slider
            anchors.verticalCenter: parent.verticalCenter
            width: control.sliderWidth
            from: 0
            // A device that is already louder than the limit keeps its position reachable.
            to: Math.max(control.maximum, control.volume)
            stepSize: 1
            enabled: control.writable
            opacity: control.muted ? 0.55 : 1
            Accessible.name: qsTr("Volume of %1").arg(control.subject)
            onMoved: control.volumeRequested(Math.round(value))
            Binding {
                target: slider
                property: "value"
                value: control.volume
                when: !slider.pressed
                restoreMode: Binding.RestoreNone
            }
        }
        Label {
            anchors.verticalCenter: parent.verticalCenter
            width: 46
            horizontalAlignment: Text.AlignRight
            text: (slider.pressed ? Math.round(slider.value) : control.volume) + " %"
            color: control.volume > 100 ? Theme.warning : Theme.textSecondary
            font.family: Theme.fontMono
        }
    }

    // One output or input device: volume row plus, if it has several, a port row.
    component DeviceItem: Item {
        id: device
        property var backend
        property bool output: true
        property int position: 0
        property int deviceIndex: -1
        property string description: ""
        property int volume: 0
        property bool muted: false
        property bool isDefault: false
        property bool volumeWritable: true
        property var ports: []
        property int activePort: -1
        property int maximum: 100
        property real sliderWidth: 200
        property int revision: 0

        Layout.fillWidth: true
        implicitHeight: deviceColumn.implicitHeight

        ColumnLayout {
            id: deviceColumn
            anchors.left: parent.left
            anchors.right: parent.right
            spacing: 0

            NxRow {
                icon: device.output ? "volume-high" : "mic"
                iconColor: device.isDefault ? Theme.accent : Theme.textSecondary
                title: device.description
                subtitle: device.isDefault ? (device.output ? qsTr("Default output") : qsTr("Default input")) : ""

                VolumeControl {
                    anchors.verticalCenter: parent.verticalCenter
                    volume: device.volume
                    muted: device.muted
                    writable: device.volumeWritable
                    maximum: device.maximum
                    sliderWidth: device.sliderWidth
                    subject: device.description
                    mutedIcon: device.output ? "volume-mute" : "mic-off"
                    unmutedIcon: device.output ? "volume-high" : "mic"
                    onVolumeRequested: (percent) => device.output ? device.backend.setSinkVolume(device.deviceIndex, percent)
                                                                  : device.backend.setSourceVolume(device.deviceIndex, percent)
                    onMuteRequested: (muted) => device.output ? device.backend.setSinkMuted(device.deviceIndex, muted)
                                                              : device.backend.setSourceMuted(device.deviceIndex, muted)
                }
                RadioButton {
                    anchors.verticalCenter: parent.verticalCenter
                    checked: (device.revision, device.isDefault)
                    Accessible.name: device.output ? qsTr("Use %1 as the default output").arg(device.description)
                                                   : qsTr("Use %1 as the default input").arg(device.description)
                    ToolTip.visible: hovered
                    ToolTip.text: device.output ? qsTr("Use as the default output") : qsTr("Use as the default input")
                    onToggled: {
                        if (!checked) return
                        if (device.output) device.backend.setDefaultSink(device.deviceIndex)
                        else device.backend.setDefaultSource(device.deviceIndex)
                    }
                }
            }
            NxRow {
                visible: device.ports.length > 1
                icon: "plug"
                title: qsTr("Port")
                subtitle: device.output ? qsTr("Connector this device plays through") : qsTr("Connector this device records from")
                ComboBox {
                    width: 260
                    model: device.ports
                    textRole: "text"
                    currentIndex: (device.revision, device.activePort)
                    Accessible.name: qsTr("Port of %1").arg(device.description)
                    onActivated: (index) => device.output ? device.backend.setSinkPort(device.deviceIndex, index)
                                                          : device.backend.setSourcePort(device.deviceIndex, index)
                }
            }
        }
        Rectangle {
            visible: device.position > 0
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: Theme.spaceLg
            height: 1
            color: Theme.borderSubtle
        }
    }

    NxCard {
        visible: !sound.available
        Layout.fillWidth: true
        implicitHeight: 300
        NxEmptyState {
            anchors.fill: parent
            icon: "volume-mute"
            tone: sound.state === "connecting" || sound.state === "" ? "neutral" : "warning"
            title: sound.state === "unsupported-build" ? qsTr("Sound settings are not available in this build")
                 : sound.state === "no-server" ? qsTr("No sound server")
                 : qsTr("Connecting to the sound server…")
            message: sound.state === "connecting" ? "" : sound.statusMessage
            actionText: sound.state === "no-server" ? qsTr("Try again") : ""
            onActionTriggered: sound.reconnect()
        }
    }

    // ---- output ------------------------------------------------------------------
    NxSection {
        visible: sound.available
        title: qsTr("Output")
        description: qsTr("New sound plays on the default output. The test plays a tone on its left, then its right channel.")
        headerActions: [
            NxButton {
                text: qsTr("Test")
                iconName: "play"
                enabled: sound.sinkCount > 0
                busy: sound.testing
                onClicked: sound.playTestTone(-1)
            }
        ]

        Repeater {
            model: sound.sinks
            DeviceItem {
                required property int index
                required property var model
                backend: sound
                output: true
                position: index
                deviceIndex: model.deviceIndex
                description: model.description
                volume: model.volume
                muted: model.muted
                isDefault: model.isDefault
                volumeWritable: model.volumeWritable
                ports: model.ports
                activePort: model.activePort
                maximum: sound.maxVolume
                sliderWidth: page.sliderWidth
                revision: page.revision
            }
        }
        EmptyRow {
            visible: sound.sinkCount === 0
            text: qsTr("The sound server reports no output device.")
        }
    }

    // ---- input ---------------------------------------------------------------------
    NxSection {
        visible: sound.available
        title: qsTr("Input")

        Repeater {
            model: sound.sources
            DeviceItem {
                required property int index
                required property var model
                backend: sound
                output: false
                position: index
                deviceIndex: model.deviceIndex
                description: model.description
                volume: model.volume
                muted: model.muted
                isDefault: model.isDefault
                volumeWritable: model.volumeWritable
                ports: model.ports
                activePort: model.activePort
                maximum: sound.maxVolume
                sliderWidth: page.sliderWidth
                revision: page.revision
            }
        }
        EmptyRow {
            visible: sound.sourceCount === 0
            text: qsTr("The sound server reports no microphone or other input device.")
        }
        NxRow {
            visible: sound.sourceCount > 0
            icon: "activity"
            title: qsTr("Input level")
            subtitle: sound.meterStatus !== "" ? sound.meterStatus
                    : sound.defaultSourceName === "" ? qsTr("Choose a default input to see its level.")
                    : sound.meterRunning ? qsTr("Live level of %1").arg(sound.defaultSourceDescription)
                    : qsTr("Paused while this window is in the background.")

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                visible: sound.meterRunning
                width: page.sliderWidth + 90
                height: 8
                radius: 4
                color: Theme.bgPressed
                Accessible.role: Accessible.ProgressBar
                Accessible.name: qsTr("Input level")
                Rectangle {
                    width: Math.max(0, Math.min(1, sound.inputLevel)) * parent.width
                    height: parent.height
                    radius: 4
                    color: sound.inputLevel > 0.9 ? Theme.danger : sound.inputLevel > 0.7 ? Theme.warning : Theme.success
                    Behavior on width { NumberAnimation { duration: Theme.motionInstant } }
                }
            }
        }
    }

    // ---- applications ----------------------------------------------------------------
    NxSection {
        visible: sound.available
        title: qsTr("Applications")
        description: qsTr("Volume of each application that is playing sound.")

        Repeater {
            model: sound.streams
            NxRow {
                id: stream
                required property int streamIndex
                required property string name
                required property string application
                required property int volume
                required property bool muted
                required property bool hasVolume
                required property bool volumeWritable

                icon: "music"
                title: application
                subtitle: name

                VolumeControl {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: stream.hasVolume
                    volume: stream.volume
                    muted: stream.muted
                    writable: stream.volumeWritable
                    maximum: sound.maxVolume
                    sliderWidth: page.sliderWidth
                    subject: stream.application
                    onVolumeRequested: (percent) => sound.setStreamVolume(stream.streamIndex, percent)
                    onMuteRequested: (muted) => sound.setStreamMuted(stream.streamIndex, muted)
                }
            }
        }
        EmptyRow {
            visible: sound.streamCount === 0
            text: qsTr("No application is playing sound right now.")
        }
    }

    // ---- card profiles -----------------------------------------------------------------
    NxSection {
        visible: sound.available && sound.cardCount > 0
        title: qsTr("Device profiles")
        description: qsTr("How a sound card is used: stereo or surround, analogue or HDMI, with or without its inputs.")

        Repeater {
            model: sound.cards
            NxRow {
                id: card
                required property int cardIndex
                required property string description
                required property var profiles
                required property int activeProfile

                icon: "sliders"
                title: description
                ComboBox {
                    width: Math.max(220, Math.min(340, page.width * 0.4))
                    model: card.profiles
                    textRole: "text"
                    currentIndex: (page.revision, card.activeProfile)
                    Accessible.name: qsTr("Profile of %1").arg(card.description)
                    onActivated: (index) => sound.setCardProfile(card.cardIndex, index)
                }
            }
        }
    }

    NxSection {
        visible: sound.available
        title: qsTr("Volume limit")
        NxRow {
            icon: "alert-triangle"
            title: qsTr("Allow volumes above 100 %")
            subtitle: qsTr("Up to 150 %. Louder than the device is meant to play: sound may distort.")
            Switch {
                anchors.verticalCenter: parent.verticalCenter
                checked: sound.allowOveramplification
                Accessible.name: qsTr("Allow volumes above 100 percent")
                onToggled: sound.allowOveramplification = checked
            }
        }
    }
}
