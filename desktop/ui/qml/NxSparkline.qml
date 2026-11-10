// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Shapes

// History line chart for a rolling series of samples.
//   NxSparkline { values: cpu.history; maxValue: 1.0; color: Theme.accent }
// 'values' is a JS array of numbers, oldest first. With maxValue <= 0 the
// vertical scale adapts to the largest sample in view.
Item {
    id: root

    property var values: []
    property real maxValue: 0
    property real minScale: 1e-9
    property color color: Theme.accent
    property bool fill: true
    property real lineWidth: 1.75
    property int gridLines: 3
    property int capacity: 0            // samples that span the full width; 0 → values.length

    readonly property real scaleMax: {
        if (maxValue > 0) return maxValue
        var m = minScale
        for (var i = 0; i < values.length; i++) if (values[i] > m) m = values[i]
        return m * 1.1
    }

    implicitWidth: 240
    implicitHeight: 64
    clip: true

    function buildPoints(closed) {
        var n = values.length
        var pts = []
        if (n === 0 || width <= 0 || height <= 0) return pts
        var cap = Math.max(capacity > 0 ? capacity : n, 2)
        var step = width / (cap - 1)
        var x0 = width - (n - 1) * step        // newest sample sits on the right edge
        for (var i = 0; i < n; i++) {
            var v = Math.max(0, Math.min(scaleMax, Number(values[i]) || 0))
            var y = height - 1 - (v / scaleMax) * (height - 3)
            pts.push(Qt.point(x0 + i * step, y))
        }
        if (closed) {
            pts.push(Qt.point(x0 + (n - 1) * step, height))
            pts.push(Qt.point(x0, height))
            pts.push(pts[0])
        }
        return pts
    }

    Repeater {
        model: root.gridLines
        Rectangle {
            required property int index
            y: Math.round((index + 1) * root.height / (root.gridLines + 1))
            width: root.width
            height: 1
            color: Theme.borderSubtle
            opacity: 0.6
        }
    }

    Shape {
        anchors.fill: parent
        visible: root.fill && root.values.length > 1
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeWidth: -1
            strokeColor: "transparent"
            fillGradient: LinearGradient {
                x1: 0; y1: 0; x2: 0; y2: root.height
                GradientStop { position: 0; color: Theme.alpha(root.color, 0.30) }
                GradientStop { position: 1; color: Theme.alpha(root.color, 0.0) }
            }
            PathPolyline { path: root.buildPoints(true) }
        }
    }

    Shape {
        anchors.fill: parent
        visible: root.values.length > 1
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeWidth: root.lineWidth
            strokeColor: root.color
            fillColor: "transparent"
            joinStyle: ShapePath.RoundJoin
            capStyle: ShapePath.RoundCap
            PathPolyline { path: root.buildPoints(false) }
        }
    }
}
