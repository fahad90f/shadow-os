// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Templates as T
import org.shadow.assistant

// Round icon-only button; `tip` is also its accessible name.
T.Button {
    id: b
    property string iconName: ""
    property string tip: ""
    property real size: 34
    property real iconSize: 18
    property color tint: Pe.inkSoft
    property color activeTint: Pe.cyan
    property bool quiet: true

    // what a screen reader says (default: the tip)
    property string accessibleName: tip

    implicitWidth: size
    implicitHeight: size
    hoverEnabled: true
    // Tab reaches it; a click leaves the keyboard where it was (e.g. in the
    // prompt: Space typed after clicking Mute must not toggle it again).
    focusPolicy: Qt.TabFocus
    Accessible.name: accessibleName
    Accessible.role: checkable ? Accessible.CheckBox : Accessible.Button
    ToolTip.visible: tip !== "" && hovered
    ToolTip.text: tip
    ToolTip.delay: 450

    background: Rectangle {
        radius: width / 2
        color: b.checked ? Pe.alpha(b.activeTint, 0.14)
             : b.down ? Pe.alpha(Pe.ink, 0.12) : b.hovered ? Pe.alpha(Pe.ink, 0.07) : "transparent"
        border.width: b.quiet ? 0 : 1
        border.color: Pe.line
        Rectangle {
            anchors.fill: parent
            anchors.margins: -3
            radius: width / 2
            color: "transparent"
            border.width: 2
            border.color: Pe.cyan
            visible: b.visualFocus
        }
    }
    contentItem: Item {
        PeIcon {
            anchors.centerIn: parent
            name: b.iconName
            size: b.iconSize
            color: !b.enabled ? Pe.inkFaint : b.checked ? b.activeTint : b.hovered ? Pe.ink : b.tint
        }
    }
}
