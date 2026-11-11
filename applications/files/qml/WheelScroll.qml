// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.nexus.ui

// Mouse-wheel / touchpad scrolling for a Flickable whose `interactive` is off.
//
// The file views are not flicked with the mouse: a press-and-drag on them
// selects (rubber band) or starts a drag and drop, as on every desktop file
// manager. Scrolling is done here instead, in short eased steps.
WheelHandler {
    id: handler

    required property Flickable flickable
    // Distance of one wheel notch.
    property real step: Theme.rowHeight * 2

    // Ctrl + wheel: +1 zoom in, -1 zoom out.
    signal zoomRequested(int direction)

    property real goal: 0

    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad

    onWheel: (event) => {
        if (event.modifiers & Qt.ControlModifier) {
            if (event.angleDelta.y !== 0)
                handler.zoomRequested(event.angleDelta.y > 0 ? 1 : -1)
            return
        }
        const f = handler.flickable
        const top = f.originY
        const bottom = Math.max(top, top + f.contentHeight - f.height)
        const precise = event.pixelDelta.y !== 0
        const dy = precise ? event.pixelDelta.y : event.angleDelta.y / 120 * handler.step
        const from = handler.glide.running ? handler.goal : f.contentY
        handler.goal = Math.max(top, Math.min(bottom, from - dy))
        if (precise) {
            handler.glide.stop()
            f.contentY = handler.goal
        } else {
            handler.glide.to = handler.goal
            handler.glide.restart()
        }
    }

    readonly property NumberAnimation glide: NumberAnimation {
        target: handler.flickable
        property: "contentY"
        duration: Theme.motionFast
        easing.type: Easing.OutCubic
    }
}
