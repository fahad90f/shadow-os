// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.shadow.ui

// The assistant's "presence" core: a rotating 3D energy sphere — a perspective
// wireframe globe with depth shading, a specular core and tilted orbital rings
// with depth-sorted particles. It is drawn on a Canvas (not QtQuick 3D) so it
// renders identically on real GPUs and on the software renderer VMs use, and it
// reacts to orbState ("idle" | "thinking" | "speaking" | "listening" | "wake").
// Honours Appearance.reduceMotion: one calm frame, no animation.
Item {
    id: root
    property string orbState: "idle"
    implicitWidth: 160
    implicitHeight: 160

    readonly property bool animated: !Appearance.reduceMotion && visible
    readonly property color c1: Theme.accent
    readonly property color c2: Theme.accentBlue
    readonly property color c3: Theme.accentViolet

    property real t: 0
    readonly property real speed: orbState === "thinking" ? 2.4
                                 : orbState === "speaking" ? 1.6
                                 : orbState === "listening" ? 1.8
                                 : orbState === "wake" ? 3.0 : 0.5
    readonly property real intensity: orbState === "idle" ? 0.5
                                 : orbState === "speaking" ? 1.0
                                 : orbState === "wake" ? 1.0 : 0.8

    property real energy: intensity
    Behavior on energy { NumberAnimation { duration: Theme.motionSlow } }

    Timer {
        interval: 33; repeat: true; running: root.animated
        onTriggered: { root.t += 0.033 * root.speed; canvas.requestPaint() }
    }
    onOrbStateChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()
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
            const t = root.t, e = root.energy
            const R = base * 0.60                 // sphere radius
            const yaw = t * 0.7                    // spin
            const tilt = 0.42
            const ct = Math.cos(tilt), st = Math.sin(tilt)
            const col1 = root.c1, col2 = root.c2, col3 = root.c3

            // Project a unit-sphere point (theta polar, phi azimuth) to screen.
            function proj(theta, phi) {
                const X = Math.sin(theta) * Math.cos(phi + yaw)
                const Y = Math.cos(theta)
                const Z = Math.sin(theta) * Math.sin(phi + yaw)
                const y2 = Y * ct - Z * st
                const z2 = Y * st + Z * ct
                const persp = 1 + z2 * 0.22
                return { x: cx + X * R * persp, y: cy + y2 * R * persp, z: z2 }
            }

            // 1. Atmospheric glow.
            let glow = ctx.createRadialGradient(cx, cy, base * 0.08, cx, cy, base)
            glow.addColorStop(0, Theme.alpha(col2, 0.42 * e))
            glow.addColorStop(0.55, Theme.alpha(col1, 0.16 * e))
            glow.addColorStop(1, Theme.alpha(col1, 0))
            ctx.fillStyle = glow
            ctx.beginPath(); ctx.arc(cx, cy, base, 0, 2 * Math.PI); ctx.fill()

            // 2. Tilted orbital rings (perspective ellipses behind + in front).
            function drawRing(rtilt, rot, rr, color) {
                // Split into back/front halves for correct depth against the core.
                for (let half = 0; half < 2; half++) {
                    ctx.beginPath()
                    let started = false
                    for (let i = 0; i <= 48; i++) {
                        const a = (i / 48) * 2 * Math.PI
                        let X = Math.cos(a + rot), Z = Math.sin(a + rot)
                        let y1 = -Z * Math.sin(rtilt), z1 = Z * Math.cos(rtilt)
                        let y2 = y1 * ct - z1 * st
                        let z2 = y1 * st + z1 * ct
                        const front = z2 >= 0
                        if (front !== (half === 1)) { started = false; continue }
                        const persp = 1 + z2 * 0.22
                        const x = cx + X * rr * persp, y = cy + y2 * rr * persp
                        if (!started) { ctx.moveTo(x, y); started = true } else ctx.lineTo(x, y)
                    }
                    ctx.lineWidth = half === 1 ? Math.max(1.4, base * 0.03) : Math.max(1.0, base * 0.018)
                    ctx.strokeStyle = Theme.alpha(color, half === 1 ? 0.9 * e : 0.28 * e)
                    ctx.stroke()
                }
            }
            drawRing(1.15, t * 1.1, base * 0.92, col3)
            drawRing(-0.8, -t * 0.9 + 2.0, base * 0.84, col2)

            // 3. Wireframe globe, drawn in three depth buckets (one stroke each
            //    so it stays cheap under software rendering).
            const buckets = [
                { a: 0.14 * e, lw: 0.8, zmax: -0.33 },   // far
                { a: 0.34 * e, lw: 1.0, zmax: 0.33 },    // middle
                { a: 0.75 * e, lw: 1.4, zmax: 1.01 }     // near
            ]
            for (let b = 0; b < buckets.length; b++) {
                const bk = buckets[b]
                const zmin = b === 0 ? -1.01 : buckets[b - 1].zmax
                ctx.beginPath()
                // Parallels (latitude).
                for (let li = 1; li <= 4; li++) {
                    const theta = (li / 5) * Math.PI
                    let started = false
                    for (let i = 0; i <= 48; i++) {
                        const p = proj(theta, (i / 48) * 2 * Math.PI)
                        if (p.z > zmin && p.z <= bk.zmax) {
                            if (!started) { ctx.moveTo(p.x, p.y); started = true } else ctx.lineTo(p.x, p.y)
                        } else started = false
                    }
                }
                // Meridians (longitude).
                for (let mi = 0; mi < 6; mi++) {
                    const phi = (mi / 6) * Math.PI
                    let started = false
                    for (let i = 0; i <= 48; i++) {
                        const p = proj((i / 48) * Math.PI, phi)
                        if (p.z > zmin && p.z <= bk.zmax) {
                            if (!started) { ctx.moveTo(p.x, p.y); started = true } else ctx.lineTo(p.x, p.y)
                        } else started = false
                    }
                }
                ctx.lineWidth = Math.max(0.6, base * 0.006 * bk.lw)
                ctx.strokeStyle = Theme.alpha(col1, bk.a)
                ctx.stroke()
            }

            // 4. Specular energy core with an offset highlight (3D sheen).
            const coreR = R * (0.52 + 0.05 * Math.sin(t * 2.5) * e)
            let core = ctx.createRadialGradient(cx - R * 0.22, cy - R * 0.24, R * 0.02, cx, cy, coreR)
            core.addColorStop(0, Qt.rgba(1, 1, 1, 0.96))
            core.addColorStop(0.22, Theme.alpha(col1, 0.92))
            core.addColorStop(0.62, Theme.alpha(col2, 0.5))
            core.addColorStop(1, Theme.alpha(col2, 0))
            ctx.fillStyle = core
            ctx.beginPath(); ctx.arc(cx, cy, coreR, 0, 2 * Math.PI); ctx.fill()

            // 5. Depth-sorted orbiting particles (true 3D positions).
            const n = 9
            const parts = []
            for (let i = 0; i < n; i++) {
                const a = t * (0.9 + i * 0.05) + i * (2 * Math.PI / n)
                const orbT = 1.15 + 0.25 * Math.sin(t * 1.1 + i)
                let X = Math.cos(a), Z = Math.sin(a), Y = 0.35 * Math.sin(t * 0.7 + i * 1.3)
                let y2 = Y * ct - Z * st, z2 = Y * st + Z * ct
                const persp = 1 + z2 * 0.3
                parts.push({ x: cx + X * R * orbT * persp, y: cy + y2 * R * orbT * persp, z: z2, persp: persp })
            }
            parts.sort((p, q) => p.z - q.z)
            for (let i = 0; i < parts.length; i++) {
                const p = parts[i]
                const depth = (p.z + 1) / 2
                const pr = base * (0.02 + 0.03 * depth)
                let pg = ctx.createRadialGradient(p.x, p.y, 0, p.x, p.y, pr * 3.2)
                pg.addColorStop(0, Theme.alpha(col1, (0.4 + 0.55 * depth) * (0.6 + 0.4 * e)))
                pg.addColorStop(1, Theme.alpha(col1, 0))
                ctx.fillStyle = pg
                ctx.beginPath(); ctx.arc(p.x, p.y, pr * 3.2, 0, 2 * Math.PI); ctx.fill()
            }
        }
    }

    // A thin status caption under the orb.
    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.bottom
        anchors.topMargin: Theme.spaceXs
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
