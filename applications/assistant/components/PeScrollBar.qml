// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Templates as T
import org.shadow.assistant

// A quiet scroll bar without animations: it shows while you scroll or point
// at it and hides at once. (A fading bar restarts its animation whenever the
// content grows, which keeps the window rendering frames while she writes.)
T.ScrollBar {
    id: bar
    // shown faintly even at rest (e.g. a code line that runs past the edge)
    property bool hint: false
    implicitWidth: 10
    implicitHeight: 10
    padding: 2
    minimumSize: orientation === Qt.Horizontal ? height / Math.max(1, width) : width / Math.max(1, height)
    visible: size < 1.0 && policy !== T.ScrollBar.AlwaysOff
    contentItem: Rectangle {
        implicitWidth: bar.interactive ? 6 : 4
        implicitHeight: bar.interactive ? 6 : 4
        radius: width / 2
        color: bar.pressed ? Pe.inkSoft : Pe.inkMute
        opacity: bar.pressed || bar.hovered ? 0.85 : bar.active ? 0.5 : bar.hint ? 0.3 : 0.0
    }
}
