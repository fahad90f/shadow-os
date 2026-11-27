// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.shadow.ui

// A popup hanging under the panel: quick settings, the calendar, the
// notification centre. It is an overlay (see OverlaySurface): clicking
// anywhere else or pressing Escape closes it.
//
//   PopupSurface {
//       alignment: "right"; popupWidth: 380
//       Component.onCompleted: shell.registerOverlay("quick-settings", this)
//       ColumnLayout { ... }          // content; its implicitHeight sizes the popup
//   }
//
// How it opens (windows of the overlay's 'reveal' clock, 220 ms in all): a
// short line ties the panel to the card, the outline is traced from there
// round the card, the fill fades in and a scan line runs from the top edge to
// the bottom. Content is in place from the first frame; a row that binds its
// opacity to rowAppear(row) shows up when the scan line reaches it.
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

    // The scan line's window of the clock, and how long a row takes.
    readonly property real beamStart: 0.2
    readonly property real beamEnd: 0.85
    readonly property real rowSpan: 0.3
    // The panel button that opened the popup (set by Shell.present() from
    // the buttons registered with Shell.registerOverlayButton()); null when
    // there is none on this screen.
    property var originButton: null
    // Where on the panel edge the popup hangs from, in the card's x: under
    // the centre of that button, else a point given by the alignment. Taken
    // each time the popup opens.
    readonly property real defaultOriginX: alignment === "left" ? Math.min(card.width / 2, Theme.spaceXxxl)
        : alignment === "center" ? card.width / 2
        : card.width - Math.min(card.width / 2, Theme.spaceXxxl)
    property real originX: defaultOriginX
    onOpenChanged: {
        if (!open) return
        const button = originButton
        // Kept on the straight part of the card's top edge.
        originX = button ? Math.max(Theme.radiusLg, Math.min(card.width - Theme.radiusLg, button.centreInWindow() - card.x))
                         : defaultOriginX
    }

    // 0..1 for a row of the content: it starts when the scan line reaches
    // its upper edge. For 'opacity', and for a few pixels of Translate on
    // what is INSIDE the row (never on the row: its place and the area that
    // takes clicks do not move).
    function rowAppear(item) {
        // Reads the clock first, so a binding follows it.
        const clock = root.stage
        if (clock >= 1 || !item) return 1
        return rowAppearAt(item.mapToItem(card, 0, 0).y)
    }
    function rowAppearAt(yInCard) {
        return root.rowPhase(beamStart, beamEnd, yInCard / Math.max(1, card.height), rowSpan, 1)
    }

    scope: "shadow-popup"
    revealDuration: Theme.motionRevealPopup

    FxTether {
        x: card.x + root.originX
        y: Theme.panelHeight
        direction: "down"
        length: card.y - Theme.panelHeight
        progress: root.ph(0, 0.2)
        color: Theme.mix(Theme.accent, Theme.borderStrong, root.settle)
    }

    // The card never moves and is never scaled or clipped: its blurred
    // background would not follow. Only the opacity of its fill changes.
    Item {
        id: card
        x: root.alignment === "left" ? Theme.spaceSm
            : root.alignment === "center" ? Math.round((root.width - width) / 2)
            : root.width - width - Theme.spaceSm
        y: Theme.panelHeight + Theme.spaceSm
        width: Math.min(root.popupWidth, root.width - Theme.spaceSm * 2)
        height: Math.min(root.maximumHeight, body.childrenRect.height + root.padding * 2)

        Rectangle {
            anchors.fill: parent
            radius: Theme.radiusLg
            color: cardBlur.glass ? Theme.glassPopup : Theme.bgOverlay
            border.width: Theme.borderWidth
            border.color: Theme.border
            opacity: root.ph(0.1, 0.5)
        }
        // The blur switches on when the fill is mostly there.
        BlurBehind { id: cardBlur; anchors.fill: parent; radius: Theme.radiusLg; surface: root; visible: root.reveal >= 0.3 }
        // Clicks inside the popup must not reach the scrim and close it.
        TapHandler { }

        Item {
            id: body
            anchors.fill: parent
            anchors.margins: root.padding
        }

        FxBeam {
            anchors.fill: parent
            inset: Theme.radiusLg
            direction: "down"
            progress: root.open ? root.lin(root.beamStart, root.beamEnd) : 0
        }
        FxFrame {
            anchors.fill: parent
            radius: Theme.radiusLg
            origin: "top"
            originPosition: root.originX
            progress: root.ph(0.1, 0.7)
            color: Theme.mix(Theme.accent, Theme.border, root.settle)
            glow: 1 - root.settle
        }
    }
}
