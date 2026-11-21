// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import org.shadow.assistant

// A small live level: five capsules that rise with the voice (mic or hers).
Row {
    id: bars
    property real level: 0
    property color tint: Pe.cyan
    property real maxHeight: 22
    spacing: 3
    Accessible.role: Accessible.Indicator
    Accessible.name: qsTr("Level %1 percent").arg(Math.round(level * 100))
    Repeater {
        model: [0.45, 0.8, 1.0, 0.75, 0.5]
        delegate: Rectangle {
            required property real modelData
            anchors.verticalCenter: parent ? parent.verticalCenter : undefined
            width: 4
            radius: 2
            height: Math.max(4, bars.maxHeight * Math.min(1, 0.18 + bars.level * modelData))
            color: Pe.mix(bars.tint, Pe.violet, modelData * 0.6)
        }
    }
}
