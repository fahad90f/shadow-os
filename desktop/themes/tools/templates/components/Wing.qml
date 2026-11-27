// SPDX-License-Identifier: GPL-3.0-or-later
// A hairline that runs sideways from the ring to a small node. grow 0..1: the
// line grows away from the ring, the node appears when the line arrives.
// The item's own x is the ring side for a rightward wing; a leftward wing
// ends with its right edge at the ring.
import QtQuick 2.15
import "../tokens.js" as T
import "Fx.js" as Fx

Item {
    id: wing

    property real u: 1
    property real grow: 1
    property bool leftward: false
    property color nodeColor: T.accent
    readonly property real node: Math.round(8 * u)
    readonly property real line: Math.max(1, Math.round(u))
    readonly property color slateColor: T.slate

    width: Math.round(120 * u)
    height: node

    Rectangle {
        readonly property real full: wing.width - wing.node
        x: wing.leftward ? wing.width - width : 0
        y: Math.round((wing.height - height) / 2)
        width: Math.round(full * Fx.lin(wing.grow, 0, 0.85))
        height: wing.line
        // Slate at 40 %: a structure line that stays readable on the
        // wallpaper at every size (the unlit trace colour vanished at
        // 1024×768).
        color: Fx.alpha(wing.slateColor, 0.4)
    }
    Rectangle {
        x: wing.leftward ? 0 : wing.width - width
        width: wing.node
        height: wing.node
        radius: width / 2
        color: "transparent"
        border.width: Math.max(1, Math.round(1.5 * wing.u))
        border.color: wing.nodeColor
        opacity: Fx.lin(wing.grow, 0.75, 1)
    }
}
