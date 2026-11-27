// SPDX-License-Identifier: GPL-3.0-or-later
// Four square-ended corners around a rectangle: the frame of whatever is the
// current target (the password row, a confirmation). The item itself is the
// target rectangle; the corners sit "spread" pixels outside it. Decoration
// only: it takes no input and no focus.
import QtQuick 2.15
import "../tokens.js" as T

Item {
    id: brackets

    property real u: 1
    property real spread: 0
    property real arm: Math.round(14 * u)
    property real thickness: Math.max(1, Math.round(2 * u))
    property color color: T.slate

    Repeater {
        model: 4
        Item {
            readonly property bool atRight: index % 2 === 1
            readonly property bool atBottom: index >= 2
            x: Math.round(atRight ? brackets.width + brackets.spread - brackets.arm : -brackets.spread)
            y: Math.round(atBottom ? brackets.height + brackets.spread - brackets.arm : -brackets.spread)
            width: brackets.arm
            height: brackets.arm

            Rectangle {
                y: parent.atBottom ? parent.height - height : 0
                width: parent.width
                height: brackets.thickness
                color: brackets.color
            }
            // Beside the first arm, not over it: translucent colours do not add up.
            Rectangle {
                x: parent.atRight ? parent.width - width : 0
                y: parent.atBottom ? 0 : brackets.thickness
                width: brackets.thickness
                height: parent.height - brackets.thickness
                color: brackets.color
            }
        }
    }
}
