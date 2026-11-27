// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.shadow.ui

// Four square-ended corners around a rectangle, 2 px thick: the mark that the
// start sequence sets on the dock when it is complete. The item itself is the
// framed rectangle; the corners sit "spread" pixels outside it. Decoration
// only: no handlers, no focus.
Item {
    id: brackets

    property real spread: 0
    property real arm: 14
    property color color: Theme.accent

    Repeater {
        model: 4
        Item {
            required property int index
            readonly property bool atRight: index % 2 === 1
            readonly property bool atBottom: index >= 2
            x: Math.round(atRight ? brackets.width + brackets.spread - brackets.arm : -brackets.spread)
            y: Math.round(atBottom ? brackets.height + brackets.spread - brackets.arm : -brackets.spread)
            width: brackets.arm
            height: brackets.arm

            Rectangle {
                y: parent.atBottom ? parent.height - height : 0
                width: parent.width
                height: 2
                color: brackets.color
            }
            Rectangle {
                x: parent.atRight ? parent.width - width : 0
                y: parent.atBottom ? 0 : 2
                width: 2
                height: parent.height - 2
                color: brackets.color
            }
        }
    }
}
