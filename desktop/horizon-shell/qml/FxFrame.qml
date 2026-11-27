// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Shapes
import org.shadow.ui

// The outline of a card, drawn as a trace: two fronts leave the origin and
// run round the card in opposite directions until they meet.
//
//   FxFrame { anchors.fill: card; radius: card.radius; origin: "top"; progress: root.ph(0.1, 0.7) }
//
// The outline is the card's own rounded rectangle (same radius), so line
// and fill always meet. Plain QtQuick.Shapes strokes: they are drawn by the
// OpenGL and by the software renderer alike.
Item {
    id: root

    // 0 = nothing drawn .. 1 = the whole outline.
    property real progress: 1
    // Where the trace starts: "topLeft" | "top" | "bottom" | "left" | "right".
    property string origin: "topLeft"
    // For "top"/"bottom" the x, for "left"/"right" the y of the origin,
    // in this item's coordinates; < 0: the middle of that edge.
    property real originPosition: -1
    property real radius: Theme.radiusLg
    property color color: Theme.accent
    // 0..1: the soft second stroke around the line. Never drawn by the
    // software renderer (Appearance.softwareRendering).
    property real glow: 1
    readonly property real lineWidth: 1.5
    readonly property real glowWidth: 5

    // The line lies inside the card's edge.
    readonly property real inset: lineWidth / 2
    readonly property real w: Math.max(1, width - inset * 2)
    readonly property real h: Math.max(1, height - inset * 2)
    readonly property real r: Math.max(0, Math.min(radius - inset, w / 2, h / 2))
    readonly property real perimeter: 2 * (w - 2 * r) + 2 * (h - 2 * r) + 2 * Math.PI * r

    // The outline clockwise from the origin: start point, then line end and
    // arc end four times, then back to the start.
    readonly property var points: {
        const o = inset
        const ends = [[w - r, 0], [w, r], [w, h - r], [w - r, h], [r, h], [0, h - r], [0, r], [r, 0]]
        const at = (v, lo, hi) => Math.max(lo, Math.min(hi, v))
        const mid = originPosition < 0
        let start, shift
        switch (origin) {
        case "top": start = [mid ? w / 2 : at(originPosition - o, r, w - r), 0]; shift = 0; break
        case "right": start = [w, mid ? h / 2 : at(originPosition - o, r, h - r)]; shift = 2; break
        case "bottom": start = [mid ? w / 2 : at(originPosition - o, r, w - r), h]; shift = 4; break
        case "left": start = [0, mid ? h / 2 : at(originPosition - o, r, h - r)]; shift = 6; break
        default: start = [r, 0]; shift = 0
        }
        const list = [start]
        for (let i = 0; i < 8; ++i) list.push(ends[(i + shift) % 8])
        list.push(start)
        return list.map(p => Qt.point(p[0] + o, p[1] + o))
    }

    visible: progress > 0

    component Outline: ShapePath {
        id: outline
        property real lineWidth: 1
        property real shown: 1
        // One dash as long as the drawn part and a gap for the rest of the
        // outline; the pattern is shifted by half a dash, so the dash lies
        // across the origin (where the path starts and ends) and grows both
        // ways. Dash lengths are counted in line widths.
        readonly property real whole: root.perimeter / lineWidth
        readonly property real drawn: Math.max(0.001, whole * Math.min(1, shown))
        strokeWidth: lineWidth
        fillColor: "transparent"
        capStyle: ShapePath.FlatCap
        joinStyle: ShapePath.RoundJoin
        strokeStyle: shown >= 1 ? ShapePath.SolidLine : ShapePath.DashLine
        dashPattern: [drawn, Math.max(0.001, whole - drawn)]
        dashOffset: drawn / 2
        startX: root.points[0].x; startY: root.points[0].y
        PathLine { x: root.points[1].x; y: root.points[1].y }
        PathArc { x: root.points[2].x; y: root.points[2].y; radiusX: root.r; radiusY: root.r }
        PathLine { x: root.points[3].x; y: root.points[3].y }
        PathArc { x: root.points[4].x; y: root.points[4].y; radiusX: root.r; radiusY: root.r }
        PathLine { x: root.points[5].x; y: root.points[5].y }
        PathArc { x: root.points[6].x; y: root.points[6].y; radiusX: root.r; radiusY: root.r }
        PathLine { x: root.points[7].x; y: root.points[7].y }
        PathArc { x: root.points[8].x; y: root.points[8].y; radiusX: root.r; radiusY: root.r }
        PathLine { x: root.points[9].x; y: root.points[9].y }
    }

    // The soft stroke: a wider, faint line under the core.
    Shape {
        anchors.fill: parent
        visible: !Appearance.softwareRendering && root.glow > 0
        Outline {
            lineWidth: root.glowWidth
            shown: root.progress
            strokeColor: Theme.alpha(root.color, 0.22 * root.glow)
        }
    }
    Shape {
        anchors.fill: parent
        Outline {
            lineWidth: root.lineWidth
            shown: root.progress
            strokeColor: root.color
        }
    }
}
