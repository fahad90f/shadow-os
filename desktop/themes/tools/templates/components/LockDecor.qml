// SPDX-License-Identifier: GPL-3.0-or-later
// Everything on the lock screen that is decoration: the scan line of the
// entrance, the ring around the user picture and the brackets around the
// password row. It also owns the clock of the entrance.
//
// LockScreen.qml loads this file through a Loader and reads one value from
// it: t. When the file does not load, the lock screen is complete without
// it and unlocks as before. Nothing here takes input or focus; the nodes on
// the ring are the real time, the comet runs while the password is really
// being checked, the danger colour follows a real refusal.
import QtQuick 2.15
import "../tokens.js" as T
import "Fx.js" as Fx

Item {
    id: decor

    // ---- set by LockScreen.qml -----------------------------------------------------------
    property real u: 1
    property bool reduce: false           // no motion: everything is there at once
    property date now: new Date()
    property bool busy: false             // the password is being checked
    property bool focused: false          // the password field has the keyboard
    property bool caps: false             // Caps Lock is on
    property point ringCenter: Qt.point(width / 2, height / 2)
    property rect fieldRect: Qt.rect(0, 0, 0, 0)

    // ---- read by LockScreen.qml ----------------------------------------------------------
    // Progress of the entrance, 0..1.
    readonly property real t: entrance.t

    // The screen is shown again: the entrance runs again.
    function restart() { entrance.restart() }
    // The password was refused: ring and brackets take the danger colour once
    // and return to their own.
    function fail() { failed.restart() }

    // Milliseconds into the entrance.
    readonly property real ms: entrance.t * T.motionEntranceLock
    property real refused: 0
    readonly property color dangerColor: T.danger
    property color frameColor: caps ? Fx.alpha(warningColor, 0.9) : focused ? Fx.alpha(accentColor, 0.9) : Fx.alpha(slateColor, 0.45)
    readonly property color warningColor: T.warning
    readonly property color accentColor: T.accent
    readonly property color slateColor: T.slate
    Behavior on frameColor {
        enabled: !decor.reduce
        ColorAnimation { duration: T.motionFast }
    }

    Entrance {
        id: entrance
        duration: T.motionEntranceLock
        reduce: decor.reduce
    }
    NumberAnimation { id: failed; target: decor; property: "refused"; from: 1; to: 0; duration: 900 }

    // 0–260 ms: the scan line; LockScreen.qml lets the wallpaper appear above it.
    ScanSweep {
        anchors.fill: parent
        u: decor.u
        progress: decor.reduce ? 1 : Fx.lin(decor.ms, 0, T.motionSweepLock)
    }

    // 90–260 ms: the hexagon, from the moment the scan line reaches the top
    // of the ring; 120–320 ms: tick ring and time nodes.
    HexRing {
        u: decor.u
        x: Math.round(decor.ringCenter.x - width / 2)
        y: Math.round(decor.ringCenter.y - height / 2)
        reveal: Fx.ph(decor.ms, 90, 260)
        ticks: Fx.ph(decor.ms, 120, 320)
        now: decor.now
        busy: decor.busy
        reduce: decor.reduce
        alert: decor.refused
    }

    // 60–200 ms: the brackets arrive from 20u outside and stop 8u around
    // the password row.
    Brackets {
        u: decor.u
        x: decor.fieldRect.x
        y: decor.fieldRect.y
        width: decor.fieldRect.width
        height: decor.fieldRect.height
        visible: width > 0 && opacity > 0
        opacity: Fx.ph(decor.ms, 60, 200)
        spread: (8 + 12 * (1 - Fx.ph(decor.ms, 60, 200))) * decor.u
        color: Fx.mix(decor.frameColor, decor.dangerColor, decor.refused)
    }
}
