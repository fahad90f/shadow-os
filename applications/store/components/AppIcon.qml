// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.nexus.ui
import org.nexus.store

// Icon of an application: a local file or icon-theme icon when there is one,
// otherwise the icon from the catalogue's media server, fetched lazily. While
// nothing can be shown (not downloaded yet, offline, no icon in the metadata)
// a neutral generic glyph is drawn — never a made-up picture.
Item {
    id: root

    // "image://nxtheme/…" for local icons, an https URL for remote ones, or "".
    property string source: ""
    property bool remote: false
    property real size: 48

    implicitWidth: size
    implicitHeight: size

    RemoteImage {
        id: fetch
        source: root.remote ? root.source : ""
        active: root.visible && root.remote
    }

    Rectangle {
        anchors.fill: parent
        visible: picture.status !== Image.Ready
        radius: Theme.radiusMd
        color: Theme.bgHover
        NxIcon {
            anchors.centerIn: parent
            name: "app-generic"
            size: root.size * 0.55
            color: Theme.textMuted
        }
    }

    Image {
        id: picture
        anchors.fill: parent
        source: root.remote ? (fetch.status === "ready" ? fetch.localSource : "") : root.source
        sourceSize: Qt.size(Math.round(root.size * 2), Math.round(root.size * 2))
        fillMode: Image.PreserveAspectFit
        asynchronous: true
        smooth: true
        visible: status === Image.Ready
    }
}
