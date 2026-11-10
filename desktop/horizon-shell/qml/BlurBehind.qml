// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.nexus.ui

// Makes the compositor blur whatever is behind this item's rectangle, so a
// translucent "glass" background stays legible.
//
//   Rectangle {
//       color: blur.glass ? Theme.glassPopup : Theme.bgOverlay
//       BlurBehind { id: blur; anchors.fill: parent; radius: parent.radius; surface: root }
//   }
//
// 'glass' is true only when the user has transparency switched on AND the
// compositor can actually blur; backgrounds must be opaque otherwise.
Item {
    id: root

    // The window (shell surface) this item lives in.
    property var surface: null
    property real radius: 0
    readonly property bool glass: Appearance.transparency && effects.blurAvailable

    function update() {
        if (!surface) return
        const origin = root.mapToItem(null, 0, 0)
        effects.setBlurBehind(surface, glass && surface.visible && root.visible, origin.x, origin.y,
                              root.width, root.height, root.radius)
    }

    onGlassChanged: Qt.callLater(update)
    onWidthChanged: Qt.callLater(update)
    onHeightChanged: Qt.callLater(update)
    onRadiusChanged: Qt.callLater(update)
    onVisibleChanged: Qt.callLater(update)
    Component.onCompleted: Qt.callLater(update)

    Connections {
        target: root.surface
        function onVisibleChanged() { Qt.callLater(root.update) }
        function onWidthChanged() { Qt.callLater(root.update) }
        function onHeightChanged() { Qt.callLater(root.update) }
    }
}
