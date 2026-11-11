// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.shadow.ui

// The application dock: pinned launchers and running applications.
ShellSurface {
    id: root

    // Asks the shell to show the context menu for a dock entry;
    // centerX is the entry's centre in screen coordinates.
    signal menuRequested(int index, real centerX)

    readonly property real iconSize: shellSettings.dockIconSize
    readonly property real barHeight: iconSize + Theme.spaceSm * 2 + 6 + Theme.spaceXs * 2
    readonly property real labelSpace: 34
    readonly property real bottomGap: Theme.spaceSm
    readonly property bool autoHide: shellSettings.dockAutoHide
    property bool revealed: !autoHide
    // A layer surface cannot ask where it is; anchored to the bottom edge
    // only, the compositor centres it horizontally on the screen.
    readonly property real screenX: ((screen ? screen.width : width) - width) / 2

    layer: "top"
    scope: "shadow-dock"
    anchorBottom: true
    exclusiveZone: autoHide ? 0 : Math.round(barHeight + bottomGap + 2)
    width: Math.max(160, bar.width + Theme.spaceXl * 2)
    height: revealed ? Math.round(barHeight + bottomGap + labelSpace) : 6
    visible: true

    onAutoHideChanged: revealed = !autoHide

    HoverHandler {
        id: dockHover
        onHoveredChanged: {
            if (!root.autoHide) return
            if (hovered) { hideTimer.stop(); root.revealed = true }
            else hideTimer.restart()
        }
    }
    Timer {
        id: hideTimer
        interval: 900
        onTriggered: if (root.autoHide && !dockHover.hovered) root.revealed = false
    }

    Rectangle {
        id: bar
        anchors.horizontalCenter: parent.horizontalCenter
        y: root.revealed ? root.labelSpace : root.height
        width: row.width + Theme.spaceSm * 2
        height: root.barHeight
        radius: Theme.radiusXl
        color: barBlur.glass ? Theme.glassPopup : Theme.bgOverlay
        BlurBehind { id: barBlur; anchors.fill: parent; radius: bar.radius; surface: root }
        border.width: Theme.borderWidth
        border.color: Theme.border
        visible: root.revealed

        Row {
            id: row
            anchors.centerIn: parent
            spacing: 2

            Repeater {
                id: items
                model: dock
                DockItem {
                    id: item
                    iconSize: root.iconSize
                    onActivated: dock.activate(index, root)
                    onMiddleClicked: dock.launchNew(index, root)
                    onMenuRequested: root.menuRequested(index, root.screenX + bar.x + row.x + item.x + item.width / 2)

                    // Name label above the hovered entry.
                    Rectangle {
                        visible: item.hovered
                        anchors.horizontalCenter: parent.horizontalCenter
                        y: -root.labelSpace + 2
                        width: label.implicitWidth + Theme.spaceMd * 2
                        height: 26
                        radius: Theme.radiusSm
                        color: Theme.bgOverlay
                        border.width: Theme.borderWidth
                        border.color: Theme.borderStrong
                        Text {
                            id: label
                            anchors.centerIn: parent
                            text: item.name
                            color: Theme.textPrimary
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeSmall
                            font.weight: Theme.weightMedium
                        }
                    }
                }
            }

            // Shown when nothing is pinned and nothing is running.
            Text {
                visible: items.count === 0
                height: root.iconSize + Theme.spaceSm * 2
                verticalAlignment: Text.AlignVCenter
                leftPadding: Theme.spaceMd
                rightPadding: Theme.spaceMd
                text: qsTr("No applications pinned")
                color: Theme.textMuted
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
            }
        }
    }
}
