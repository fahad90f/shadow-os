// SPDX-License-Identifier: GPL-3.0-or-later
// The clock of an entrance: t runs once from 0 to 1, linear, in "duration"
// milliseconds. Everything that appears reads a window of t, so one value
// drives the whole screen and nothing has a timer of its own.
//
// The clock does not start with the program but after the window has drawn:
// two frame ticks are counted first, so a slow first frame cannot swallow the
// beginning. Should no tick come at all, a timer starts it. With "reduce"
// there is no entrance: t is 1 at once.
//
// The only file of the two themes with a versionless import: FrameAnimation
// does not exist under "import QtQuick 2.15". Loaded behind a Loader, never
// by the form itself.
import QtQuick

Item {
    id: entrance

    property real t: 0
    property int duration: 700
    property bool reduce: false
    property int ticks: 0

    function restart() {
        run.stop()
        frames.stop()
        fallback.stop()
        if (reduce) {
            t = 1
            return
        }
        t = 0
        ticks = 0
        frames.start()
        fallback.restart()
    }
    function begin() {
        frames.stop()
        fallback.stop()
        run.restart()
    }
    onReduceChanged: if (reduce) { run.stop(); frames.stop(); fallback.stop(); t = 1 }

    FrameAnimation {
        id: frames
        onTriggered: if (++entrance.ticks >= 2) entrance.begin()
    }
    Timer {
        id: fallback
        interval: 600
        onTriggered: entrance.begin()
    }
    NumberAnimation {
        id: run
        target: entrance
        property: "t"
        from: 0
        to: 1
        duration: entrance.duration
    }
    Component.onCompleted: restart()
}
