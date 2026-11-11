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

    HoverHandler { id: hover; cursorShape: root.interactive ? Qt.PointingHandCursor : Qt.ArrowCursor }
    TapHandler { id: tap; enabled: root.interactive; onTapped: root.clicked() }

    ToolTip.visible: tooltip !== "" && hover.hovered && !checked && !tap.pressed
    ToolTip.timeout: 5000
    ToolTip.text: tooltip
    ToolTip.delay: 700
}
