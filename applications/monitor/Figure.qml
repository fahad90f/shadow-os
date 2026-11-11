// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.nexus.ui

// A small right-aligned figure with a caption above it, for the trailing
// area of a row ("Read / 1.2 MiB/s"). The minimum width keeps a column of
// rows aligned while the numbers change.
Column {
    id: root

    property string label: ""
    property string value: ""
    property color valueColor: Theme.textPrimary
    property real minimumWidth: 88
    property bool rightAligned: true

    spacing: 1
    width: Math.max(minimumWidth, labelText.implicitWidth, valueText.implicitWidth)
    Accessible.role: Accessible.StaticText
    Accessible.name: label + " " + value

    Text {
        id: labelText
        anchors.right: root.rightAligned ? parent.right : undefined
        text: root.label
        color: Theme.textMuted
        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeCaption
    }
    Text {
        id: valueText
        anchors.right: root.rightAligned ? parent.right : undefined
        text: root.value
        color: root.valueColor
        font.family: Theme.fontMono
        font.pixelSize: Theme.fontSizeSmall
    }
}
