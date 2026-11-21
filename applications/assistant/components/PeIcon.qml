// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import org.shadow.assistant

// A glyph from the Shadow symbolic icon set, tinted.
Image {
    id: icon
    property string name: ""
    property color color: Pe.inkSoft
    property real size: 18
    width: size
    height: size
    visible: name !== ""
    source: name !== "" ? "image://nxicon/" + name + "?color=" + encodeURIComponent(Pe.hex(color)) : ""
    sourceSize: Qt.size(Math.round(size * Screen.devicePixelRatio), Math.round(size * Screen.devicePixelRatio))
    smooth: true
    cache: true
    Accessible.ignored: true
}
