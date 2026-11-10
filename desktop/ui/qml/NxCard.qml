// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

// Raised surface that groups related content.
Rectangle {
    id: root

    default property alias content: container.data
    property real padding: Theme.spaceLg
    property bool hoverable: false
    property bool accentBorder: false
    readonly property bool hovered: hover.hovered

    implicitWidth: container.childrenRect.width + padding * 2
    implicitHeight: container.childrenRect.height + padding * 2

    color: Theme.bgRaised
    radius: Theme.radiusMd
    border.width: Theme.borderWidth
    border.color: accentBorder ? Theme.alpha(Theme.accent, 0.55)
        : hoverable && hovered ? Theme.borderStrong : Theme.borderSubtle
    Behavior on border.color { ColorAnimation { duration: Theme.motionFast } }

    HoverHandler { id: hover; enabled: root.hoverable }

    Item {
        id: container
        anchors.fill: parent
        anchors.margins: root.padding
    }
}
