// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui

// Card with a titled history chart: the current figure of each series in the
// header, the chart, and a caption line saying what time span and scale it shows.
NxCard {
    id: root

    property string title: ""
    // [{ label, value, values, color }] — one or two series; 'value' is the current figure as text.
    property var series: []
    property real maxValue: 0           // > 0: fixed scale; otherwise the scale follows the data
    property real minScale: 1
    property int capacity: 120
    property string caption: ""
    property string scaleCaption: ""
    property string unavailableText: "" // non-empty: there is no data source; shown instead of the chart
    property real chartHeight: 92
    readonly property alias peak: chart.peak
    readonly property alias valueScale: chart.valueScale

    implicitHeight: column.implicitHeight + padding * 2

    ColumnLayout {
        id: column
        anchors.fill: parent
        spacing: Theme.spaceSm

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceLg

            Label {
                Layout.fillWidth: true
                text: root.title
                font.weight: Theme.weightSemibold
                elide: Text.ElideRight
            }
            Repeater {
                model: root.unavailableText === "" ? root.series.length : 0
                Row {
                    id: legend
                    required property int index
                    readonly property var entry: root.series[index] ?? ({ label: "", value: "", color: Theme.accent })
                    spacing: Theme.spaceXs + 2
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: root.series.length > 1
                        width: 8
                        height: 8
                        radius: 4
                        color: legend.entry.color
                    }
                    Label {
                        visible: text !== ""
                        text: legend.entry.label
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSizeSmall
                    }
                    Label {
                        text: legend.entry.value
                        font.pixelSize: Theme.fontSizeSmall
                        font.weight: Theme.weightSemibold
                    }
                }
            }
        }

        Item {
            Layout.fillWidth: true
            implicitHeight: root.chartHeight

            SeriesChart {
                id: chart
                anchors.fill: parent
                visible: root.unavailableText === ""
                series: root.unavailableText === "" ? root.series : []
                maxValue: root.maxValue
                minScale: root.minScale
                capacity: root.capacity
            }
            Label {
                anchors.centerIn: parent
                width: parent.width - Theme.spaceXl
                visible: text !== ""
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                color: Theme.textMuted
                font.pixelSize: Theme.fontSizeSmall
                text: root.unavailableText !== "" ? root.unavailableText
                    : chart.sampleCount < 2 ? qsTr("Collecting samples…") : ""
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: root.caption !== "" || root.scaleCaption !== ""
            spacing: Theme.spaceMd
            Label {
                Layout.fillWidth: true
                text: root.caption
                color: Theme.textMuted
                font.pixelSize: Theme.fontSizeCaption
                elide: Text.ElideRight
            }
            Label {
                visible: root.unavailableText === ""
                text: root.scaleCaption
                color: Theme.textMuted
                font.pixelSize: Theme.fontSizeCaption
            }
        }
    }
}
