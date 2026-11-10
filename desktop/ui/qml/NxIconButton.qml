// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls

// Square icon-only button with an obligatory tooltip (accessibility name).
ToolButton {
    id: root

    property string iconName: ""
    property string tooltip: ""
    property real iconSize: Theme.icon

    icon.source: iconName !== "" ? "image://nxicon/" + iconName : ""
    icon.width: iconSize
    icon.height: iconSize
    display: AbstractButton.IconOnly
    Accessible.name: tooltip
    hoverEnabled: true

    ToolTip.visible: tooltip !== "" && hovered
    ToolTip.text: tooltip
}
