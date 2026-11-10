// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.nexus.ui

// Full-screen layer above everything, for the launcher, menus and dialogs.
// Clicking outside the content (on the scrim) or pressing Escape dismisses
// it, unless 'modal' is set (authentication prompts).
ShellSurface {
    id: root

    default property alias content: host.data
    property real scrimOpacity: 0.0
    property bool modal: false
    property bool open: false
    // Set by Shell.registerOverlay(): the name this overlay can be opened by.
    property string overlayName: ""

    signal dismissed()

    function show() { open = true }
    function hide() { open = false }

    layer: "overlay"
    anchorTop: true; anchorBottom: true; anchorLeft: true; anchorRight: true
    exclusiveZone: -1
    keyboard: "exclusive"
    width: screen ? screen.width : 1280
    height: screen ? screen.height : 800
    visible: open

    onVisibleChanged: if (visible) { requestActivate(); host.forceActiveFocus() }

    Rectangle {
        anchors.fill: parent
        color: "#000000"
        opacity: root.open ? root.scrimOpacity : 0
        Behavior on opacity { NumberAnimation { duration: Theme.motionNormal } }
    }

    TapHandler {
        acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
        onTapped: if (!root.modal) { root.hide(); root.dismissed() }
    }

    FocusScope {
        id: host
        anchors.fill: parent
        focus: true
        Keys.onEscapePressed: if (!root.modal) { root.hide(); root.dismissed() } else event.accepted = false
    }
}
