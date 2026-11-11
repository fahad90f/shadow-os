// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui

// One level of the quick settings: a caption (what is being set), an icon
// (a mute button where muting exists), the slider and the value in percent.
ColumnLayout {
    id: root

    property string icon: ""
    // Accessible name of the slider, e.g. "Output volume".
    property string label: ""
    // What the level belongs to, e.g. the name of the sound device.
    property string caption: ""
    // The real level (0–1); the slider follows it except while it is dragged.
    property real level: 0
    property bool muted: false
    property bool canMute: false
    property string muteTooltip: ""

    signal moved(real level)
    signal muteToggled()

    spacing: 0

    Text {
        Layout.fillWidth: true
        Layout.leftMargin: Theme.spaceXs
        visible: text !== ""
        text: root.caption
        color: Theme.textSecondary
        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeCaption
        elide: Text.ElideRight
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spaceXs

        NxIconButton {
            visible: root.canMute
            iconName: root.icon
            tooltip: root.muteTooltip
            onClicked: root.muteToggled()
        }
        Item {
            visible: !root.canMute
            implicitWidth: Theme.controlHeight
            implicitHeight: Theme.controlHeight
            NxIcon { anchors.centerIn: parent; name: root.icon; color: Theme.textPrimary }
        }
        Slider {
            id: slider
            Layout.fillWidth: true
            from: 0
            to: 1
            stepSize: 0.05      // arrow keys; dragging is continuous
            opacity: root.muted ? Theme.opacityDisabled : 1
            Accessible.name: root.label
            onMoved: root.moved(value)
        }
        Text {
            Layout.preferredWidth: percentMetrics.advanceWidth
            horizontalAlignment: Text.AlignRight
            text: qsTr("%1 %").arg(Math.round((slider.pressed ? slider.value : root.level) * 100))
            color: root.muted ? Theme.textMuted : Theme.textPrimary
            font: percentMetrics.font
        }
    }

    TextMetrics {
        id: percentMetrics
        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeSmall
        text: qsTr("%1 %").arg(100)
    }
    Binding {
        target: slider
        property: "value"
        value: Math.min(1, root.level)
        when: !slider.pressed
        restoreMode: Binding.RestoreNone
    }
}
