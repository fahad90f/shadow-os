// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

// Mutually exclusive choice shown as a segmented bar.
//   NxSegmented { model: ["1H", "24H", "7D"]; currentIndex: 0; onActivated: (i) => ... }
//
// currentIndex can be used in two ways, and a tap respects both:
//   * not bound (a plain value, or none): the selector keeps its own state —
//     a tap sets currentIndex to the tapped segment, then emits activated();
//   * bound to application state (currentIndex: backend.mode): a tap leaves
//     the binding in place and only emits activated(index). The selector
//     shows the tapped segment once the state it is bound to has changed,
//     so a back-end that refuses or delays the change is displayed truthfully.
// activated() is emitted only for a segment other than the current one.
Rectangle {
    id: root

    property var model: []
    property int currentIndex: 0

    signal activated(int index)

    // Select a segment as a tap does (also used by the keyboard handling).
    function select(index) {
        if (index === currentIndex || index < 0)
            return
        // Write through a Binding that is switched on and off again: with
        // RestoreBinding a caller's binding on currentIndex is put back (and
        // re-evaluated) when it is switched off, while a plain value stays
        // at what was written. A direct assignment would destroy the binding.
        tapValue.value = index
        tapValue.when = true
        tapValue.when = false
        activated(index)
    }

    Binding {
        id: tapValue
        target: root
        property: "currentIndex"
        when: false
        restoreMode: Binding.RestoreBinding
    }

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
                Keys.onReturnPressed: root.select(index)
                Keys.onEnterPressed: root.select(index)
                Keys.onSpacePressed: root.select(index)

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
                    onTapped: root.select(seg.index)
                }
            }
        }
    }
}
