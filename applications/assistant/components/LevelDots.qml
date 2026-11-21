// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import org.shadow.assistant

// A long dot meter for the microphone in Settings.
Row {
    id: meter
    property real level: 0
    property int dots: 24
    spacing: 4
    Accessible.role: Accessible.Indicator
    Accessible.name: qsTr("Microphone level %1 percent").arg(Math.round(level * 100))
    Repeater {
        model: meter.dots
        delegate: Rectangle {
            required property int index
            readonly property bool lit: index / meter.dots < meter.level
            anchors.verticalCenter: parent ? parent.verticalCenter : undefined
            width: 6; height: 6; radius: 3
            color: lit ? Pe.brandAt(index / meter.dots) : Pe.line
        }
    }
}
