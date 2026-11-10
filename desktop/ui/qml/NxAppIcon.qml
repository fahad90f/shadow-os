// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Window

// An application / file-type icon resolved through the freedesktop icon theme.
//   NxAppIcon { name: "org.nexus.files"; size: 48 }
Image {
    id: root

    property string name: ""
    property real size: 48

    width: size
    height: size
    source: name !== "" ? "image://nxtheme/" + encodeURIComponent(name) : ""
    sourceSize: Qt.size(Math.round(size * Screen.devicePixelRatio), Math.round(size * Screen.devicePixelRatio))
    smooth: true
    asynchronous: true
    fillMode: Image.PreserveAspectFit
}
