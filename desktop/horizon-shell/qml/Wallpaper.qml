// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.shadow.ui

// Desktop background of one screen.
ShellSurface {
    id: root

    layer: "background"
    scope: "shadow-wallpaper"
    anchorTop: true; anchorBottom: true; anchorLeft: true; anchorRight: true
    exclusiveZone: -1
    width: screen ? screen.width : 1280
    height: screen ? screen.height : 800
    visible: true

    readonly property string source: Theme.dark || shellSettings.wallpaperLight === ""
        ? shellSettings.wallpaper : shellSettings.wallpaperLight

    Rectangle {
        anchors.fill: parent
        // Shown while the image loads, and instead of it if it is missing.
        gradient: Gradient {
            GradientStop { position: 0; color: Theme.dark ? "#020306" : "#DCE8F7" }
            GradientStop { position: 1; color: Theme.dark ? "#0A1120" : "#F6F1EC" }
        }
    }

    Image {
        id: image
        anchors.fill: parent
        source: shellSettings.isReadableFile(root.source) ? "file://" + root.source : ""
        asynchronous: true
        cache: false
        smooth: true
        sourceSize: Qt.size(root.width * Screen.devicePixelRatio, root.height * Screen.devicePixelRatio)
        fillMode: shellSettings.wallpaperFill === "fit" ? Image.PreserveAspectFit
            : shellSettings.wallpaperFill === "stretch" ? Image.Stretch
            : shellSettings.wallpaperFill === "center" ? Image.Pad
            : shellSettings.wallpaperFill === "tile" ? Image.Tile : Image.PreserveAspectCrop
        opacity: status === Image.Ready ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: Theme.motionSlow } }
    }
}
