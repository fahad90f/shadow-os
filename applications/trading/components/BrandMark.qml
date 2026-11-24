// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.shadow.ui

// The mark of Shadow Trading: a hooded figure with two lit eyes, drawn here
// (an original drawing, not an image file).
Canvas {
    id: mark

    property color tone: Theme.accent
    property color deep: Theme.bgBase

    implicitWidth: 96
    implicitHeight: 96
    Accessible.ignored: true
    onToneChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()

    onPaint: {
        var ctx = getContext("2d")
        var w = width, h = height
        ctx.reset()

        // Light behind the figure.
        var halo = ctx.createRadialGradient(w * 0.5, h * 0.5, 1, w * 0.5, h * 0.5, w * 0.58)
        halo.addColorStop(0, Qt.rgba(tone.r, tone.g, tone.b, 0.42))
        halo.addColorStop(1, Qt.rgba(tone.r, tone.g, tone.b, 0))
        ctx.fillStyle = halo
        ctx.fillRect(0, 0, w, h)

        // The hood and shoulders.
        var cloth = ctx.createLinearGradient(0, 0, 0, h)
        cloth.addColorStop(0, Qt.rgba(tone.r * 0.55, tone.g * 0.45, tone.b * 0.75, 1))
        cloth.addColorStop(1, Qt.rgba(deep.r, deep.g, deep.b, 1))
        ctx.beginPath()
        ctx.moveTo(w * 0.50, h * 0.05)
        ctx.bezierCurveTo(w * 0.80, h * 0.10, w * 0.86, h * 0.44, w * 0.79, h * 0.62)
        ctx.bezierCurveTo(w * 0.92, h * 0.68, w * 0.99, h * 0.84, w * 0.99, h * 0.98)
        ctx.lineTo(w * 0.01, h * 0.98)
        ctx.bezierCurveTo(w * 0.01, h * 0.84, w * 0.08, h * 0.68, w * 0.21, h * 0.62)
        ctx.bezierCurveTo(w * 0.14, h * 0.44, w * 0.20, h * 0.10, w * 0.50, h * 0.05)
        ctx.closePath()
        ctx.fillStyle = cloth
        ctx.fill()
        ctx.lineWidth = Math.max(1, w / 64)
        ctx.strokeStyle = Qt.rgba(tone.r, tone.g, tone.b, 0.85)
        ctx.stroke()

        // The opening of the hood: nothing to see but the eyes.
        ctx.beginPath()
        ctx.moveTo(w * 0.50, h * 0.20)
        ctx.bezierCurveTo(w * 0.70, h * 0.22, w * 0.72, h * 0.50, w * 0.62, h * 0.66)
        ctx.bezierCurveTo(w * 0.56, h * 0.74, w * 0.44, h * 0.74, w * 0.38, h * 0.66)
        ctx.bezierCurveTo(w * 0.28, h * 0.50, w * 0.30, h * 0.22, w * 0.50, h * 0.20)
        ctx.closePath()
        ctx.fillStyle = Qt.rgba(deep.r * 0.6, deep.g * 0.6, deep.b * 0.8, 1)
        ctx.fill()

        // The eyes, each with its own small glow.
        var eyes = [w * 0.42, w * 0.58]
        for (var i = 0; i < eyes.length; i++) {
            var ex = eyes[i], ey = h * 0.46
            var glow = ctx.createRadialGradient(ex, ey, 0, ex, ey, w * 0.10)
            glow.addColorStop(0, Qt.rgba(0.75, 0.80, 1, 0.95))
            glow.addColorStop(0.35, Qt.rgba(tone.r, tone.g, tone.b, 0.55))
            glow.addColorStop(1, Qt.rgba(tone.r, tone.g, tone.b, 0))
            ctx.fillStyle = glow
            ctx.fillRect(ex - w * 0.10, ey - w * 0.10, w * 0.20, w * 0.20)
            ctx.beginPath()
            ctx.ellipse(ex - w * 0.035, ey - h * 0.014, w * 0.07, h * 0.028)
            ctx.fillStyle = Qt.rgba(0.86, 0.90, 1, 1)
            ctx.fill()
        }
    }
}
