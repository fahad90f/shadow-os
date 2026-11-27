// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.shadow.ui

// A scan line that crosses a card once, at constant speed: the front behind
// which the content appears. A bright 2 px line with a fading tail behind
// it. It is only there while it travels (0 < progress < 1).
//
//   FxBeam { anchors.fill: card; inset: card.radius; from: search.y + search.height; progress: root.lin(0.13, 0.8) }
//
// The software renderer gets the plain line without the tail
// (Appearance.softwareRendering).
Item {
    id: root

    // 0 = at 'from' .. 1 = at 'to'.
    property real progress: 0
    // Which way it travels: "down" | "up" | "right" | "left".
    property string direction: "down"
    // Start and end along the direction of travel, in this item's
    // coordinates; to < 0: the far edge.
    property real from: 0
    property real to: -1
    // Kept clear at both ends of the line (the card's corner radius).
    property real inset: 0
    property color color: Theme.accent
    property real tail: 28
    readonly property real coreWidth: 2

    readonly property bool vertical: direction === "down" || direction === "up"
    readonly property bool forward: direction === "down" || direction === "right"
    readonly property real far: to >= 0 ? to : (vertical ? height : width)
    readonly property real position: from + (far - from) * progress

    visible: progress > 0 && progress < 1
    // Fades in over the first and out over the last tenth of its way.
    opacity: Math.min(1, progress * 10, (1 - progress) * 10)

    Rectangle {
        id: tailItem
        visible: !Appearance.softwareRendering
        readonly property real length: Math.min(root.tail, Math.abs(root.position - root.from))
        x: root.vertical ? root.inset : (root.forward ? root.position - length : root.position)
        y: root.vertical ? (root.forward ? root.position - length : root.position) : root.inset
        width: root.vertical ? root.width - root.inset * 2 : length
        height: root.vertical ? length : root.height - root.inset * 2
        gradient: Gradient {
            orientation: root.vertical ? Gradient.Vertical : Gradient.Horizontal
            GradientStop { position: 0; color: Theme.alpha(root.color, root.forward ? 0 : 0.3) }
            GradientStop { position: 1; color: Theme.alpha(root.color, root.forward ? 0.3 : 0) }
        }
    }
    Rectangle {
        x: root.vertical ? root.inset : Math.round(root.position - root.coreWidth / 2)
        y: root.vertical ? Math.round(root.position - root.coreWidth / 2) : root.inset
        width: root.vertical ? root.width - root.inset * 2 : root.coreWidth
        height: root.vertical ? root.coreWidth : root.height - root.inset * 2
        color: root.color
    }
}
