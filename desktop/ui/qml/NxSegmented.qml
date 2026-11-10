// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

// Mutually exclusive choice shown as a segmented bar.
//   NxSegmented { model: ["1H", "24H", "7D"]; currentIndex: 0; onActivated: (i) => ... }
Rectangle {
    id: root

    property var model: []
    property int currentIndex: 0

    signal activated(int index)

    implicitWidth: row.implicitWidth + 6
    implicitHeight: Theme.controlHeight
    radius: Theme.radiusSm
    color: Theme.bgInput
    border.width: Theme.borderWidth
    border.color: Theme.border

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 2

        Repeater {
            model: root.model
            Rectangle {
                id: seg
                required property var modelData
                required property int index
                readonly property bool selected: index === root.currentIndex
                width: Math.max(44, label.implicitWidth + Theme.spaceLg * 2)
                height: root.height - 6
                radius: Theme.radiusXs
                color: selected ? Theme.accentFill : hover.hovered ? Theme.hoverOverlay : "transparent"
                activeFocusOnTab: true
                Accessible.role: Accessible.RadioButton
                Accessible.name: String(modelData)
                Accessible.checked: selected
                Keys.onReturnPressed: tap.tapped(null, null)
                Keys.onSpacePressed: tap.tapped(null, null)

                Text {
                    id: label
                    anchors.centerIn: parent
                    text: String(seg.modelData)
                    color: seg.selected ? Theme.accent : Theme.textSecondary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                    font.weight: seg.selected ? Theme.weightSemibold : Theme.weightMedium
                }
                HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
                TapHandler {
                    id: tap
                    onTapped: {
                        if (root.currentIndex !== seg.index) {
                            root.currentIndex = seg.index
                            root.activated(seg.index)
                        }
                    }
                }
            }
        }
    }
}
