// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.shadow.ui
import "Fx.js" as Fx

// Full-screen layer above everything, for the launcher, menus and dialogs.
// Clicking outside the content (on the scrim) or pressing Escape dismisses
// it, unless 'modal' is set (authentication prompts).
//
// Opening and closing are drawn from ONE clock, 'reveal': it runs linearly
// from 0 to 1 while the overlay opens (revealDuration) and back to 0 while
// it closes (exitDuration). Everything a surface animates is a binding on a
// window of that clock, ph(a, b), so opening, closing, opening again in
// mid-flight and "Reduce motion" (both durations are 0: the clock jumps)
// can never disagree. The rules for what may be bound to it:
//
//   - Layout never depends on it. Content has its final geometry and the
//     keyboard focus from the first frame; only opacity and a few pixels of
//     Translate on the inside of a row change.
//   - A card that carries BlurBehind is never moved, scaled or clipped.
//   - When the clock has reached 1 nothing moves any more; 'settle' then
//     runs once from 0 to 1 for a change of colour (Theme.motionSettle).
//
// Closing does not play the opening backwards: most windows of the clock
// lie early in it, so run back they would change nothing until the last
// frame. The picture is held as it was when the exit began ('stage' stays
// there) and fades as a whole, linearly, with 'fade'.
//
// While it closes the surface stays mapped, and a mapped overlay keeps the
// keyboard under KWin whatever it asks for. So exits are short (100 ms, 80 ms
// for small menus), the content ignores keys while closing, the pointer
// passes through, and an overlay that makes room for another one closes
// with closeNow().
ShellSurface {
    id: root

    default property alias content: host.data
    property real scrimOpacity: 0.0
    property bool modal: false
    property bool open: false
    // Set by Shell.registerOverlay(): the name this overlay can be opened by.
    property string overlayName: ""

    // The clock, 0 (not there) .. 1 (complete). Linear; see above.
    property real reveal: 0
    property int revealDuration: Theme.motionReveal
    property int exitDuration: Theme.motionExit
    // false: closing unmaps the surface at once (authentication prompts).
    property bool animatedExit: true
    // Open is false but the exit is still being drawn.
    readonly property bool closing: !open && reveal > 0
    // Where the picture stands: the clock while opening; while closing, the
    // point the exit began at (held until the surface is gone).
    property real exitFrom: 0
    readonly property real stage: open ? reveal : exitFrom
    // 1 while open; while closing it falls linearly to 0 with the clock.
    readonly property real fade: open ? 1 : exitFrom > 0 ? Math.min(1, reveal / exitFrom) : 0
    // false: the surface fades its content itself (the launcher keeps the
    // tile of a started application lit while the rest goes).
    property bool fadeContent: true
    // 0 .. 1 once after every completed reveal: colours settle on it.
    property real settle: 0
    // The plain variant for the software renderer: no soft strokes, no
    // beam tail, fewer staggered rows.
    readonly property bool lite: Appearance.softwareRendering

    signal dismissed()

    function show() { open = true }
    function hide() { open = false }
    // Closes without an exit: the surface is unmapped in the same tick.
    function closeNow() {
        open = false
        revealAnimation.stop()
        reveal = 0
    }

    // How far the picture is through the window [a, b] of the clock:
    // decelerating (for things that arrive) and linear (for beams).
    function ph(a, b) { return Fx.ph(stage, a, b) }
    function lin(a, b) { return Fx.lin(stage, a, b) }
    // The same for a part that has a clock of its own (a submenu that opens
    // while its overlay is already complete).
    function phOf(clock, a, b) { return Fx.ph(clock, a, b) }
    // The window of a row that a beam travelling through [beamA, beamB]
    // reaches at 'fraction' of its way; it takes 'span' and ends by 'latest'.
    function rowPhase(beamA, beamB, fraction, span, latest) {
        return Fx.rowPhase(stage, beamA, beamB, fraction, span, latest)
    }

    layer: "overlay"
    anchorTop: true; anchorBottom: true; anchorLeft: true; anchorRight: true
    exclusiveZone: -1
    // What the surface asks of the compositor follows 'open', in a fixed
    // order (see onOpenChanged): the keyboard and the pointer first, the
    // mapping last. An open overlay has the keyboard to itself; a closing
    // one asks for neither keyboard nor pointer, so a click goes to
    // whatever is underneath.
    keyboard: "none"
    flags: Qt.FramelessWindowHint | Qt.WindowTransparentForInput
    property bool mapped: false
    width: screen ? screen.width : 1280
    height: screen ? screen.height : 800
    visible: mapped || reveal > 0

    NumberAnimation { id: revealAnimation; target: root; property: "reveal" }
    NumberAnimation { id: settleAnimation; target: root; property: "settle"; from: 0; to: 1; duration: Theme.motionSettle }

    onOpenChanged: {
        // Before anything else looks for the keyboard focus (the handlers of
        // the surfaces built on this one run after this one).
        host.enabled = open
        // Keyboard and pointer are asked for BEFORE the surface is mapped:
        // the first keys typed after the shortcut must not wait for a
        // second round with the compositor.
        keyboard = open ? "exclusive" : "none"
        flags = open ? Qt.FramelessWindowHint : (Qt.FramelessWindowHint | Qt.WindowTransparentForInput)
        mapped = open
        revealAnimation.stop()
        if (!open) exitFrom = reveal
        // From wherever the clock is: opening again during an exit, or
        // closing during a reveal, continues without a jump.
        const duration = open ? revealDuration * (1 - reveal) : (animatedExit ? exitDuration * reveal : 0)
        if (duration <= 0) {
            reveal = open ? 1 : 0
        } else {
            revealAnimation.from = reveal
            revealAnimation.to = open ? 1 : 0
            revealAnimation.duration = duration
            revealAnimation.start()
        }
        // Opened again while the exit was still showing: the surface never
        // went away, so nothing else gives it the keyboard back.
        if (open && visible) { requestActivate(); host.forceActiveFocus() }
    }
    onRevealChanged: {
        if (reveal >= 1 && open) settleAnimation.restart()
        else if (reveal <= 0) { settleAnimation.stop(); settle = 0 }
    }

    onVisibleChanged: if (visible) { requestActivate(); host.forceActiveFocus() }

    Rectangle {
        anchors.fill: parent
        color: "#000000"
        opacity: root.scrimOpacity * root.ph(0, 0.5) * root.fade
    }

    TapHandler {
        acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
        // Only a tap on the scrim dismisses: a tap that landed on content is
        // the content's business, even if the control there did not grab it.
        onTapped: (eventPoint) => {
            if (root.modal || !root.open) return
            if (host.childAt(eventPoint.position.x, eventPoint.position.y)) return
            root.hide(); root.dismissed()
        }
    }

    FocusScope {
        id: host
        anchors.fill: parent
        focus: true
        opacity: root.fadeContent ? root.fade : 1
        // A closing overlay still has the keyboard; its content must not act
        // on it. Follows 'open' (see onOpenChanged).
        enabled: false
        Keys.onEscapePressed: if (!root.modal) { root.hide(); root.dismissed() } else event.accepted = false
    }
}
