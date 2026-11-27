// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.shadow.ui

// The short line that ties a menu to what it came from: it grows from the
// origin (the panel edge, a dock icon, a menu row) to the edge of the card.
// Place the item at the origin; 'length' and 'direction' say where the card is.
//
//   FxTether { x: iconCentre; y: panelBottom; direction: "down"; length: gap; progress: root.ph(0, 0.2) }
Item {
    id: root

    // 0 = nothing .. 1 = the origin is tied to the card.
    property real progress: 0
    // Which way the card lies: "down" | "up" | "right" | "left".
    property string direction: "down"
    property real length: Theme.spaceSm
    property color color: Theme.accent
    readonly property real thickness: 2
    readonly property real node: 6

    readonly property bool vertical: direction === "down" || direction === "up"
    readonly property real sign: direction === "down" || direction === "right" ? 1 : -1
    readonly property real grown: length * Math.max(0, Math.min(1, progress))

    width: 0
    height: 0
    visible: progress > 0

    Rectangle {
        x: root.vertical ? -root.thickness / 2 : (root.sign > 0 ? 0 : -root.grown)
        y: root.vertical ? (root.sign > 0 ? 0 : -root.grown) : -root.thickness / 2
        width: root.vertical ? root.thickness : root.grown
        height: root.vertical ? root.grown : root.thickness
        color: root.color
    }
    // The node the line starts from.
    Rectangle {
        x: -root.node / 2
        y: -root.node / 2
        width: root.node
        height: root.node
        radius: root.node / 2
        color: Theme.bgOverlay
        border.width: 1.5
        border.color: root.color
        opacity: Math.min(1, root.progress * 2)
    }
}
