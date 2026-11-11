// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.shadow.ui

// A reactive "presence" orb for the assistant. QtQuick 3D is not available in
// this build, so this is a GPU-friendly 2.5D orb drawn on a Canvas: layered
// radial glows, an oscillating energy ring and orbiting particles. It reacts
// to orbState ("idle" | "thinking" | "speaking" | "listening") and fully
// respects Appearance.reduceMotion (it renders a single calm frame then).
Item {
    id: root
    property string orbState: "idle"
    implicitWidth: 160
    implicitHeight: 160

    readonly property bool animated: !Appearance.reduceMotion && visible
    readonly property color c1: Theme.accent
    readonly property color c2: Theme.accentBlue
    readonly property color c3: Theme.accentViolet

    // Animation clock. Driven by a frame timer so it also works under software
    // rendering; stopped entirely when motion is reduced.
    property real t: 0
    readonly property real speed: orbState === "thinking" ? 2.4
                                 : orbState === "speaking" ? 1.6
                                 : orbState === "listening" ? 1.8
                                 : orbState === "wake" ? 3.0 : 0.5
    readonly property real intensity: orbState === "idle" ? 0.5
                                 : orbState === "speaking" ? 1.0
                                 : orbState === "wake" ? 1.0 : 0.8

    // Smoothly eased "energy" level the drawing reads from.
    property real energy: intensity
    Behavior on energy { NumberAnimation { duration: Theme.motionSlow } }

    Timer {
        interval: 33; repeat: true; running: root.animated
        onTriggered: { root.t += 0.033 * root.speed; canvas.requestPaint() }
    }
    onOrbStateChanged: canvas.requestPaint()
    Component.onCompleted: canvas.requestPaint()

    Canvas {
        id: canvas
        anchors.fill: parent
        renderStrategy: Canvas.Threaded
        onPaint: {
            const ctx = getContext("2d")
            const w = width, h = height
            ctx.clearRect(0, 0, w, h)
            const cx = w / 2, cy = h / 2
            const base = Math.min(w, h) / 2
            const t = root.t
            const e = root.energy

            // Outer atmospheric glow.
            let glow = ctx.createRadialGradient(cx, cy, base * 0.1, cx, cy, base)
            glow.addColorStop(0, Theme.alpha(root.c2, 0.45 * e))
            glow.addColorStop(0.55, Theme.alpha(root.c1, 0.18 * e))
            glow.addColorStop(1, Theme.alpha(root.c1, 0))
            ctx.fillStyle = glow
            ctx.beginPath(); ctx.arc(cx, cy, base, 0, 2 * Math.PI); ctx.fill()

            // Oscillating energy ring.
            const pulse = 0.78 + 0.07 * Math.sin(t * 3.0) * e
            ctx.lineWidth = Math.max(1.5, base * 0.035)
            let ring = ctx.createLinearGradient(cx - base, cy, cx + base, cy)
            ring.addColorStop(0, Theme.alpha(root.c3, 0.9))
            ring.addColorStop(0.5, Theme.alpha(root.c1, 0.95))
            ring.addColorStop(1, Theme.alpha(root.c2, 0.9))
            ctx.strokeStyle = ring
            ctx.beginPath()
            // Wobbly ring: radius modulated around the circle for a liquid feel.
            const steps = 72
            for (let i = 0; i <= steps; i++) {
                const a = (i / steps) * 2 * Math.PI
                const wob = 1 + 0.05 * e * Math.sin(a * 3 + t * 2.2) + 0.03 * e * Math.sin(a * 5 - t * 1.7)
                const r = base * pulse * wob
                const x = cx + Math.cos(a) * r, y = cy + Math.sin(a) * r
                if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
            }
            ctx.stroke()

            // Bright core.
            let core = ctx.createRadialGradient(cx - base * 0.12, cy - base * 0.12, base * 0.02,
                                                cx, cy, base * 0.62)
            core.addColorStop(0, Qt.rgba(1, 1, 1, 0.95))
            core.addColorStop(0.25, Theme.alpha(root.c1, 0.9))
            core.addColorStop(0.7, Theme.alpha(root.c2, 0.55))
            core.addColorStop(1, Theme.alpha(root.c2, 0))
            ctx.fillStyle = core
            const coreR = base * (0.5 + 0.04 * Math.sin(t * 2.5) * e)
            ctx.beginPath(); ctx.arc(cx, cy, coreR, 0, 2 * Math.PI); ctx.fill()

            // Orbiting particles.
            const n = 7
            for (let i = 0; i < n; i++) {
                const a = t * (0.8 + i * 0.07) + i * (2 * Math.PI / n)
                const orbit = base * (0.62 + 0.12 * Math.sin(t * 1.3 + i))
                const x = cx + Math.cos(a) * orbit
                const y = cy + Math.sin(a) * orbit * 0.9
                const pr = base * (0.035 + 0.02 * (0.5 + 0.5 * Math.sin(t * 2 + i)))
                let pg = ctx.createRadialGradient(x, y, 0, x, y, pr * 3)
                pg.addColorStop(0, Theme.alpha(root.c1, 0.95))
                pg.addColorStop(1, Theme.alpha(root.c1, 0))
                ctx.fillStyle = pg
                ctx.beginPath(); ctx.arc(x, y, pr * 3, 0, 2 * Math.PI); ctx.fill()
            }
        }
    }

    // A thin status caption under the orb.
    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.bottom
        anchors.topMargin: Theme.spaceXs
        // Only on the large presence orbs; tiny brand/header orbs stay clean.
        visible: root.orbState !== "idle" && root.height >= 96
        color: Theme.textSecondary
        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeCaption
        text: root.orbState === "thinking" ? qsTr("Thinking…")
            : root.orbState === "speaking" ? qsTr("Responding…")
            : root.orbState === "listening" ? qsTr("Listening…")
            : root.orbState === "wake" ? qsTr("Yes?") : ""
    }
}
