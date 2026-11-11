// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.installer

// What the engine reported during this run (its events and its stderr).
Rectangle {
    id: root

    Layout.fillWidth: true
    Layout.fillHeight: true
    color: Theme.bgInput
    radius: Theme.radiusMd
    border.width: Theme.borderWidth
    border.color: Theme.borderSubtle
    clip: true

    Flickable {
        id: flick
        anchors.fill: parent
        anchors.margins: Theme.spaceSm
        contentWidth: width
        contentHeight: log.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        ScrollBar.vertical: ScrollBar { }
        property bool follow: true
        onMovementEnded: follow = atYEnd
        onContentHeightChanged: if (follow) contentY = Math.max(0, contentHeight - height)

        TextEdit {
            id: log
            width: flick.width - Theme.spaceMd
            readOnly: true
            selectByMouse: true
            wrapMode: TextEdit.Wrap
            text: Installer.logText
            color: Theme.textSecondary
            selectionColor: Theme.accentFillStrong
            font.family: Theme.fontMono
            font.pixelSize: Theme.fontSizeSmall
            activeFocusOnTab: true
            Accessible.name: qsTr("Messages of the installation engine")
        }
    }
}
