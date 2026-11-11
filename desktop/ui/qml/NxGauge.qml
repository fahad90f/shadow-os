// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Shapes

// Circular arc gauge for a single 0..1 measurement.
//   NxGauge { value: cpu.usage; text: Math.round(cpu.usage * 100) + "%"; label: "CPU" }
Item {
    id: root

    property real value: 0              // 0..1; values outside are clamped
    property string text: ""
    property string label: ""
    property real thickness: Math.max(6, width / 14)
    property color color: Theme.accent
    property color trackColor: Theme.bgPressed
    property real startAngle: 135       // degrees, clockwise from 3 o'clock
    property real sweepAngle: 270
    property bool available: true       // false → greyed out, shows "n/a"

    readonly property real clamped: Math.max(0, Math.min(1, value))
    property real shown: available ? clamped : 0
    // The arc glides to a new value — unless "reduce motion" is on or the
    // scene is rendered in software, where every frame of 12 gauges moving
    // once a second is CPU time (see README.md, "Value animations").
    readonly property bool animated: Appearance.animateValues
    Behavior on shown {
        enabled: root.animated
        NumberAnimation { duration: Theme.motionSlow; easing.type: Easing.OutCubic }
    }

    implicitWidth: 140
    implicitHeight: 140
    Accessible.role: Accessible.ProgressBar
    Accessible.name: label + " " + (available ? text : qsTr("not available"))

    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            strokeWidth: root.thickness
            strokeColor: root.trackColor
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            PathAngleArc {
                centerX: root.width / 2
                centerY: root.height / 2
                radiusX: root.width / 2 - root.thickness / 2 - 1
                radiusY: root.height / 2 - root.thickness / 2 - 1
                startAngle: root.startAngle
                sweepAngle: root.sweepAngle
            }
        }
        ShapePath {
            strokeWidth: root.thickness
            strokeColor: root.shown > 0.001 ? root.color : "transparent"
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            PathAngleArc {
                centerX: root.width / 2
                centerY: root.height / 2
                radiusX: root.width / 2 - root.thickness / 2 - 1
                radiusY: root.height / 2 - root.thickness / 2 - 1
                startAngle: root.startAngle
                sweepAngle: Math.max(0.01, root.sweepAngle * root.shown)
            }
        }
    }

    Column {
        anchors.centerIn: parent
        spacing: 0
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: root.available ? root.text : qsTr("n/a")
            color: root.available ? Theme.textPrimary : Theme.textDisabled
            font.family: Theme.fontUi
            font.pixelSize: Math.max(Theme.fontSizeTitle, root.width / 5.2)
            font.weight: Theme.weightSemibold
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            visible: root.label !== ""
            text: root.label
            color: Theme.textMuted
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeCaption
            font.weight: Theme.weightMedium
            font.capitalization: Font.AllUppercase
            font.letterSpacing: 0.8
        }
    }
}
