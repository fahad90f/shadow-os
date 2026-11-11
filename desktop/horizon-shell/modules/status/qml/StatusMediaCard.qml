// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.shadow.ui

// What the current media player (MPRIS) plays, with its transport controls.
Rectangle {
    id: root

    readonly property var media: status.media

    implicitHeight: row.implicitHeight + Theme.spaceMd * 2
    radius: Theme.radiusMd
    color: Theme.hoverOverlay
    border.width: Theme.borderWidth
    border.color: Theme.borderSubtle

    RowLayout {
        id: row
        anchors.fill: parent
        anchors.margins: Theme.spaceMd
        spacing: Theme.spaceMd

        Rectangle {
            implicitWidth: Theme.rowHeight
            implicitHeight: Theme.rowHeight
            radius: Theme.radiusSm
            color: Theme.bgPressed
            clip: true
            NxIcon {
                anchors.centerIn: parent
                visible: art.status !== Image.Ready
                name: "music"
                color: Theme.textSecondary
            }
            Image {
                id: art
                anchors.fill: parent
                // Only local files; see MediaBackend.
                source: root.media.artUrl
                sourceSize: Qt.size(Theme.rowHeight * 2, Theme.rowHeight * 2)
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                visible: status === Image.Ready
            }
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0
            Text {
                Layout.fillWidth: true
                text: root.media.title !== "" ? root.media.title : root.media.playerName
                color: Theme.textPrimary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
                font.weight: Theme.weightSemibold
                elide: Text.ElideRight
            }
            Text {
                Layout.fillWidth: true
                text: {
                    const parts = []
                    if (root.media.artist !== "") parts.push(root.media.artist)
                    if (root.media.title !== "") parts.push(root.media.playerName)
                    else if (root.media.playbackStatus === "Stopped") parts.push(qsTr("Stopped"))
                    return parts.join("  ·  ")
                }
                visible: text !== ""
                color: Theme.textSecondary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeCaption
                elide: Text.ElideRight
            }
        }
        RowLayout {
            spacing: 0
            NxIconButton {
                iconName: "skip-back"
                tooltip: qsTr("Previous track")
                enabled: root.media.canGoPrevious
                onClicked: root.media.previous()
            }
            NxIconButton {
                iconName: root.media.playing ? "pause" : "play"
                tooltip: root.media.playing ? qsTr("Pause") : qsTr("Play")
                enabled: root.media.canPlayPause
                onClicked: root.media.playPause()
            }
            NxIconButton {
                iconName: "skip-forward"
                tooltip: qsTr("Next track")
                enabled: root.media.canGoNext
                onClicked: root.media.next()
            }
        }
    }
}
