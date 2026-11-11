// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.nexus.ui

// Icon of a profile: the themed icon named by the manifest, or — when the icon
// theme does not have it — the glyph of the profile's main category.
Item {
    id: root

    property var profile: ({})
    property real size: 48

    implicitWidth: size
    implicitHeight: size

    NxAppIcon {
        anchors.fill: parent
        visible: (root.profile.icon || "") !== ""
        name: root.profile.icon || ""
        size: root.size
    }
    Rectangle {
        anchors.fill: parent
        visible: (root.profile.icon || "") === ""
        radius: Theme.radiusMd
        color: Theme.alpha(Theme.accent, 0.14)
        NxIcon {
            anchors.centerIn: parent
            name: root.profile.glyph || "shield"
            size: Math.round(root.size * 0.5)
            color: Theme.accent
        }
    }
}
