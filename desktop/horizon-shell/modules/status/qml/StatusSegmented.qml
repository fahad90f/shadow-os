// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.shadow.ui

// A segmented choice for the quick settings, as wide as it is allowed to be.
// It shows 'currentIndex' and never changes it itself: choosing a segment
// only emits activated(), and the selection moves when the back-end reports
// the new state — so it cannot show a choice that did not take effect.
//
// (NxSegmented is not used here: its taps would also reach the popup's
// "click outside" handling and close the popup.)
Rectangle {
    id: root

    property var model: []
    property int currentIndex: -1

    signal activated(int index)

    implicitHeight: Theme.controlHeight
    radius: Theme.radiusSm
    color: Theme.bgInput
    border.width: Theme.borderWidth
    border.color: Theme.border

    RowLayout {
        anchors.fill: parent
        anchors.margins: 3
        spacing: 2

        Repeater {
            model: root.model
            Rectangle {
                id: segment
                required property var modelData
                required property int index
                readonly property bool selected: index === root.currentIndex

                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: 1
                radius: Theme.radiusXs
                color: selected ? Theme.accentFill : tap.pressed ? Theme.pressedOverlay
                    : hover.hovered || activeFocus ? Theme.hoverOverlay : "transparent"
                border.width: activeFocus ? Theme.focusRing : 0
                border.color: Theme.accent
                activeFocusOnTab: true
                Accessible.role: Accessible.RadioButton
                Accessible.name: String(modelData)
                Accessible.checked: selected
                Accessible.onPressAction: root.activated(index)
                Keys.onReturnPressed: root.activated(index)
                Keys.onSpacePressed: root.activated(index)

                Text {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spaceXs
                    anchors.rightMargin: Theme.spaceXs
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    text: String(segment.modelData)
                    color: segment.selected ? Theme.accent : Theme.textSecondary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                    font.weight: segment.selected ? Theme.weightSemibold : Theme.weightMedium
                    elide: Text.ElideRight
                }
                HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
                TapHandler {
                    id: tap
                    gesturePolicy: TapHandler.ReleaseWithinBounds
                    onTapped: root.activated(segment.index)
                }
            }
        }
    }
}
