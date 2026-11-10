// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import org.nexus.ui

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

    signal clicked()

    implicitWidth: row.implicitWidth + Theme.spaceMd * 2
    implicitHeight: Theme.panelHeight
    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: tooltip !== "" ? tooltip : text
    Keys.onReturnPressed: clicked()
    Keys.onSpacePressed: clicked()

    Rectangle {
        anchors.fill: parent
        anchors.topMargin: 4
        anchors.bottomMargin: 4
        radius: Theme.radiusSm
        color: root.checked ? Theme.accentFill
            : tap.pressed ? Theme.pressedOverlay
            : hover.hovered || root.activeFocus ? Theme.hoverOverlay : "transparent"
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

    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler { id: tap; onTapped: root.clicked() }

    ToolTip.visible: tooltip !== "" && hover.hovered && !checked
    ToolTip.text: tooltip
    ToolTip.delay: 700
}
