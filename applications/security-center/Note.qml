// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.shadow.ui

// Muted explanatory line under a section (what a value means, why something
// is not shown).
RowLayout {
    id: root
    property string text: ""
    property string icon: "info"
    Layout.fillWidth: true
    Layout.leftMargin: Theme.spaceXs
    Layout.rightMargin: Theme.spaceXs
    spacing: Theme.spaceSm
    NxIcon {
        Layout.alignment: Qt.AlignTop
        Layout.topMargin: 1
        name: root.icon
        size: Theme.iconSmall
        color: Theme.textMuted
    }
    Text {
        Layout.fillWidth: true
        text: root.text
        color: Theme.textMuted
        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeSmall
        wrapMode: Text.WordWrap
    }
}
