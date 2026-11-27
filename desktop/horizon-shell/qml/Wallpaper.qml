// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.shadow.ui

// Desktop background of one screen.
//
// When the session starts, a scan line resolves the picture from the top
// (0.46 s): the picture sits in a clip whose lower edge is the line. A
// picture that is decoded only after the line has started fades in when it
// arrives (behind the line, if the line is still on its way), as every
// later change of the wallpaper does.
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

    StartClock { id: start }

    Rectangle {
        anchors.fill: parent
        // Shown while the image loads, and instead of it if it is missing.
        gradient: Gradient {
            GradientStop { position: 0; color: Theme.dark ? "#020306" : "#DCE8F7" }
            GradientStop { position: 1; color: Theme.dark ? "#0A1120" : "#F6F1EC" }
        }
    }

    Item {
        width: root.width
        // Nothing of the picture before the sequence begins, then down to
        // the line; all of it once the line has passed, and at once when
        // there is no sequence.
        height: Math.round(root.height * start.lin(0, Theme.motionPage))
        clip: true

        Image {
            id: image
            width: root.width
            height: root.height
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
            Behavior on opacity {
                // A picture that is ready before the line starts waits for
                // the line at full strength.
                enabled: start.t > 0
                NumberAnimation { duration: Theme.motionSlow }
            }
        }
    }

    StartScan {
        anchors.fill: parent
        progress: start.lin(0, Theme.motionPage)
    }
}
