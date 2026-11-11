// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.nexus.ui

// A gauge with two caption lines under it (what is measured, and the figures).
ColumnLayout {
    id: root

    property alias value: gauge.value
    property alias text: gauge.text
    property alias label: gauge.label
    property alias color: gauge.color
    property alias available: gauge.available
    property string caption: ""
    property string detail: ""
    property real gaugeSize: 128

    spacing: Theme.spaceXs

    NxGauge {
        id: gauge
        Layout.alignment: Qt.AlignHCenter
        Layout.preferredWidth: root.gaugeSize
        Layout.preferredHeight: root.gaugeSize
    }
    Text {
        Layout.fillWidth: true
        Layout.topMargin: Theme.spaceXs
        horizontalAlignment: Text.AlignHCenter
        text: root.caption
        color: Theme.textPrimary
        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeSmall
        font.weight: Theme.weightMedium
        elide: Text.ElideRight
    }
    Text {
        Layout.fillWidth: true
        horizontalAlignment: Text.AlignHCenter
        text: root.detail
        color: Theme.textMuted
        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeSmall
        wrapMode: Text.WordWrap
        maximumLineCount: 2
        elide: Text.ElideRight
    }
}
