// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.store

// One AppStream screenshot. The picture is on the catalogue's media server
// and is only requested while the page is shown. Until it is there — or if it
// cannot be fetched — a placeholder says exactly that; no stand-in image is
// ever shown.
Rectangle {
    id: root

    // { thumbnail, source, caption, thumbnailWidth, thumbnailHeight }
    property var shot: ({})
    property bool active: false
    // Fetch the full-size picture instead of the thumbnail.
    property bool fullSize: false

    signal clicked()

    readonly property bool ready: fetch.status === "ready"

    radius: Theme.radiusMd
    color: Theme.bgInput
    border.width: Theme.borderWidth
    border.color: tapper.enabled && hover.hovered ? Theme.borderStrong : Theme.borderSubtle
    clip: true
    activeFocusOnTab: ready
    Accessible.role: ready ? Accessible.Button : Accessible.Graphic
    Accessible.name: (shot.caption || "") !== "" ? qsTr("Screenshot: %1").arg(shot.caption) : qsTr("Screenshot")
    Keys.onReturnPressed: if (ready) clicked()
    Keys.onSpacePressed: if (ready) clicked()

    RemoteImage {
        id: fetch
        source: root.fullSize ? (root.shot.source || "") : (root.shot.thumbnail || "")
        active: root.active
    }

    HoverHandler { id: hover; enabled: tapper.enabled; cursorShape: Qt.PointingHandCursor }
    TapHandler { id: tapper; enabled: root.ready && !root.fullSize; onTapped: root.clicked() }

    Image {
        anchors.fill: parent
        anchors.margins: 1
        visible: root.ready
        source: root.ready ? fetch.localSource : ""
        fillMode: Image.PreserveAspectFit
        asynchronous: true
        smooth: true
    }

    ColumnLayout {
        anchors.centerIn: parent
        width: parent.width - Theme.spaceXl * 2
        visible: !root.ready
        spacing: Theme.spaceSm

        BusyIndicator {
            Layout.alignment: Qt.AlignHCenter
            visible: fetch.status === "loading"
            running: visible
        }
        NxIcon {
            Layout.alignment: Qt.AlignHCenter
            visible: fetch.status !== "loading"
            name: "image"
            size: Theme.iconXL
            color: fetch.status === "error" ? Theme.warning : Theme.textMuted
        }
        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: fetch.status === "loading" ? qsTr("Loading the screenshot…")
                : fetch.status === "error" ? qsTr("Screenshot not available")
                : qsTr("Screenshot not loaded")
            color: Theme.textSecondary
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeBody
            font.weight: Theme.weightMedium
            wrapMode: Text.WordWrap
        }
        PkText {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            visible: fetch.status === "error"
            text: qsTr("It is stored on the catalogue's server and could not be downloaded (offline?): %1").arg(fetch.errorString)
            color: Theme.textMuted
            font.pixelSize: Theme.fontSizeSmall
            wrapMode: Text.WordWrap
            maximumLineCount: 4
            elide: Text.ElideRight
        }
        NxButton {
            Layout.alignment: Qt.AlignHCenter
            visible: fetch.status === "error"
            text: qsTr("Try again")
            iconName: "refresh"
            onClicked: fetch.retry()
        }
    }
}
