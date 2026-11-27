// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Templates as T
import org.shadow.ui

T.Dialog {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding,
                            implicitHeaderWidth, implicitFooterWidth)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding
                             + (implicitHeaderHeight > 0 ? implicitHeaderHeight + spacing : 0)
                             + (implicitFooterHeight > 0 ? implicitFooterHeight + spacing : 0))

    padding: Theme.spaceXl
    topPadding: Theme.spaceSm
    anchors.centerIn: T.Overlay.overlay
    modal: true

    // Opening: the dialog fades in while a line grows along its top edge;
    // title, content and buttons are in place from the first frame. Nothing
    // moves: the style cannot move the content a dialog is given, and a
    // background that moved alone would no longer frame it. Closing: a
    // short fade.
    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.motionFast; easing.type: Easing.OutCubic }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.motionInstant }
    }

    background: Rectangle {
        color: Theme.bgOverlay
        radius: Theme.radiusLg
        border.width: Theme.borderWidth
        border.color: Theme.border

        // The trace of this popup: a line in the accent colour that grows
        // along the top edge while it opens.
        Rectangle {
            id: trace
            readonly property real grown: control.opacity
            x: parent.radius
            width: (parent.width - parent.radius * 2) * grown
            height: 1
            color: Theme.alpha(Theme.accent, 0.7)
        }

        // Presses that nothing in the popup consumes stop here. Without this,
        // a tap on popup content that only uses pointer handlers with a passive
        // grab (TapHandler's default: NxSegmented, NxRow, table rows …) is also
        // delivered to the pointer handlers of whatever lies beneath the popup.
        TapHandler {
            acceptedButtons: Qt.AllButtons
            gesturePolicy: TapHandler.ReleaseWithinBounds
        }
    }

    header: Label {
        text: control.title
        visible: control.title
        elide: Label.ElideRight
        font.pixelSize: Theme.fontSizeTitle
        font.weight: Theme.weightSemibold
        leftPadding: Theme.spaceXl
        rightPadding: Theme.spaceXl
        topPadding: Theme.spaceXl
        bottomPadding: Theme.spaceXs
    }

    footer: DialogButtonBox {
        visible: count > 0
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
