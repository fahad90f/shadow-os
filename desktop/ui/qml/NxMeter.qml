// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts

// Labelled horizontal usage bar (disk usage, memory, quotas).
// The bar turns warning/danger coloured as it fills, unless 'toned' is false.
ColumnLayout {
    id: root

    property string label: ""
    property string valueText: ""
    property real value: 0              // 0..1
    property bool toned: true
    property color color: !toned ? Theme.accent
        : value >= 0.92 ? Theme.danger
        : value >= 0.80 ? Theme.warning : Theme.accent

    spacing: Theme.spaceXs + 2
    Accessible.role: Accessible.ProgressBar
    Accessible.name: label + " " + valueText

    RowLayout {
        Layout.fillWidth: true
        visible: root.label !== "" || root.valueText !== ""
        Text {
            Layout.fillWidth: true
            text: root.label
            color: Theme.textSecondary
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
            elide: Text.ElideRight
        }
        Text {
            text: root.valueText
            color: Theme.textPrimary
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
            font.weight: Theme.weightMedium
        }
    }
    Rectangle {
        Layout.fillWidth: true
        implicitHeight: 6
        radius: 3
        color: Theme.bgPressed
        Rectangle {
            width: Math.max(0, Math.min(1, root.value)) * parent.width
            height: parent.height
            radius: 3
            color: root.color
            Behavior on width { NumberAnimation { duration: Theme.motionSlow; easing.type: Easing.OutCubic } }
            Behavior on color { ColorAnimation { duration: Theme.motionNormal } }
        }
    }
}
