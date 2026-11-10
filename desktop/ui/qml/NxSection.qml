// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts

// A titled group of rows inside one card (the standard settings layout).
//
//   NxSection {
//       title: qsTr("Network")
//       NxRow { title: "Wi-Fi"; Switch { } }
//       NxRow { title: "VPN"; chevron: true; onClicked: ... }
//   }
ColumnLayout {
    id: root

    default property alias content: rows.data
    property string title: ""
    property string description: ""
    property alias headerActions: actions.data

    spacing: Theme.spaceSm
    Layout.fillWidth: true

    RowLayout {
        Layout.fillWidth: true
        Layout.leftMargin: Theme.spaceXs
        Layout.rightMargin: Theme.spaceXs
        visible: root.title !== "" || actions.children.length > 0
        spacing: Theme.spaceSm

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            Text {
                Layout.fillWidth: true
                visible: root.title !== ""
                text: root.title
                color: Theme.textPrimary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBodyLarge
                font.weight: Theme.weightSemibold
                elide: Text.ElideRight
            }
            Text {
                Layout.fillWidth: true
                visible: root.description !== ""
                text: root.description
                color: Theme.textMuted
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WordWrap
            }
        }
        Row {
            id: actions
            spacing: Theme.spaceSm
        }
    }

    Rectangle {
        Layout.fillWidth: true
        implicitHeight: rows.implicitHeight
        color: Theme.bgRaised
        radius: Theme.radiusMd
        border.width: Theme.borderWidth
        border.color: Theme.borderSubtle
        clip: true

        ColumnLayout {
            id: rows
            anchors.left: parent.left
            anchors.right: parent.right
            spacing: 0
        }
    }
}
