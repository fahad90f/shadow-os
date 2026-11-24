// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.shadow.ui

// Decoration for the analysts' panel: a small web of linked points in the
// outline of a brain, drawn here. While an analysis runs it glows brighter.
Canvas {
    id: art

    property color tone: Theme.accent
    property color second: Theme.accentBlue
    property bool busy: false

    implicitWidth: 120
    implicitHeight: 96
    Accessible.ignored: true
    onToneChanged: requestPaint()
    onBusyChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()

    // Points of two lobes and a stem, in a 0..1 box (fixed: the same drawing every time).
    readonly property var points: [
        [0.20, 0.42], [0.27, 0.24], [0.40, 0.14], [0.54, 0.12], [0.68, 0.17], [0.80, 0.30], [0.86, 0.46],
        [0.82, 0.62], [0.70, 0.72], [0.56, 0.74], [0.42, 0.70], [0.29, 0.62], [0.36, 0.40], [0.50, 0.32],
        [0.64, 0.38], [0.72, 0.52], [0.58, 0.56], [0.44, 0.54], [0.50, 0.46], [0.60, 0.88], [0.66, 0.97]
    ]

    onPaint: {
        var ctx = getContext("2d")
        var w = width, h = height
        ctx.reset()
        var strength = busy ? 1.0 : 0.7

        var halo = ctx.createRadialGradient(w * 0.54, h * 0.45, 1, w * 0.54, h * 0.45, w * 0.55)
        halo.addColorStop(0, Qt.rgba(tone.r, tone.g, tone.b, 0.30 * strength))
        halo.addColorStop(1, Qt.rgba(tone.r, tone.g, tone.b, 0))
        ctx.fillStyle = halo
        ctx.fillRect(0, 0, w, h)

        // Links between points that are close to each other.
        ctx.lineWidth = 1.3
        for (var i = 0; i < points.length; i++) {
            for (var j = i + 1; j < points.length; j++) {
                var dx = points[i][0] - points[j][0], dy = points[i][1] - points[j][1]
                var d = Math.sqrt(dx * dx + dy * dy)
                if (d > 0.30)
                    continue
                var mixed = (i + j) % 3 === 0 ? second : tone
                ctx.strokeStyle = Qt.rgba(mixed.r, mixed.g, mixed.b, Math.min(1, (1.25 - d * 3.2) * strength))
                ctx.beginPath()
                ctx.moveTo(points[i][0] * w, points[i][1] * h)
                ctx.lineTo(points[j][0] * w, points[j][1] * h)
                ctx.stroke()
            }
        }
        for (var k = 0; k < points.length; k++) {
            var x = points[k][0] * w, y = points[k][1] * h
            var r = (k % 4 === 0 ? 2.2 : 1.4)
            var dot = ctx.createRadialGradient(x, y, 0, x, y, r * 3)
            dot.addColorStop(0, Qt.rgba(0.88, 0.90, 1, 0.95 * strength))
            dot.addColorStop(0.4, Qt.rgba(tone.r, tone.g, tone.b, 0.6 * strength))
            dot.addColorStop(1, Qt.rgba(tone.r, tone.g, tone.b, 0))
            ctx.fillStyle = dot
            ctx.fillRect(x - r * 3, y - r * 3, r * 6, r * 6)
        }
    }
}
