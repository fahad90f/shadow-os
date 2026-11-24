// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.shadow.ui

// The panel of the trading desk: a dark surface with a thin luminous edge.
// With 'glow' the edge is brighter and has a soft halo (the panel that
// matters most on the page). The content fills the card, so the card states
// its own size.
Rectangle {
    id: root

    default property alias content: container.data
    property real padding: Theme.spaceMd
    property bool glow: false
    property color tone: Theme.accent

    implicitWidth: 240
    implicitHeight: 120
    radius: Theme.radiusLg
    border.width: Theme.borderWidth
    border.color: Theme.alpha(tone, glow ? 0.8 : 0.3)
    gradient: Gradient {
        GradientStop { position: 0; color: Theme.mix(Theme.bgRaised, root.tone, root.glow ? 0.10 : 0.05) }
        GradientStop { position: 1; color: Theme.bgSurface }
    }

    // The halo: two wider, fainter outlines behind the edge.
    Rectangle {
        visible: root.glow
        anchors.fill: parent
        anchors.margins: -2
        z: -1
        radius: root.radius + 2
        color: "transparent"
        border.width: 2
        border.color: Theme.alpha(root.tone, 0.22)
    }
    Rectangle {
        visible: root.glow
        anchors.fill: parent
        anchors.margins: -5
        z: -2
        radius: root.radius + 5
        color: "transparent"
        border.width: 3
        border.color: Theme.alpha(root.tone, 0.08)
    }

    Item {
        id: container
        anchors.fill: parent
        anchors.margins: root.padding
    }
}
