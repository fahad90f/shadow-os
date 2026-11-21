// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Shapes
import org.shadow.assistant

// Her voice, drawn as a thread of light that leaves her mouth and runs along
// the top of the stage while she speaks, swelling with the real speech level
// and fading as it travels. Only drawn (and only ticking) while she speaks.
Item {
    id: thread
    property real level: 0
    property bool running: false
    property real frozenClock: -1
    property bool animate: true
    property real runClock: 0
    // >= 0: follow this clock (her face's) instead of ticking on its own
    property real sharedClock: -1
    readonly property real clock: frozenClock >= 0 ? frozenClock : sharedClock >= 0 ? sharedClock : runClock
    implicitHeight: 34

    Timer {
        running: thread.running && thread.animate && thread.frozenClock < 0 && thread.sharedClock < 0 && thread.visible
        repeat: true
        interval: 40
        onTriggered: thread.runClock += 0.04
    }

    readonly property var ribbons: {
        const w = width, h = height, mid = h / 2
        if (w < 10 || !running)
            return []
        const n = Math.max(24, Math.min(140, Math.floor(w / 9)))
        const lv = Math.max(0.06, Math.min(1, level))
        const c = clock
        // one smooth strand and its softer echo, same rhythm, slightly behind
        const strand = (amp, phase, hw) => {
            const top = [], bot = []
            for (let i = 0; i <= n; ++i) {
                const t = i / n
                const env = Math.pow(Math.max(0, 1 - t / 0.75), 2.2) * Math.min(1, t * 6)
                const y = mid + amp * lv * env * Math.sin(2 * Math.PI * 3.2 * t - c * 6.5 + phase)
                const ww = hw * (0.3 + 0.7 * (1 - t))
                top.push(Qt.point(t * w, y - ww))
                bot.push(Qt.point(t * w, y + ww))
            }
            bot.reverse()
            return top.concat(bot)
        }
        return [strand(h * 0.44, 0, 0.9), strand(h * 0.3, 0.7, 0.45)]
    }

    Shape {
        anchors.fill: parent
        visible: thread.running
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeWidth: -1
            fillGradient: LinearGradient {
                x1: 0; y1: 0; x2: thread.width; y2: 0
                GradientStop { position: 0.0; color: Pe.mouthA }
                GradientStop { position: 0.3; color: Pe.alpha(Pe.mouthB, 0.85) }
                GradientStop { position: 0.75; color: Pe.alpha(Pe.mouthB, 0.0) }
            }
            PathMultiline { paths: thread.ribbons }
        }
    }
}
