// SPDX-License-Identifier: GPL-3.0-or-later
// The scan line: a bright line with a fading tail above it that crosses the
// item once from top to bottom. What lies above the line has been resolved.
// progress 0 = top edge, 1 = bottom edge; outside 0..1 nothing is drawn.
import QtQuick 2.15
import "../tokens.js" as T

Item {
    id: sweep

    property real u: 1
    property real progress: 0
    readonly property color glow: T.accent

    visible: progress > 0 && progress < 1

    Item {
        y: Math.round(sweep.progress * sweep.height)
        width: sweep.width

        Rectangle {
            y: -height
            width: parent.width
            height: Math.round(40 * sweep.u)
            gradient: Gradient {
                GradientStop { position: 0.0; color: Qt.rgba(sweep.glow.r, sweep.glow.g, sweep.glow.b, 0.0) }
                GradientStop { position: 1.0; color: Qt.rgba(sweep.glow.r, sweep.glow.g, sweep.glow.b, 0.30) }
            }
        }
        Rectangle {
            width: parent.width
            height: Math.max(2, Math.round(2 * sweep.u))
            color: T.hot
        }
    }
}
