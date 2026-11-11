// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.nexus.ui

// Status glyphs that show a level, which a fixed symbolic icon cannot:
//
//   kind "wifi"      the Wi-Fi fan with 'level' (0–3) of its three arcs lit
//   kind "battery"   the battery outline filled to 'level' (0–1)
//
// Drawn on the 24-unit grid and with the stroke of the NEXUS symbolic icon
// set, so they sit next to NxIcon glyphs without looking foreign.
Canvas {
    id: root

    property string kind: "wifi"
    property real level: 0
    property real size: Theme.iconSmall
    property color color: Theme.textPrimary
    // Colour of the battery's charge (e.g. a warning colour when low).
    property color fillColor: color
    // Colour of the parts that are not lit.
    readonly property color dimColor: Theme.alpha(color, 0.28)

    implicitWidth: size
    implicitHeight: size
    width: size
    height: size

    onKindChanged: requestPaint()
    onLevelChanged: requestPaint()
    onColorChanged: requestPaint()
    onFillColorChanged: requestPaint()
    onSizeChanged: requestPaint()

    onPaint: {
        const ctx = getContext("2d")
        ctx.reset()
        ctx.scale(width / 24, height / 24)
        ctx.lineWidth = 1.75
        ctx.lineCap = "round"
        ctx.lineJoin = "round"

        if (kind === "wifi") {
            // Three arcs around one centre, widest first, and the dot.
            const radii = [13.5, 9, 4.5]
            const half = 42.5 * Math.PI / 180
            for (let i = 0; i < 3; i++) {
                ctx.beginPath()
                ctx.strokeStyle = (3 - i) <= level ? color : dimColor
                ctx.arc(12, 19.2, radii[i], -Math.PI / 2 - half, -Math.PI / 2 + half, false)
                ctx.stroke()
            }
            ctx.beginPath()
            ctx.fillStyle = color
            ctx.arc(12, 19, 1.3, 0, Math.PI * 2, false)
            ctx.fill()
        } else if (kind === "battery") {
            ctx.strokeStyle = color
            ctx.beginPath()
            ctx.roundedRect(3, 8, 15.5, 8, 2, 2)
            ctx.stroke()
            ctx.beginPath()
            ctx.moveTo(21, 10.7)
            ctx.lineTo(21, 13.3)
            ctx.stroke()
            const charge = Math.max(0, Math.min(1, level))
            if (charge > 0) {
                ctx.fillStyle = fillColor
                ctx.beginPath()
                ctx.roundedRect(4.9, 9.9, Math.max(1.2, 11.7 * charge), 4.2, 0.8, 0.8)
                ctx.fill()
            }
        }
    }
}
