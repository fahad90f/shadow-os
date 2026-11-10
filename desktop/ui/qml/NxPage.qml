// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Scrollable content page with a title block and a centred, width-limited
// content column — the layout used by every settings-style screen.
Item {
    id: root

    default property alias content: column.data
    property string title: ""
    property string subtitle: ""
    property alias actions: actionRow.data
    property real maxContentWidth: 880
    property real contentSpacing: Theme.spaceXl
    property alias flickable: flick

    Flickable {
        id: flick
        anchors.fill: parent
        contentWidth: width
        contentHeight: body.implicitHeight + Theme.spaceXl * 2
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        ScrollBar.vertical: ScrollBar { }

        ColumnLayout {
            id: body
            y: Theme.spaceXl
            x: Math.max(Theme.spaceXl, (flick.width - width) / 2)
            width: Math.min(root.maxContentWidth, flick.width - Theme.spaceXl * 2)
            spacing: Theme.spaceXl

            RowLayout {
                Layout.fillWidth: true
                visible: root.title !== "" || actionRow.children.length > 0
                spacing: Theme.spaceMd

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceXs
                    Text {
                        Layout.fillWidth: true
                        text: root.title
                        color: Theme.textPrimary
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeHeading
                        font.weight: Theme.weightSemibold
                        elide: Text.ElideRight
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: root.subtitle !== ""
                        text: root.subtitle
                        color: Theme.textSecondary
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeBody
                        wrapMode: Text.WordWrap
                    }
                }
                Row {
                    id: actionRow
                    spacing: Theme.spaceSm
                    Layout.alignment: Qt.AlignTop
                }
            }

            ColumnLayout {
                id: column
                Layout.fillWidth: true
                spacing: root.contentSpacing
            }
        }
    }
}
