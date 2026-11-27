// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.shadow.ui

// The clock of the desktop's start sequence: t runs once from 0 to 1, at
// constant speed, in Theme.motionPage + Theme.motionSlow (0.8 s). Wallpaper,
// panel and dock each own one; they are created in the same moment, so the
// three agree within a frame or two and need nothing from each other.
//
// The clock starts after its window has drawn two frames, so a slow first
// frame cannot swallow the beginning. There is no sequence (t is 1 from the
// start) with "Reduce motion", whose durations are 0, and when an application
// window is already open: then the session is not starting, the shell was
// restarted under running applications.
//
// Whatever takes part reads a window of the sequence in milliseconds:
// lin(120, 460) at constant speed, ph(60, 280) slowing down into place.
Item {
    id: clock

    property real t: 0
    readonly property int total: Theme.motionPage + Theme.motionSlow
    property int ticks: 0

    function lin(from, to) {
        if (t >= 1)
            return 1
        return Math.max(0, Math.min(1, (t * total - from) / (to - from)))
    }
    function ph(from, to) {
        const x = 1 - lin(from, to)
        return 1 - x * x * x
    }
    function finish() {
        frames.stop()
        run.stop()
        t = 1
    }

    FrameAnimation {
        id: frames
        onTriggered: {
            if (++clock.ticks < 2)
                return
            stop()
            run.start()
        }
    }
    NumberAnimation { id: run; target: clock; property: "t"; from: 0; to: 1; duration: clock.total }

    Connections {
        target: windows
        function onOpenWindowsChanged() { if (windows.openWindows && clock.t < 1) clock.finish() }
    }
    Connections {
        target: Theme
        function onReduceMotionChanged() { if (Theme.reduceMotion && clock.t < 1) clock.finish() }
    }
    Component.onCompleted: {
        if (total <= 0 || windows.openWindows)
            t = 1
        else
            frames.start()
    }
}
