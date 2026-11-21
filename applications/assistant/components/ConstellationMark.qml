// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Shapes
import org.shadow.assistant

// Every conversation gets its own tiny constellation, derived from its id:
// four stars and three links. Static (drawn once), so it costs nothing.
Item {
    id: mark
    property string seed: ""
    property bool lit: false
    implicitWidth: 22
    implicitHeight: 22
    Accessible.ignored: true

    readonly property var stars: {
        let h = 2166136261
        for (let i = 0; i < seed.length; ++i) {
            h ^= seed.charCodeAt(i)
            h = Math.imul(h, 16777619) >>> 0
        }
        const pts = []
        for (let k = 0; k < 4; ++k) {
            h = Math.imul(h ^ (h >>> 13), 1274126177) >>> 0
            const x = 3 + k * (width - 6) / 3 + ((h >>> 20) % 3) - 1
            const y = 3 + ((h >>> 4) % 1000) / 1000 * (height - 6)
            pts.push(Qt.point(x, y))
        }
        return pts
    }

    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeColor: mark.lit ? Pe.alpha(Pe.her, 0.8) : Pe.alpha(Pe.inkMute, 0.55)
            strokeWidth: 1
            fillColor: "transparent"
            PathPolyline { path: mark.stars }
        }
    }
    Repeater {
        model: 4
        delegate: Rectangle {
            required property int index
            readonly property real d: index === 1 ? 4.5 : 3.2
            x: mark.stars[index].x - d / 2
            y: mark.stars[index].y - d / 2
            width: d; height: d; radius: d / 2
            color: mark.lit ? Pe.brandAt(index / 3) : Pe.inkSoft
        }
    }
}
