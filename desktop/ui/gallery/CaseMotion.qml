// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui

// Test page: many gauges and meters whose values change once per second, the
// way a monitoring page drives them. Used to measure what the value
// animations cost (gallery/tests/measure-motion.sh runs it with
// QT_QUICK_BACKEND=software) and to check that the animations are off when
// the scene is rendered in software or "reduce motion" is on.
NxWindow {
    id: window
    title: "NexusUI Gallery — motion"
    width: 1100
    height: 720

    property int tick: 0
    function sample(i) {
        // Deterministic pseudo-load: every gauge moves on every tick.
        return 0.5 + 0.45 * Math.sin(tick * 1.7 + i * 0.9)
    }
    Timer { interval: 1000; running: true; repeat: true; onTriggered: window.tick++ }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spaceXl
        spacing: Theme.spaceLg

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textSecondary
            text: "Scene graph backend: " + (Appearance.softwareRendering ? "software" : "hardware (RHI)")
                  + " · reduce motion: " + (Appearance.reduceMotion ? "on" : "off")
                  + " · value animations: " + (firstGauge.animated ? "on" : "off")
                  + " · tick " + window.tick
        }

        GridLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            columns: 6
            rowSpacing: Theme.spaceLg
            columnSpacing: Theme.spaceLg
            NxGauge {
                id: firstGauge
                Layout.fillWidth: true; Layout.fillHeight: true
                Layout.maximumHeight: width
                value: window.sample(0)
                text: Math.round(value * 100) + "%"
                label: "Core 0"
            }
            Repeater {
                model: 11
                NxGauge {
                    required property int index
                    Layout.fillWidth: true; Layout.fillHeight: true
                    Layout.maximumHeight: width
                    value: window.sample(index + 1)
                    text: Math.round(value * 100) + "%"
                    label: "Core " + (index + 1)
                    color: index % 3 === 0 ? Theme.accentViolet : Theme.accent
                }
            }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: 3
            rowSpacing: Theme.spaceMd
            columnSpacing: Theme.spaceXl
            NxMeter {
                id: firstMeter
                Layout.fillWidth: true
                label: "Volume 0"
                value: window.sample(20)
                valueText: Math.round(value * 100) + " %"
            }
            Repeater {
                model: 11
                NxMeter {
                    required property int index
                    Layout.fillWidth: true
                    label: "Volume " + (index + 1)
                    value: window.sample(index + 21)
                    valueText: Math.round(value * 100) + " %"
                }
            }
        }
    }

    CaseChecks {
        name: "motion"
        interval: 300
        property real before: 0
        steps: [
            () => {
                compare(firstGauge.animated, !Appearance.softwareRendering && !Appearance.reduceMotion,
                        "gauge animates exactly when the scene is hardware-rendered and motion is not reduced")
                compare(firstMeter.animated, firstGauge.animated, "meter follows the same rule")
                before = firstGauge.shown
                window.tick += 3
            },
            () => {
                // 300 ms later: an animated gauge (340 ms) is still on its way, a non-animated one has arrived.
                const arrived = Math.abs(firstGauge.shown - firstGauge.clamped) < 0.0001
                if (firstGauge.animated)
                    verify(!arrived || Theme.motionSlow < 300, "animated gauge is still moving towards the new value")
                else
                    verify(arrived, "non-animated gauge shows the new value at once")
            },
            () => {},
            () => verify(Math.abs(firstGauge.shown - firstGauge.clamped) < 0.0001, "gauge has reached its value")
        ]
    }
}
