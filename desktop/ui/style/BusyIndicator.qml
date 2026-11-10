// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Shapes
import QtQuick.Templates as T
import org.nexus.ui

T.BusyIndicator {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)
    padding: Theme.spaceXs

    contentItem: Item {
        implicitWidth: 28
        implicitHeight: 28
        opacity: control.running ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: Theme.motionNormal } }

        Shape {
            id: ring
            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                strokeWidth: Math.max(2, ring.width / 10)
                strokeColor: Theme.accent
                fillColor: "transparent"
                capStyle: ShapePath.RoundCap
                PathAngleArc {
                    centerX: ring.width / 2
                    centerY: ring.height / 2
                    radiusX: ring.width / 2 - 2
                    radiusY: ring.height / 2 - 2
                    startAngle: 0
                    sweepAngle: 270
                }
            }
            RotationAnimator on rotation {
                running: control.running && control.visible
                loops: Animation.Infinite
                from: 0
                to: 360
                duration: 900
            }
        }
    }
}
