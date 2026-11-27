// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.shadow.ui

// Four "L" corners around whatever this item fills: the mark that says
// "this is the thing in front". They arrive from outside and close onto the
// corners; they never overshoot.
//
//   FxBrackets { anchors.fill: card; spread: 16 * (1 - root.ph(0.55, 0.9)); strength: root.ph(0.55, 0.9) }
Item {
    id: root

    // How far outside the corners the brackets stand, in pixels.
    property real spread: 0
    // Their opacity, 0..1.
    property real strength: 1
    property color color: Theme.accent
    property real arm: 18
    property real thickness: 2

    visible: strength > 0
    opacity: strength

    Repeater {
        model: 4
        Item {
            id: corner
            required property int index
            readonly property bool atRight: index % 2 === 1
            readonly property bool atBottom: index >= 2
            x: atRight ? root.width + root.spread - root.arm : -root.spread
            y: atBottom ? root.height + root.spread - root.arm : -root.spread
            width: root.arm
            height: root.arm
            Rectangle {
                y: corner.atBottom ? parent.height - height : 0
                width: parent.width
                height: root.thickness
                color: root.color
            }
            Rectangle {
                x: corner.atRight ? parent.width - width : 0
                width: root.thickness
                height: parent.height
                color: root.color
            }
        }
    }
}
