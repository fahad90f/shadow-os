// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.nexus.ui

ColumnLayout {
    property alias text: title.text
    property alias detail: detailText.text

    Layout.fillWidth: true
    Layout.leftMargin: Theme.spaceXs
    spacing: 2
    Text {
        id: title
        Layout.fillWidth: true
        color: Theme.textPrimary
        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeBodyLarge
        font.weight: Theme.weightSemibold
        Accessible.role: Accessible.Heading
        Accessible.name: text
    }
    Text {
        id: detailText
        Layout.fillWidth: true
        visible: text !== ""
        color: Theme.textMuted
        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeSmall
        wrapMode: Text.WordWrap
    }
}
