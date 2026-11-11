// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui

// Read-only monospaced output of a running tool. Follows the end of the text
// while it grows, unless the user has scrolled away from it.
Rectangle {
    id: root

    property string text: ""
    property string placeholder: ""

    implicitWidth: 400
    implicitHeight: 220
    radius: Theme.radiusSm
    color: Theme.bgInput
    border.width: Theme.borderWidth
    border.color: Theme.border
    clip: true

    function scrollToEnd() {
        flick.contentY = Math.max(0, flick.contentHeight - flick.height)
    }

    Flickable {
        id: flick
        anchors.fill: parent
        anchors.margins: Theme.spaceSm
        contentWidth: Math.max(width, edit.implicitWidth)
        contentHeight: edit.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        // "At the end" before the text grows decides whether to keep following.
        property bool pinned: true
        onMovementEnded: pinned = contentY >= contentHeight - height - 4
        ScrollBar.vertical: ScrollBar { }
        ScrollBar.horizontal: ScrollBar { }

        TextEdit {
            id: edit
            text: root.text
            readOnly: true
            selectByMouse: true
            wrapMode: TextEdit.NoWrap
            color: Theme.textPrimary
            selectionColor: Theme.accentFillStrong
            selectedTextColor: Theme.textPrimary
            font.family: Theme.fontMono
            font.pixelSize: Theme.fontSizeSmall
            Accessible.role: Accessible.StaticText
            Accessible.name: qsTr("Tool output")
            onTextChanged: {
                if (text === "")
                    flick.pinned = true
                if (flick.pinned)
                    Qt.callLater(root.scrollToEnd)
            }
        }
    }

    Label {
        anchors.centerIn: parent
        visible: root.text === "" && root.placeholder !== ""
        text: root.placeholder
        color: Theme.textMuted
    }
}
