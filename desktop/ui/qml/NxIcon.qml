// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Window

// A glyph from the SHADOW symbolic icon set, tinted with any colour.
//   NxIcon { name: "wifi"; size: 20; color: Theme.accent }
Item {
    id: root

    property string name: ""
    property real size: Theme.icon
    property color color: Theme.textSecondary

    implicitWidth: size
    implicitHeight: size

    Image {
        anchors.fill: parent
        visible: root.name !== ""
        source: root.name !== ""
            ? "image://nxicon/" + root.name + "?color=" + encodeURIComponent(root.color.toString())
            : ""
        sourceSize: Qt.size(Math.round(root.width * Screen.devicePixelRatio),
                            Math.round(root.height * Screen.devicePixelRatio))
        smooth: true
        cache: true
    }
}
