// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import org.shadow.ui

// A clickable area in the top panel: icon and/or text, with hover and
// "open" (checked) states.
Item {
    id: root

    property string icon: ""
    property string text: ""
    property bool checked: false
    property color iconColor: Theme.textPrimary
    property string tooltip: ""
    property bool bold: false
    // false: shows its content but does not react (no hover, no click).
    property bool interactive: true
    // A small count shown at the icon's corner (0 hides it).
    property int badge: 0

    signal clicked()

    // A popup this button opens hangs its tether from the button's centre
    // (PopupSurface.originButton). The panel spans its screen from x 0, so
    // the x in the panel window is the x on the screen.
    readonly property var hostScreen: Window.window ? Window.window.screen : null
    function centreInWindow() { return mapToItem(null, width / 2, 0).x }

    implicitWidth: row.implicitWidth + Theme.spaceMd * 2
    implicitHeight: Theme.panelHeight
    activeFocusOnTab: interactive
    Accessible.role: interactive ? Accessible.Button : Accessible.StaticText
    Accessible.name: tooltip !== "" ? tooltip : text
    Keys.onReturnPressed: if (interactive) clicked()
    Keys.onSpacePressed: if (interactive) clicked()

    Rectangle {
        anchors.fill: parent
        anchors.topMargin: 4
        anchors.bottomMargin: 4
        radius: Theme.radiusSm
        color: root.checked ? Theme.accentFill
            : tap.pressed ? Theme.pressedOverlay
            : root.interactive && (hover.hovered || root.activeFocus) ? Theme.hoverOverlay : "transparent"
        Behavior on color { ColorAnimation { duration: Theme.motionFast } }
    }

    Row {
        id: row
        anchors.centerIn: parent
        spacing: Theme.spaceXs + 2
        NxIcon {
            visible: root.icon !== ""
            anchors.verticalCenter: parent.verticalCenter
            name: root.icon
            size: Theme.iconSmall
            color: root.checked ? Theme.accent : root.iconColor
        }
        Text {
            visible: root.text !== ""
            anchors.verticalCenter: parent.verticalCenter
            text: root.text
            color: Theme.textPrimary
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
            font.weight: root.bold ? Theme.weightSemibold : Theme.weightMedium
        }
    }

    Rectangle {
        visible: root.badge > 0
        anchors.right: row.right
        anchors.rightMargin: -7
        anchors.top: row.top
        anchors.topMargin: -5
        width: Math.max(14, badgeText.implicitWidth + 6)
        height: 14
        radius: 7
        color: Theme.accent
        Text {
            id: badgeText
            anchors.centerIn: parent
            text: root.badge > 99 ? "99+" : root.badge
            color: Theme.textOnAccent
            font.family: Theme.fontUi
            font.pixelSize: 9
            font.weight: Theme.weightBold
        }
    }

    HoverHandler {
        id: hover
        cursorShape: root.interactive ? Qt.PointingHandCursor : Qt.ArrowCursor
        onHoveredChanged: if (!hovered) root.pressedSinceHover = false
    }
    TapHandler {
        id: tap
        enabled: root.interactive
        onPressedChanged: if (pressed) root.pressedSinceHover = true
        onTapped: root.clicked()
    }

    // A press hides the tool tip until the pointer has left the button.
    property bool pressedSinceHover: false

    // The panel window is only as high as its buttons, so a tool tip has no
    // room above or below one. It is shown beside the button (towards the
    // middle of the panel) and never takes the pointer: a tool tip on top
    // of the button used to swallow the click it was explaining.
    ToolTip {
        id: tip
        readonly property bool onLeft: hover.hovered
            && root.mapToItem(null, root.width / 2, 0).x > (root.Window.window ? root.Window.window.width / 2 : 0)
        x: onLeft ? -implicitWidth - Theme.spaceXs : root.width + Theme.spaceXs
        y: Math.round((root.height - implicitHeight) / 2)
        margins: 0
        enabled: false
        visible: root.tooltip !== "" && hover.hovered && !root.checked && !tap.pressed && !root.pressedSinceHover
        text: root.tooltip
        delay: 700
        timeout: 5000
    }
}
