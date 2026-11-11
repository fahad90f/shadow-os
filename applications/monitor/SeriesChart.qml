// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.shadow.ui

// One or two history series drawn over the same time and value axes.
//   SeriesChart { series: [{ values: monitor.networkRxHistory, color: Theme.accent }, ...] }
// With maxValue <= 0 the vertical scale follows the largest sample of all
// series (never below minScale), so the series stay comparable.
Item {
    id: root

    property var series: []             // [{ values: [...], color: <color> }], oldest sample first
    property real maxValue: 0
    property real minScale: 1
    property int capacity: 120
    property int gridLines: 3

    readonly property real peak: {
        var m = 0
        for (var i = 0; i < series.length; i++) {
            var v = series[i].values
            for (var j = 0; j < v.length; j++) if (v[j] > m) m = v[j]
        }
        return m
    }
    readonly property real valueScale: maxValue > 0 ? maxValue : Math.max(minScale, peak * 1.15)
    readonly property int sampleCount: series.length > 0 ? series[0].values.length : 0

    implicitWidth: 240
    implicitHeight: 64

    Repeater {
        model: root.series.length
        NxSparkline {
            required property int index
            readonly property var entry: root.series[index] ?? ({ values: [], color: Theme.accent })
            anchors.fill: parent
            values: entry.values
            color: entry.color
            maxValue: root.valueScale
            capacity: root.capacity
            fill: index === 0
            gridLines: index === 0 ? root.gridLines : 0
        }
    }
}
