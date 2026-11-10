// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.nexus.ui

// A popup hanging under the panel: quick settings, the calendar, the
// notification centre. It is an overlay (see OverlaySurface): clicking
// anywhere else or pressing Escape closes it.
//
//   PopupSurface {
//       alignment: "right"; popupWidth: 380
//       Component.onCompleted: shell.registerOverlay("quick-settings", this)
//       ColumnLayout { ... }          // content; its implicitHeight sizes the popup
//   }
OverlaySurface {
    id: root

    // Which part of the panel the popup hangs under: "left" | "center" | "right".
    property string alignment: "right"
    property real popupWidth: 380
    // Upper bound; content taller than this must scroll itself.
    property real maximumHeight: root.height - Theme.panelHeight - Theme.spaceSm * 2
    property real padding: Theme.spaceLg
    default property alias popupContent: body.data
    readonly property alias card: card

    scope: "nexus-popup"

    Rectangle {
        id: card
        x: root.alignment === "left" ? Theme.spaceSm
            : root.alignment === "center" ? Math.round((root.width - width) / 2)
            : root.width - width - Theme.spaceSm
        y: Theme.panelHeight + Theme.spaceSm
        width: Math.min(root.popupWidth, root.width - Theme.spaceSm * 2)
        height: Math.min(root.maximumHeight, body.childrenRect.height + root.padding * 2)
        radius: Theme.radiusLg
        color: cardBlur.glass ? Theme.glassPopup : Theme.bgOverlay
        border.width: Theme.borderWidth
        border.color: Theme.border
        opacity: root.open ? 1 : 0
        transformOrigin: Item.Top
        scale: root.open ? 1 : 0.97
        Behavior on opacity { NumberAnimation { duration: Theme.motionFast } }
        Behavior on scale { NumberAnimation { duration: Theme.motionNormal; easing.type: Easing.OutCubic } }

        BlurBehind { id: cardBlur; anchors.fill: parent; radius: card.radius; surface: root }
        // Clicks inside the popup must not reach the scrim and close it.
        TapHandler { }

        Item {
            id: body
            anchors.fill: parent
            anchors.margins: root.padding
        }
    }
}
