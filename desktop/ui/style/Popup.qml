// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Templates as T
import org.nexus.ui

T.Popup {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)

    padding: Theme.spaceMd

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.motionFast; easing.type: Easing.OutCubic }
        NumberAnimation { property: "scale"; from: 0.96; to: 1; duration: Theme.motionFast; easing.type: Easing.OutCubic }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.motionFast; easing.type: Easing.InCubic }
    }

    background: Rectangle {
        color: Theme.bgOverlay
        radius: Theme.radiusMd
        border.width: Theme.borderWidth
        border.color: Theme.border

        // Presses that nothing in the popup consumes stop here. Without this,
        // a tap on popup content that only uses pointer handlers with a passive
        // grab (TapHandler's default: NxSegmented, NxRow, table rows …) is also
        // delivered to the pointer handlers of whatever lies beneath the popup.
        TapHandler {
            acceptedButtons: Qt.AllButtons
            gesturePolicy: TapHandler.ReleaseWithinBounds
        }
    }

    // The dimmer of a modal popup takes the exclusive grab of every press that
    // reaches it, so pointer handlers of the content beneath never see a tap —
    // also for popups that replace the style's background.
    T.Overlay.modal: Rectangle {
        color: Theme.scrim
        TapHandler {
            acceptedButtons: Qt.AllButtons
            gesturePolicy: TapHandler.ReleaseWithinBounds
        }
    }
    T.Overlay.modeless: Rectangle { color: "transparent" }
}
