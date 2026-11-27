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
    // "never" | "smart" (hidden while an application window is open) | "always"
    readonly property string hideMode: shellSettings.dockHide
    readonly property bool wantsHidden: hideMode === "always" || (hideMode === "smart" && windows.openWindows)
    // Shown anyway while the pointer is on it, and while something belonging
    // to it (its menu) is open.
    property bool peeking: false
    property bool holdOpen: false
    readonly property bool revealed: !wantsHidden || peeking || holdOpen
    // A layer surface cannot ask where it is; anchored to the bottom edge
    // only, the compositor centres it horizontally on the screen.
    readonly property real screenX: ((screen ? screen.width : width) - width) / 2

    layer: "top"
    scope: "shadow-dock"
    anchorBottom: true
    // Only a dock that never hides reserves space; a hiding one floats over
    // windows when it is shown.
    exclusiveZone: hideMode === "never" ? Math.round(barHeight + bottomGap + 2) : 0
    width: Math.max(160, bar.width + Theme.spaceXl * 2)
    // Hidden, a thin strip stays at the bottom edge: touching it with the
    // pointer brings the dock back.
    height: tall ? Math.round(barHeight + bottomGap + labelSpace) : 4
    visible: true

    // ---- showing and hiding ------------------------------------------------------
    // What is drawn follows "revealed" through "shown" (0..1). Coming back,
    // the window has its full height at once and the bar fades in while the
    // icons slide up 8 px (Theme.motionFast). Leaving, everything fades
    // (Theme.motionInstant) and only then the window shrinks to the strip:
    // "tall" stays true for as long as something is still drawn. With
    // "Reduce motion" both are the instant jumps they always were.
    property real shown: revealed ? 1 : 0
    Behavior on shown {
        enabled: !Theme.reduceMotion
        NumberAnimation {
            duration: root.revealed ? Theme.motionFast : Theme.motionInstant
            easing.type: Easing.BezierSpline
            easing.bezierCurve: root.revealed ? Theme.easingDecelerate : Theme.easingAccelerate
        }
    }
    readonly property bool tall: revealed || shown > 0

    // ---- start of the session ----------------------------------------------------
    // The dock is there and takes input from the first frame. In the first
    // 0.4 s the bar fades in, the row of icons rises 12 px with it and the
    // icons light up from the centre outwards; then four brackets close on
    // the bar and fade. The bar, which carries the blur, only changes its
    // opacity; what moves are its contents.
    StartClock { id: start }
    readonly property real barIn: start.ph(Theme.motionInstant, Theme.motionInstant + Theme.motionNormal)
    // Nothing of the above is on its way any more.
    readonly property bool settled: start.t >= 1 && (shown >= 1 || shown <= 0)
    function iconIn(index) {
        // 28 ms later for every place away from the centre, all done by 0.4 s.
        const from = Math.min(Theme.motionFast + 28 * Math.abs(index - (items.count - 1) / 2), 260)
        return start.ph(from, from + Theme.motionFast)
    }

    HoverHandler {
        id: dockHover
        onHoveredChanged: {
            if (hovered) { hideTimer.stop(); root.peeking = true }
            else hideTimer.restart()
        }
    }
    Timer {
        id: hideTimer
        interval: 900
        onTriggered: if (!dockHover.hovered) root.peeking = false
    }

    // The bar is only the background: it is placed once and then changes
    // nothing but its opacity.
    Rectangle {
        id: bar
        anchors.horizontalCenter: parent.horizontalCenter
        y: root.tall ? root.labelSpace : root.height
        width: row.width + Theme.spaceSm * 2
        height: root.barHeight
        radius: Theme.radiusXl
        color: barBlur.glass ? Theme.glassPopup : Theme.bgOverlay
        BlurBehind { id: barBlur; anchors.fill: parent; radius: bar.radius; surface: root }
        border.width: Theme.borderWidth
        border.color: Theme.border
        visible: root.tall
        opacity: root.barIn * root.shown
    }

    // The icons lie on the bar, not in it, so they can move and fade on
    // their own.
    Row {
        id: row
        anchors.centerIn: bar
        spacing: 2
        visible: root.tall
        transform: Translate { y: 12 * (1 - root.barIn) + (root.revealed ? 8 * (1 - root.shown) : 0) }

        Repeater {
            id: items
            model: dock
            DockItem {
                id: item
                iconSize: root.iconSize
                opacity: (0.2 + 0.8 * root.iconIn(index)) * root.shown
                scale: 0.85 + 0.15 * root.iconIn(index)
                onActivated: dock.activate(index, root)
                onMiddleClicked: dock.launchNew(index, root)
                onMenuRequested: root.menuRequested(index, root.screenX + row.x + item.x + item.width / 2)

                // Tell KWin where this icon is, so the entry's windows
                // minimise into it (magic lamp). Re-sent when the icon
                // moves, its windows change or the dock hides/shows.
                onWindowCountChanged: geometryTimer.restart()
                onXChanged: geometryTimer.restart()
                Component.onCompleted: geometryTimer.restart()
                Connections {
                    target: root
                    function onRevealedChanged() { geometryTimer.restart() }
                    function onWidthChanged() { geometryTimer.restart() }
                    // The icons have reached their places.
                    function onSettledChanged() { if (root.settled) geometryTimer.restart() }
                }
                Timer {
                    id: geometryTimer
                    interval: 120
                    onTriggered: {
                        const p = item.mapToItem(null, 0, 0)
                        dock.setIconGeometry(index, root, Qt.rect(p.x, p.y, item.width, item.height))
                    }
                }

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
            opacity: (0.2 + 0.8 * root.barIn) * root.shown
        }
    }

    // Start of the session, 0.4 s to 0.8 s: the brackets close on the
    // complete dock, hold and fade. They take no input.
    StartBrackets {
        readonly property real closing: start.ph(400, 400 + Theme.motionFast)
        x: bar.x
        y: bar.y
        width: bar.width
        height: bar.height
        spread: 8 - 5 * closing
        opacity: start.t >= 1 ? 0 : closing * (1 - start.lin(600, 800))
        visible: opacity > 0 && root.tall
    }
}
