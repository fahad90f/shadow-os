// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Shapes
import org.shadow.assistant

// Shadow's face: an abstract constellation drawn with light. Stars and
// tapered light-strokes form a temple-to-chin contour, brows, eyes, a single
// "bridge" star and a mouth that is a living waveform. A crown of small stars
// above the brows is where thoughts happen: they rise and link up while she
// thinks.
//
// Everything is Shapes + Rectangles (no Canvas, no shader, no MultiEffect),
// so it renders the same under QT_QUICK_BACKEND=software.
//
// Cost model (software renderer repaints only dirty rectangles):
//  * each part of the face is its own Shape with tight bounds, so a speaking
//    mouth repaints the mouth, listening repaints the two "ears", thinking
//    repaints the crown — never the whole face;
//  * the 30 Hz clock only runs while she listens, speaks, thinks or startles;
//  * calm moods are event-driven: a blink every few seconds and one star
//    twinkling at a time, nothing in between;
//  * `animate: false` (hidden/minimised window, reduce motion, screenshot
//    frames) stops everything; levels still update the mouth and ears.
//
// Geometry lives in a 1000 x 1000 unit box; u converts units to pixels.
Item {
    id: face

    // rest | armed | listening | thinking | writing | speaking | asking |
    // concerned | sleeping | dormant | startled  (anything else = rest)
    property string mood: "rest"
    property real level: 0               // mic level (listening) / speech level (speaking), 0..1
    property bool voiceless: false       // no voice engine: the mouth is a dotted line
    property real installProgress: -1    // >= 0 while the engine installs: the mouth fills in
    property bool animate: true
    // with reduce motion (animate false while on screen): the levels are still
    // read, a few times a second, so the mouth and ears follow the voice
    property bool sampleLevels: false
    property real frozenClock: -1        // >= 0: draw exactly this instant (screenshots)
    property bool aura: true
    property real burst: 1.0             // wake flash progress 0 -> 1 (1 = none)
    property color auraTint: Pe.violet

    implicitWidth: 320
    implicitHeight: 320

    Accessible.role: Accessible.Animation
    Accessible.name: qsTr("Shadow")

    readonly property real u: width / 1000
    readonly property bool compact: width < 150
    readonly property bool tiny: width < 64

    // ---- clocks ----------------------------------------------------------------------
    readonly property bool lively: mood === "listening" || mood === "speaking"
                                   || mood === "thinking" || mood === "startled"
    property real runClock: 2.6
    readonly property real clock: frozenClock >= 0 ? frozenClock : runClock
    // While the clock runs, the level is read on its tick, so the mouth, the
    // ears, the crown (and the voice thread, which shares this clock) change
    // together: one repaint per tick instead of one per level update too.
    readonly property bool ticking: liveClock.running
    readonly property bool sampling: levelSampler.running
    property real levelNow: 0
    readonly property real liveLevel: ticking || sampling ? levelNow : level
    Timer {
        id: liveClock
        running: face.animate && face.lively && face.frozenClock < 0 && face.visible && face.width > 0
        repeat: true
        interval: 40
        onTriggered: {
            face.levelNow = face.level
            face.runClock += 0.04
        }
        onRunningChanged: face.levelNow = face.level
    }
    Timer {
        id: levelSampler
        running: face.sampleLevels && !liveClock.running && face.lively && face.frozenClock < 0 && face.visible
                 && face.width > 0
        repeat: true
        interval: 125
        onTriggered: face.levelNow = face.level
        onRunningChanged: face.levelNow = face.level
    }
    // Calm life, event-driven so an idle face costs almost nothing: every
    // 1.5 s one star glints (a single repaint of that star, no animation) and
    // every third tick she blinks (a 240 ms animation of the eye region).
    property int lifeTick: 0
    property real blinkAmt: 0
    property int twinkleIdx: -1
    readonly property real twinkleAmt: 1
    Timer {
        running: face.animate && !face.lively && face.frozenClock < 0 && face.visible && face.width > 0
        repeat: true
        interval: 1500
        onTriggered: {
            face.lifeTick++
            if (face.blinks && face.lifeTick % 3 === 0)
                blinkAnim.restart()
            face.twinkleIdx = (face.lifeTick * 7) % 28
        }
    }
    SequentialAnimation {
        id: blinkAnim
        NumberAnimation { target: face; property: "blinkAmt"; to: 1; duration: 90; easing.type: Easing.InQuad }
        NumberAnimation { target: face; property: "blinkAmt"; to: 0; duration: 150; easing.type: Easing.OutQuad }
    }

    // ---- expressions -------------------------------------------------------
    // bl/br brow raise, inner = inner brow ends (+ worried, - focused), open =
    // lid curvature (negative = closed), gx/gy gaze, smile (+ = smile),
    // mw = mouth half-width, lean = scale towards you, turn = head tilt (deg),
    // rise = thought (crown) amount, links = crown line strength.
    readonly property var poses: ({
        "rest":      { bl: 0,  br: 0,  inner: 0,   open: 25,  gx: 0,  gy: 2,   pupil: 1.0,  smile: 11,  mw: 84,  lean: 1.0,  turn: 0,  rise: 0,   links: 0.22, glow: 0.55, dim: 1.0,  scatter: 0, warm: 0 },
        "armed":     { bl: 6,  br: 6,  inner: 0,   open: 29,  gx: 0,  gy: 0,   pupil: 1.05, smile: 10,  mw: 82,  lean: 1.0,  turn: 0,  rise: 0,   links: 0.26, glow: 0.7,  dim: 1.0,  scatter: 0, warm: 0 },
        "listening": { bl: 24, br: 24, inner: 0,   open: 40,  gx: 0,  gy: 0,   pupil: 1.2,  smile: 5,   mw: 58,  lean: 1.06, turn: 0,  rise: -8,  links: 0.3,  glow: 1.0,  dim: 1.0,  scatter: 0, warm: 0 },
        "thinking":  { bl: 16, br: 6,  inner: -4,  open: 21,  gx: 18, gy: -22, pupil: 1.0,  smile: 0,   mw: 42,  lean: 1.0,  turn: 2,  rise: 48,  links: 0.9,  glow: 0.8,  dim: 1.0,  scatter: 0, warm: 0 },
        "writing":   { bl: 2,  br: 2,  inner: 0,   open: 17,  gx: 0,  gy: 15,  pupil: 0.95, smile: 11,  mw: 70,  lean: 1.0,  turn: 0,  rise: 6,   links: 0.32, glow: 0.65, dim: 1.0,  scatter: 0, warm: 0 },
        "speaking":  { bl: 8,  br: 8,  inner: 0,   open: 27,  gx: 0,  gy: 2,   pupil: 1.05, smile: 7,   mw: 98,  lean: 1.0,  turn: 0,  rise: 0,   links: 0.3,  glow: 0.9,  dim: 1.0,  scatter: 0, warm: 0.35 },
        "asking":    { bl: 14, br: 30, inner: 2,   open: 33,  gx: -4, gy: 12,  pupil: 1.1,  smile: 6,   mw: 60,  lean: 1.03, turn: -4, rise: 0,   links: 0.3,  glow: 0.85, dim: 1.0,  scatter: 0, warm: 0 },
        "concerned": { bl: 6,  br: 6,  inner: 22,  open: 23,  gx: 0,  gy: 6,   pupil: 1.0,  smile: -15, mw: 60,  lean: 1.0,  turn: -2, rise: 0,   links: 0.18, glow: 0.6,  dim: 1.0,  scatter: 0, warm: 1.0 },
        "sleeping":  { bl: -8, br: -8, inner: 0,   open: -15, gx: 0,  gy: 0,   pupil: 0.0,  smile: 2,   mw: 48,  lean: 0.98, turn: 3,  rise: -26, links: 0.1,  glow: 0.22, dim: 0.62, scatter: 0, warm: 0 },
        "dormant":   { bl: -6, br: -6, inner: 0,   open: -14, gx: 0,  gy: 0,   pupil: 0.0,  smile: 0,   mw: 0,   lean: 0.96, turn: 0,  rise: -10, links: 0.0,  glow: 0.3,  dim: 0.78, scatter: 1, warm: 0 },
        "startled":  { bl: 44, br: 44, inner: 0,   open: 55,  gx: 0,  gy: -2,  pupil: 1.6,  smile: 0,   mw: 34,  lean: 1.08, turn: 0,  rise: 14,  links: 0.6,  glow: 1.35, dim: 1.0,  scatter: 0, warm: 0 }
    })
    readonly property var pose: poses[mood] !== undefined ? poses[mood] : poses["rest"]

    property real browL: pose.bl
    property real browR: pose.br
    property real inner: pose.inner
    property real open: pose.open
    property real gazeX: pose.gx
    property real gazeY: pose.gy
    property real pupil: pose.pupil
    property real smile: pose.smile
    property real mouthHalf: pose.mw
    property real lean: pose.lean
    property real turn: pose.turn
    property real rise: pose.rise
    property real links: pose.links
    property real glow: pose.glow
    property real dim: pose.dim
    property real scatter: pose.scatter
    property real warm: pose.warm
    Behavior on browL { NumberAnimation { duration: Pe.tMood; easing.type: Easing.OutCubic } }
    Behavior on browR { NumberAnimation { duration: Pe.tMood; easing.type: Easing.OutCubic } }
    Behavior on inner { NumberAnimation { duration: Pe.tMood; easing.type: Easing.OutCubic } }
    Behavior on open { NumberAnimation { duration: Pe.tMood; easing.type: Easing.OutCubic } }
    Behavior on gazeX { NumberAnimation { duration: Pe.tMood; easing.type: Easing.OutCubic } }
    Behavior on gazeY { NumberAnimation { duration: Pe.tMood; easing.type: Easing.OutCubic } }
    Behavior on pupil { NumberAnimation { duration: Pe.tMood; easing.type: Easing.OutBack } }
    Behavior on smile { NumberAnimation { duration: Pe.tMood; easing.type: Easing.OutCubic } }
    Behavior on mouthHalf { NumberAnimation { duration: Pe.tMood; easing.type: Easing.OutCubic } }
    Behavior on lean { NumberAnimation { duration: Pe.tMood; easing.type: Easing.OutBack } }
    Behavior on turn { NumberAnimation { duration: Pe.tMood; easing.type: Easing.OutCubic } }
    Behavior on rise { NumberAnimation { duration: Pe.tMood * 1.6; easing.type: Easing.OutCubic } }
    Behavior on links { NumberAnimation { duration: Pe.tMood * 1.4 } }
    Behavior on glow { NumberAnimation { duration: Pe.tMood } }
    Behavior on dim { NumberAnimation { duration: Pe.tMood } }
    Behavior on scatter { NumberAnimation { duration: Pe.tMood * 2.2; easing.type: Easing.InOutCubic } }
    Behavior on warm { NumberAnimation { duration: Pe.tMood } }

    readonly property bool blinks: pose.open > 10 && (mood === "rest" || mood === "armed" || mood === "speaking"
                                                    || mood === "writing" || mood === "asking")
    readonly property real eyeOpen: open + (-13 - open) * blinkAmt
    readonly property real speak: mood === "speaking" ? Math.max(0, Math.min(1, liveLevel)) : 0
    readonly property real hear: mood === "listening" ? Math.max(0, Math.min(1, liveLevel)) : 0
    readonly property real think: Math.max(0, Math.min(1, rise / 48))

    // ---- geometry (units) ----------------------------------------------------
    readonly property var contourBase: [[262, 352], [236, 478], [266, 600], [326, 704], [410, 776], [500, 802],
                                        [590, 776], [674, 704], [734, 600], [764, 478], [738, 352]]
    readonly property var scatterOff: [[-70, -60], [-120, 20], [-60, 90], [-100, 70], [-30, 110], [10, 90],
                                       [60, 120], [110, 60], [70, 90], [120, -10], [80, -70]]
    // x, y, lift factor, size, thought-only (1 = only while thinking)
    readonly property var crownBase: [[500, 206, 1.0, 1.0, 0], [426, 222, 0.85, 0.8, 0], [574, 222, 0.9, 0.8, 0],
                                      [356, 266, 0.6, 0.65, 0], [644, 266, 0.65, 0.65, 0], [468, 150, 1.2, 0.6, 0],
                                      [548, 132, 1.3, 0.55, 0], [400, 150, 1.0, 0.5, 1], [612, 168, 1.1, 0.55, 1],
                                      [520, 70, 1.4, 0.5, 1]]
    // While she thinks the crown drifts into an irregular constellation that
    // climbs up and to the right, where her gaze goes.
    readonly property var crownThink: [[502, 186], [432, 210], [612, 172], [364, 252], [682, 224], [552, 130],
                                       [628, 96], [474, 142], [694, 128], [748, 60]]
    readonly property var crownScatter: [[-40, -110], [-120, -50], [110, -70], [-150, 40], [150, 50], [-70, -130],
                                         [90, -140], [-130, -100], [130, -110], [20, -160]]
    readonly property var eyeL: [380, 454]
    readonly property var eyeR: [620, 454]

    readonly property var contour: {
        const out = []
        for (let i = 0; i < contourBase.length; ++i)
            out.push([contourBase[i][0] + scatterOff[i][0] * scatter, contourBase[i][1] + scatterOff[i][1] * scatter])
        return out
    }
    readonly property var crown: {
        const out = []
        const lift = Math.min(0, rise)          // sleeping: the crown settles
        const c = think > 0 ? clock : 0         // only thinking makes the crown drift
        for (let j = 0; j < crownBase.length; ++j) {
            let x = crownBase[j][0] + (crownThink[j][0] - crownBase[j][0]) * think
            let y = crownBase[j][1] + (crownThink[j][1] - crownBase[j][1]) * think - lift * crownBase[j][2]
            if (think > 0) {
                x += Math.cos(c * 0.8 + j * 1.7) * 9 * think
                y += Math.sin(c * 0.65 + j * 1.1) * 7 * think
            }
            x += crownScatter[j][0] * scatter
            y += crownScatter[j][1] * scatter
            out.push([x, y])
        }
        return out
    }
    readonly property var browStars: [
        [318, 378 - browL * 0.7], [376, 350 - browL], [432, 362 - browL - inner],
        [568, 362 - browR - inner], [624, 350 - browR], [682, 378 - browR * 0.7]
    ]

    // ---- helpers -------------------------------------------------------------
    function lerp(a, b, t) { return [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t] }
    function segPts(a, b, gap, n) {
        const dx = b[0] - a[0], dy = b[1] - a[1]
        const len = Math.sqrt(dx * dx + dy * dy) || 1
        const g = Math.min(gap, len * 0.3) / len
        const pts = []
        for (let i = 0; i < n; ++i)
            pts.push(lerp(a, b, g + (1 - 2 * g) * i / (n - 1)))
        return pts
    }
    // Quadratic curve that passes through `m` at t = 0.5.
    function throughPts(a, m, b, n) {
        const c = [2 * m[0] - (a[0] + b[0]) / 2, 2 * m[1] - (a[1] + b[1]) / 2]
        const pts = []
        for (let i = 0; i < n; ++i) {
            const t = i / (n - 1), s = 1 - t
            pts.push([s * s * a[0] + 2 * s * t * c[0] + t * t * b[0],
                      s * s * a[1] + 2 * s * t * c[1] + t * t * b[1]])
        }
        return pts
    }
    // A tapered light-stroke along pts (units), as a closed polygon in pixels
    // relative to the region whose top-left is (ox, oy) in units.
    function ribbon(pts, hw, taper, ox, oy) {
        const n = pts.length
        if (n < 2) return []
        const minHw = (tiny ? 0.42 : 0.5) / u
        const left = [], right = []
        for (let i = 0; i < n; ++i) {
            const p = pts[i], a = pts[Math.max(0, i - 1)], b = pts[Math.min(n - 1, i + 1)]
            const dx = b[0] - a[0], dy = b[1] - a[1]
            const len = Math.sqrt(dx * dx + dy * dy) || 1
            const nx = -dy / len, ny = dx / len
            const t = i / (n - 1)
            const w = Math.max(minHw, hw) * (taper ? (0.22 + 0.78 * Math.sin(Math.PI * t)) : 1)
            left.push(Qt.point((p[0] + nx * w - ox) * u, (p[1] + ny * w - oy) * u))
            right.push(Qt.point((p[0] - nx * w - ox) * u, (p[1] - ny * w - oy) * u))
        }
        right.reverse()
        return left.concat(right)
    }
    function disc(cx, cy, r, ox, oy) {
        const pts = []
        for (let i = 0; i < 10; ++i) {
            const a = i / 10 * Math.PI * 2
            pts.push(Qt.point((cx + Math.cos(a) * r - ox) * u, (cy + Math.sin(a) * r - oy) * u))
        }
        return pts
    }
    function lid(c, half, curve, n) {
        return throughPts([c[0] - half, c[1]], [c[0], c[1] - curve], [c[0] + half, c[1]], n)
    }
    // half-width in units at full size; ink on light needs a little more weight than light on dark
    readonly property real stroke: (compact ? 3.4 : 2.6) * (Pe.dark ? 1.0 : 1.22)

    // ---- regions (units): each is one Shape with tight bounds -------------------------
    // jaw & cheeks: x 150..850, y 250..880
    readonly property var jawLines: {
        const out = [], c = contour, b = browStars
        for (let i = 2; i < 8; ++i)
            out.push(ribbon(segPts(c[i], c[i + 1], 30, 7), stroke, true, 150, 250))
        return out
    }
    readonly property var jawSoft: {
        const out = [], c = contour, b = browStars
        out.push(ribbon(segPts(c[1], c[2], 30, 7), stroke * 0.9, true, 150, 250))
        out.push(ribbon(segPts(c[8], c[9], 30, 7), stroke * 0.9, true, 150, 250))
        if (!tiny) {
            out.push(ribbon(segPts(c[0], b[0], 26, 6), stroke * 0.8, true, 150, 250))
            out.push(ribbon(segPts(c[10], b[5], 26, 6), stroke * 0.8, true, 150, 250))
            out.push(ribbon(segPts(c[0], c[1], 30, 6), stroke * 0.7, true, 150, 250))
            out.push(ribbon(segPts(c[9], c[10], 30, 6), stroke * 0.7, true, 150, 250))
        }
        return out
    }
    // brows & eyes: x 270..730, y 270..540
    readonly property var browsAndLids: {
        const out = [], b = browStars
        out.push(ribbon(throughPts(b[0], b[1], b[2], 9), stroke * 0.95, true, 270, 270))
        out.push(ribbon(throughPts(b[3], b[4], b[5], 9), stroke * 0.95, true, 270, 270))
        const half = 58 + Math.max(0, eyeOpen - 30) * 0.35
        out.push(ribbon(lid(eyeL, half, eyeOpen, 11), stroke * 1.25, true, 270, 270))
        out.push(ribbon(lid(eyeR, half, eyeOpen, 11), stroke * 1.25, true, 270, 270))
        return out
    }
    readonly property var lowerLids: {
        const out = []
        if (eyeOpen > 6 && !tiny) {
            out.push(ribbon(lid(eyeL, 50, -eyeOpen * 0.42, 9), stroke * 0.9, true, 270, 270))
            out.push(ribbon(lid(eyeR, 50, -eyeOpen * 0.42, 9), stroke * 0.9, true, 270, 270))
        }
        return out
    }
    // crown: x 250..820, y 0..330
    readonly property var crownLinks: {
        const out = [], s = crown
        const tiara = [[3, 1], [1, 0], [0, 2], [2, 4]]
        const chain = [[3, 1], [1, 0], [0, 5], [5, 6], [6, 9], [0, 2], [2, 4], [5, 7], [6, 8]]
        if (think < 0.5) {
            for (let k = 0; k < tiara.length; ++k)
                out.push(ribbon(segPts(s[tiara[k][0]], s[tiara[k][1]], 22, 6), stroke * 0.85, true, 250, 0))
            return out
        }
        // the chain is traced one link at a time, then starts over
        const traced = (frozenClock >= 0 || !animate) ? chain.length
                     : Math.floor((clock * 3.2) % (chain.length + 4)) + 1
        for (let k = 0; k < Math.min(traced, chain.length); ++k)
            out.push(ribbon(segPts(s[chain[k][0]], s[chain[k][1]], 22, 6), stroke * 0.85, true, 250, 0))
        return out
    }
    // mouth: x 360..640, y 560..760
    readonly property var mouthShapes: {
        const out = []
        if (mouthHalf < 4) return out
        const ox = 360, oy = 560
        const cx = 500, cy = 652
        const n = compact ? 15 : 27
        const amp = speak > 0 ? (7 + 50 * speak) : 0
        const t0 = speak > 0 ? clock : 0
        const strand = (phase, ampK, freq) => {
            const pts = []
            for (let i = 0; i < n; ++i) {
                const t = i / (n - 1)
                const x = cx - mouthHalf + 2 * mouthHalf * t
                const env = Math.sin(Math.PI * t)
                let y = cy + smile * (1 - (2 * t - 1) * (2 * t - 1))
                if (amp > 0)
                    y += amp * ampK * env * Math.sin(2 * Math.PI * freq * t + phase)
                pts.push([x, y])
            }
            return pts
        }
        if (voiceless) {
            const pts = strand(0, 0, 1)
            const filled = installProgress > 0 ? Math.floor(installProgress * (n - 1)) : 0
            if (filled >= 1)
                out.push(ribbon(pts.slice(0, filled + 1), stroke * 1.4, false, ox, oy))
            for (let i = filled + (filled >= 1 ? 2 : 0); i < n; i += 2)
                out.push(disc(pts[i][0], pts[i][1], Math.max(stroke * 1.5, 1.1 / u), ox, oy))
            return out
        }
        out.push(ribbon(strand(t0 * 9.0, 1.0, 1.5), stroke * 1.5, true, ox, oy))
        if (speak > 0.02)
            out.push(ribbon(strand(t0 * 9.0 + 0.9, 0.55, 1.5), stroke * 0.8, true, ox, oy))
        return out
    }
    // ears (listening): left x 60..330, right x 670..940, y 280..720
    function earArcs(side) {
        const out = []
        if (hear <= 0.01 || compact) return out
        const ox = side === 0 ? 60 : 670, oy = 280
        const drift = (clock * 0.9) % 1
        for (let k = 0; k < 3; ++k) {
            const r = 360 + (k + 1 - drift) * 46
            const span = 0.34 - k * 0.04
            const pts = []
            for (let i = 0; i < 9; ++i) {
                const a = (side === 0 ? Math.PI : 0) + (i / 8 - 0.5) * 2 * span
                pts.push([500 + Math.cos(a) * r, 500 + Math.sin(a) * r * 1.05])
            }
            out.push(ribbon(pts, stroke * (1.25 - k * 0.2), true, ox, oy))
        }
        return out
    }

    // ---- drawing ---------------------------------------------------------------
    // Soft aura (radial gradient: works in every renderer), cached as a layer.
    Shape {
        id: auraShape
        visible: face.aura && !face.tiny
        anchors.fill: parent
        opacity: Math.min(1, face.glow) * face.dim
        layer.enabled: Pe.software
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeWidth: -1
            fillGradient: RadialGradient {
                centerX: 500 * face.u; centerY: 500 * face.u
                focalX: centerX; focalY: centerY
                centerRadius: 500 * face.u; focalRadius: 0
                GradientStop { position: 0.0; color: Pe.dark ? Pe.alpha(face.auraTint, 0.2) : Pe.alpha("#FFFFFF", 0.9) }
                GradientStop { position: 0.45; color: Pe.dark ? Pe.alpha(Pe.mix(face.auraTint, Pe.blue, 0.5), 0.09)
                                                              : Pe.alpha("#EFEAFF", 0.55) }
                GradientStop { position: 1.0; color: Pe.dark ? Pe.alpha(Pe.blue, 0.0) : Pe.alpha("#EFEAFF", 0.0) }
            }
            PathAngleArc {
                centerX: 500 * face.u; centerY: 500 * face.u
                radiusX: 500 * face.u; radiusY: 500 * face.u
                startAngle: 0; sweepAngle: 360
            }
        }
    }

    Item {
        id: body
        anchors.fill: parent
        opacity: face.dim
        rotation: face.turn
        scale: face.lean
        transformOrigin: Item.Center

        // listening "ears": sound arriving from both sides
        Region {
            u: face.u
            visible: face.hear > 0.01 && !face.compact
            rx: 60; ry: 280; rw: 270; rh: 440
            opacity: 0.25 + 0.75 * face.hear
            ShapePath {
                strokeWidth: -1
                fillGradient: LinearGradient {
                    x1: 0; y1: 0; x2: 270 * face.u; y2: 0
                    GradientStop { position: 0.0; color: Pe.alpha(Pe.starLineA, 0.85) }
                    GradientStop { position: 1.0; color: Pe.alpha(Pe.starLineA, 0.25) }
                }
                PathMultiline { paths: face.earArcs(0) }
            }
        }
        Region {
            u: face.u
            visible: face.hear > 0.01 && !face.compact
            rx: 670; ry: 280; rw: 270; rh: 440
            opacity: 0.25 + 0.75 * face.hear
            ShapePath {
                strokeWidth: -1
                fillGradient: LinearGradient {
                    x1: 0; y1: 0; x2: 270 * face.u; y2: 0
                    GradientStop { position: 0.0; color: Pe.alpha(Pe.starLineB, 0.25) }
                    GradientStop { position: 1.0; color: Pe.alpha(Pe.starLineB, 0.85) }
                }
                PathMultiline { paths: face.earArcs(1) }
            }
        }

        // jaw & cheeks
        Region {
            u: face.u
            rx: 150; ry: 250; rw: 700; rh: 630
            ShapePath {
                strokeWidth: -1
                fillGradient: LinearGradient {
                    x1: 70 * face.u; y1: 0; x2: 630 * face.u; y2: 0
                    GradientStop { position: 0.0; color: Pe.alpha(Pe.starLineA, (Pe.dark ? 0.8 : 0.84) * (1 - face.scatter * 0.85)) }
                    GradientStop { position: 0.5; color: Pe.alpha(Pe.blue, (Pe.dark ? 0.74 : 0.8) * (1 - face.scatter * 0.85)) }
                    GradientStop { position: 1.0; color: Pe.alpha(Pe.starLineB, (Pe.dark ? 0.82 : 0.86) * (1 - face.scatter * 0.85)) }
                }
                PathMultiline { paths: face.jawLines }
            }
            ShapePath {
                strokeWidth: -1
                fillGradient: LinearGradient {
                    x1: 70 * face.u; y1: 0; x2: 630 * face.u; y2: 0
                    GradientStop { position: 0.0; color: Pe.alpha(Pe.starLineA, (Pe.dark ? 0.36 : 0.42) * (1 - face.scatter)) }
                    GradientStop { position: 1.0; color: Pe.alpha(Pe.starLineB, (Pe.dark ? 0.36 : 0.42) * (1 - face.scatter)) }
                }
                PathMultiline { paths: face.jawSoft }
            }
        }

        // brows & eyes
        Region {
            u: face.u
            rx: 270; ry: 270; rw: 460; rh: 270
            ShapePath {
                strokeWidth: -1
                fillGradient: LinearGradient {
                    x1: 50 * face.u; y1: 0; x2: 410 * face.u; y2: 0
                    GradientStop { position: 0.0; color: Pe.alpha(Pe.starLineA, (Pe.dark ? 0.82 : 0.86) * (1 - face.scatter * 0.85)) }
                    GradientStop { position: 0.5; color: Pe.alpha(Pe.blue, (Pe.dark ? 0.76 : 0.82) * (1 - face.scatter * 0.85)) }
                    GradientStop { position: 1.0; color: Pe.alpha(Pe.starLineB, (Pe.dark ? 0.84 : 0.88) * (1 - face.scatter * 0.85)) }
                }
                PathMultiline { paths: face.browsAndLids }
            }
            ShapePath {
                strokeWidth: -1
                fillGradient: LinearGradient {
                    x1: 50 * face.u; y1: 0; x2: 410 * face.u; y2: 0
                    GradientStop { position: 0.0; color: Pe.alpha(Pe.starLineA, Pe.dark ? 0.36 : 0.42) }
                    GradientStop { position: 1.0; color: Pe.alpha(Pe.starLineB, Pe.dark ? 0.36 : 0.42) }
                }
                PathMultiline { paths: face.lowerLids }
            }
        }

        // the crown, where thoughts link up
        Region {
            u: face.u
            rx: 250; ry: 0; rw: 570; rh: 330
            ShapePath {
                strokeWidth: -1
                fillGradient: LinearGradient {
                    x1: 50 * face.u; y1: 0; x2: 500 * face.u; y2: 0
                    GradientStop { position: 0.0; color: Pe.alpha(Pe.starLineA, Math.min(0.9, face.links) * (1 - face.scatter)) }
                    GradientStop { position: 1.0; color: Pe.alpha(Pe.starLineB, Math.min(0.9, face.links) * (1 - face.scatter)) }
                }
                PathMultiline { paths: face.crownLinks }
            }
        }

        // the mouth: her voice
        Region {
            u: face.u
            rx: 360; ry: 560; rw: 280; rh: 200
            ShapePath {
                strokeWidth: -1
                fillGradient: LinearGradient {
                    x1: (140 - face.mouthHalf) * face.u; y1: 0
                    x2: (140 + face.mouthHalf) * face.u; y2: 0
                    GradientStop { position: 0.0; color: Pe.mix(Pe.mouthA, Pe.alarm, face.warm > 0.9 ? 0.55 : 0) }
                    GradientStop { position: 1.0; color: Pe.mix(Pe.mouthB, Pe.alarm, face.warm > 0.9 ? 0.55 : 0) }
                }
                PathMultiline { paths: face.mouthShapes }
            }
        }

        // stars: contour, crown, brows, bridge, pupils (index order = twinkle order)
        Repeater {
            model: face.contour.length
            delegate: Star {
                u: face.u
                tiny: face.tiny
                required property int index
                cx: face.contour[index][0]
                cy: face.contour[index][1]
                size: index === 5 ? 13 : (index === 0 || index === 10 ? 8 : (index === 1 || index === 9 ? 11 : 10))
                halo: 2.6
                tint: Pe.brandAt(face.contourBase[index][0] / 1000)
                base: 0.78
                sparkle: face.twinkleIdx === index ? face.twinkleAmt : 0
            }
        }
        Repeater {
            model: face.crown.length
            delegate: Star {
                u: face.u
                tiny: face.tiny
                required property int index
                readonly property bool thoughtOnly: face.crownBase[index][4] === 1
                visible: (!face.tiny || index < 5) && (!thoughtOnly || face.think > 0.05)
                cx: face.crown[index][0]
                cy: face.crown[index][1]
                size: 13 * face.crownBase[index][3] * (1 + 0.3 * face.think)
                tint: Pe.brandAt(face.crownBase[index][0] / 1000)
                base: (thoughtOnly ? face.think : 1) * ((index > 4 ? 0.5 : 0.78) + 0.22 * face.think)
                sparkle: face.twinkleIdx === index + 11 ? face.twinkleAmt : 0
            }
        }
        Repeater {
            model: face.tiny ? 0 : 6
            delegate: Star {
                u: face.u
                tiny: face.tiny
                required property int index
                cx: face.browStars[index][0]
                cy: face.browStars[index][1]
                size: index === 1 || index === 4 ? 9 : 7
                halo: 2.6
                tint: Pe.brandAt(face.browStars[index][0] / 1000)
                base: 0.9 * (1 - 0.6 * face.scatter)
                sparkle: face.twinkleIdx === index + 21 ? face.twinkleAmt : 0
            }
        }
        Star {
            u: face.u
            tiny: face.tiny
            visible: !face.tiny
            cx: 500; cy: 560
            size: 8
            tint: Pe.blue
            base: 0.55 * (1 - face.scatter)
            sparkle: face.twinkleIdx === 27 ? face.twinkleAmt : 0
        }
        Star {
            u: face.u
            tiny: face.tiny
            cx: face.eyeL[0] + face.gazeX
            cy: face.eyeL[1] + face.gazeY - Math.max(0, face.eyeOpen) * 0.18
            size: 19 * face.pupil
            halo: 3.4
            tint: Pe.starLineA
            base: face.eyeOpen > 5 ? 1 : 0
        }
        Star {
            u: face.u
            tiny: face.tiny
            cx: face.eyeR[0] + face.gazeX
            cy: face.eyeR[1] + face.gazeY - Math.max(0, face.eyeOpen) * 0.18
            size: 19 * face.pupil
            halo: 3.4
            tint: Pe.starLineB
            base: face.eyeOpen > 5 ? 1 : 0
        }
    }

    // wake flash: a ring of light that blooms out of her
    Shape {
        anchors.fill: parent
        visible: face.burst < 0.999
        opacity: 1 - face.burst
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeWidth: -1
            fillGradient: LinearGradient {
                x1: 0; y1: 0; x2: face.width; y2: face.height
                GradientStop { position: 0.0; color: Pe.starLineA }
                GradientStop { position: 1.0; color: Pe.starLineB }
            }
            PathMultiline {
                paths: {
                    if (face.burst >= 0.999) return []
                    const ring = (r, hw) => {
                        const pts = []
                        for (let i = 0; i <= 72; ++i) {
                            const a = i / 72 * Math.PI * 2
                            pts.push([500 + Math.cos(a) * r, 500 + Math.sin(a) * r])
                        }
                        return face.ribbon(pts, hw, false, 0, 0)
                    }
                    // a sharp ring and an echo just behind it
                    return [ring(160 + face.burst * 330, 1.4 + (1 - face.burst) * 3.2),
                            ring(120 + face.burst * 250, 0.8 + (1 - face.burst) * 1.4)]
                }
            }
        }
    }

    // A Shape that covers only one region of the face (bounds in units).
    component Region: Shape {
        property real u: 1              // pixels per unit (the face's u)
        property real rx: 0
        property real ry: 0
        property real rw: 1000
        property real rh: 1000
        x: rx * u
        y: ry * u
        width: rw * u
        height: rh * u
        preferredRendererType: Shape.CurveRenderer
    }

    // One star: a hot core inside two soft halos.
    component Star: Item {
        id: star
        property real u: 1              // pixels per unit (the face's u)
        property bool tiny: false
        property real cx: 0
        property real cy: 0
        property real size: 12          // core diameter in units
        property real halo: 3.0
        property color tint: Pe.cyan
        property real base: 1
        property real sparkle: 0        // 0..1 extra brightness while it twinkles
        readonly property real core: Math.max(tiny ? 1.6 : 2.2, size * u) * (1 + 0.35 * sparkle)
        readonly property real haloD: Math.max(core * halo, core + 4)
        readonly property real midD: core * 1.9
        readonly property color haloColor: Pe.alpha(tint, Pe.dark ? 0.16 : 0.12)
        readonly property color midColor: Pe.alpha(tint, Pe.dark ? 0.35 : 0.22)
        readonly property color coreColor: Pe.dark ? Pe.mix(Pe.starCore, tint, 0.18) : Pe.mix(Pe.starCore, tint, 0.35)
        x: cx * u
        y: cy * u
        width: 0
        height: 0
        opacity: Math.max(0, Math.min(1, base + 0.35 * sparkle))
        visible: opacity > 0.01
        // GPU: three round rectangles. The software renderer draws rectangles
        // this small with square corners, so there the discs are one Shape.
        Rectangle {
            x: -star.haloD / 2; y: -star.haloD / 2
            width: star.haloD; height: star.haloD; radius: star.haloD / 2
            color: star.haloColor
            visible: !star.tiny && !Pe.software
        }
        Rectangle {
            x: -star.midD / 2; y: -star.midD / 2
            width: star.midD; height: star.midD; radius: star.midD / 2
            color: star.midColor
            visible: !Pe.software
        }
        Rectangle {
            x: -star.core / 2; y: -star.core / 2
            width: star.core; height: star.core; radius: star.core / 2
            color: star.coreColor
            visible: !Pe.software
        }
        Loader {
            active: Pe.software
            sourceComponent: Shape {
                x: -star.haloD / 2
                y: -star.haloD / 2
                width: star.haloD
                height: star.haloD
                ShapePath {
                    strokeWidth: -1
                    fillColor: star.tiny ? "transparent" : star.haloColor
                    PathAngleArc {
                        centerX: star.haloD / 2; centerY: star.haloD / 2
                        radiusX: star.haloD / 2; radiusY: star.haloD / 2
                        startAngle: 0; sweepAngle: 360
                    }
                }
                ShapePath {
                    strokeWidth: -1
                    fillColor: star.midColor
                    PathAngleArc {
                        centerX: star.haloD / 2; centerY: star.haloD / 2
                        radiusX: star.midD / 2; radiusY: star.midD / 2
                        startAngle: 0; sweepAngle: 360
                    }
                }
                ShapePath {
                    strokeWidth: -1
                    fillColor: star.coreColor
                    PathAngleArc {
                        centerX: star.haloD / 2; centerY: star.haloD / 2
                        radiusX: star.core / 2; radiusY: star.core / 2
                        startAngle: 0; sweepAngle: 360
                    }
                }
            }
        }
    }
}
