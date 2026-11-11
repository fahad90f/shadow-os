// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Window
import org.shadow.ui
import org.shadow.screenshot

// Full-screen surface on one monitor showing the frozen capture. Dragging
// selects the region; releasing the button confirms it, Escape or the right
// mouse button cancels.
Window {
    id: overlay

    required property var screenInfo        // {name, x, y, width, height}

    readonly property real selX: Math.min(drag.startX, drag.endX)
    readonly property real selY: Math.min(drag.startY, drag.endY)
    readonly property real selW: Math.abs(drag.endX - drag.startX)
    readonly property real selH: Math.abs(drag.endY - drag.startY)
    readonly property bool hasSelection: drag.active && selW >= 1 && selH >= 1

    title: qsTr("Select a region")
    width: screenInfo.width
    height: screenInfo.height
    flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    color: Theme.bgBase
    visible: false

    Component.onCompleted: {
        shot.assignScreen(overlay, screenInfo.name)
        showFullScreen()
        requestActivate()
    }

    FrozenView {
        anchors.fill: parent
        screenGeometry: Qt.rect(overlay.screenInfo.x, overlay.screenInfo.y, overlay.screenInfo.width, overlay.screenInfo.height)
    }

    // Dim everything but the selection (four bands around it).
    Rectangle { color: Theme.scrim; x: 0; y: 0; width: parent.width; height: overlay.hasSelection ? overlay.selY : parent.height }
    Rectangle { color: Theme.scrim; visible: overlay.hasSelection; x: 0; y: overlay.selY + overlay.selH; width: parent.width; height: parent.height - y }
    Rectangle { color: Theme.scrim; visible: overlay.hasSelection; x: 0; y: overlay.selY; width: overlay.selX; height: overlay.selH }
    Rectangle { color: Theme.scrim; visible: overlay.hasSelection; x: overlay.selX + overlay.selW; y: overlay.selY; width: parent.width - x; height: overlay.selH }

    Rectangle {
        visible: overlay.hasSelection
        x: overlay.selX - 1
        y: overlay.selY - 1
        width: overlay.selW + 2
        height: overlay.selH + 2
        color: "transparent"
        border.width: 1
        border.color: Theme.accent
    }

    // Size of the selection, next to it.
    Rectangle {
        visible: overlay.hasSelection
        x: Math.min(Math.max(Theme.spaceSm, overlay.selX), overlay.width - width - Theme.spaceSm)
        y: overlay.selY + overlay.selH + Theme.spaceSm + height > overlay.height ? Math.max(Theme.spaceSm, overlay.selY - height - Theme.spaceSm)
                                                                                 : overlay.selY + overlay.selH + Theme.spaceSm
        width: sizeLabel.implicitWidth + Theme.spaceMd * 2
        height: sizeLabel.implicitHeight + Theme.spaceXs * 2
        radius: Theme.radiusSm
        color: Theme.bgOverlay
        border.width: Theme.borderWidth
        border.color: Theme.border
        Text {
            id: sizeLabel
            anchors.centerIn: parent
            text: qsTr("%1 × %2").arg(Math.round(overlay.selW)).arg(Math.round(overlay.selH))
            color: Theme.textPrimary
            font.family: Theme.fontMono
            font.pixelSize: Theme.fontSizeSmall
        }
    }

    Rectangle {
        visible: !drag.active
        anchors.horizontalCenter: parent.horizontalCenter
        y: Theme.spaceXxl
        width: hint.implicitWidth + Theme.spaceXl * 2
        height: hint.implicitHeight + Theme.spaceMd * 2
        radius: Theme.radiusMd
        color: Theme.bgOverlay
        border.width: Theme.borderWidth
        border.color: Theme.border
        Text {
            id: hint
            anchors.centerIn: parent
            text: qsTr("Drag to select the region to capture · Esc cancels")
            color: Theme.textPrimary
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeBodyLarge
        }
    }

    Item {
        id: keys
        anchors.fill: parent
        focus: true
        Accessible.name: qsTr("Region selection. Drag with the mouse; Escape cancels.")
        Keys.onEscapePressed: shot.cancelRegion()
    }

    MouseArea {
        id: drag
        property bool active: false
        property real startX: 0
        property real startY: 0
        property real endX: 0
        property real endY: 0
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        cursorShape: Qt.CrossCursor
        onPressed: (mouse) => {
            if (mouse.button === Qt.RightButton) { shot.cancelRegion(); return }
            active = true
            startX = endX = mouse.x
            startY = endY = mouse.y
        }
        onPositionChanged: (mouse) => {
            if (!active) return
            endX = Math.min(Math.max(0, mouse.x), width)
            endY = Math.min(Math.max(0, mouse.y), height)
        }
        onReleased: (mouse) => {
            if (!active) return
            active = false
            // A click without a drag is not a selection; keep waiting.
            if (overlay.selW < 4 || overlay.selH < 4) return
            shot.finishRegion(overlay.screenInfo.x + overlay.selX, overlay.screenInfo.y + overlay.selY, overlay.selW, overlay.selH)
        }
    }
}
