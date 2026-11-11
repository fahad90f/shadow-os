// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls

// Push button with explicit visual intent.
//   kind: "primary" | "secondary" | "ghost" | "danger"
//   iconName: glyph from the SHADOW symbolic set shown before the label
Button {
    id: root

    property string kind: "secondary"
    property string iconName: ""
    property bool busy: false

    highlighted: kind === "primary"
    flat: kind === "ghost"
    icon.source: iconName !== "" && !busy ? "image://nxicon/" + iconName : ""
    enabled: !busy
    leftPadding: busy ? Theme.spaceLg + 20 : Theme.spaceLg

    BusyIndicator {
        anchors.left: parent.left
        anchors.leftMargin: Theme.spaceSm
        anchors.verticalCenter: parent.verticalCenter
        width: 20
        height: 20
        padding: 0
        running: root.busy
        visible: root.busy
    }
}
