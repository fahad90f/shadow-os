// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui

// "● Live": a coloured dot and a word, for a panel's title row.
RowLayout {
    id: root

    property string text: ""
    property color tone: Theme.success

    spacing: Theme.spaceXs + 1
    Accessible.role: Accessible.StaticText
    Accessible.name: text

    Rectangle { width: 8; height: 8; radius: 4; color: root.tone }
    Label {
        text: root.text
        color: root.tone
        font.pixelSize: Theme.fontSizeSmall
    }
}
