// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui

// A desk panel with a title row: icon, title, and room for a status or a link.
GlowCard {
    id: root

    default property alias body: bodySlot.data
    property string title: ""
    property string icon: ""
    property alias trailing: trailingRow.data

    implicitWidth: 260
    implicitHeight: 160

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.spaceSm

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceSm
            NxIcon {
                visible: root.icon !== ""
                name: root.icon
                size: Theme.iconSmall + 2
                color: Theme.accent
            }
            Label {
                Layout.fillWidth: true
                text: root.title
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSizeBody
                font.weight: Theme.weightSemibold
                elide: Text.ElideRight
            }
            RowLayout {
                id: trailingRow
                spacing: Theme.spaceSm
            }
        }
        Item {
            id: bodySlot
            Layout.fillWidth: true
            Layout.fillHeight: true
        }
    }
}
