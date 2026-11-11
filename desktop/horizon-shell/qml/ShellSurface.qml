// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Window
import org.kde.layershell as LayerShell

// A window placed by the compositor as a wlr-layer-shell surface: anchored
// to screen edges, on a layer below or above application windows, optionally
// reserving space (exclusive zone).
//
// Outside a Wayland compositor (the headless UI smoke test) the layer-shell
// attached properties are inert and this is an ordinary frameless window.
Window {
    id: root

    // "background" | "bottom" | "top" | "overlay"
    property string layer: "top"
    property bool anchorTop: false
    property bool anchorBottom: false
    property bool anchorLeft: false
    property bool anchorRight: false
    // > 0 reserves that many pixels at the anchored edge; 0 respects other
    // surfaces' reservations; -1 ignores them (covers the whole output).
    property int exclusiveZone: 0
    // "none" | "exclusive" | "ondemand"
    property string keyboard: "none"
    property string scope: "shadow-shell"

    flags: Qt.FramelessWindowHint
    color: "transparent"

    LayerShell.Window.scope: root.scope
    LayerShell.Window.layer: root.layer === "background" ? LayerShell.Window.LayerBackground
        : root.layer === "bottom" ? LayerShell.Window.LayerBottom
        : root.layer === "overlay" ? LayerShell.Window.LayerOverlay : LayerShell.Window.LayerTop
    LayerShell.Window.anchors: (root.anchorTop ? LayerShell.Window.AnchorTop : 0)
        | (root.anchorBottom ? LayerShell.Window.AnchorBottom : 0)
        | (root.anchorLeft ? LayerShell.Window.AnchorLeft : 0)
        | (root.anchorRight ? LayerShell.Window.AnchorRight : 0)
    LayerShell.Window.exclusionZone: root.exclusiveZone
    LayerShell.Window.keyboardInteractivity: root.keyboard === "exclusive" ? LayerShell.Window.KeyboardInteractivityExclusive
        : root.keyboard === "ondemand" ? LayerShell.Window.KeyboardInteractivityOnDemand
        : LayerShell.Window.KeyboardInteractivityNone
}
