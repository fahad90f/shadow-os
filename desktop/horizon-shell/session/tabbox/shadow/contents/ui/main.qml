// SPDX-License-Identifier: GPL-3.0-or-later
// The Shadow OS window switcher (Alt+Tab). KWin loads it from
// /usr/share/kwin/tabbox/shadow; kwinrc selects it ([TabBox] LayoutName).
//
// Every window, minimised ones included, as a live preview card. Minimised
// windows are dimmed and marked. The selected card lifts and glows in the
// accent colour; the cards rise in one after the other when the switcher
// opens, without overshoot.
//
// This file runs inside KWin, where the Shadow theme and settings do not
// exist. Its durations are multiples of Kirigami.Units.longDuration (200 ms
// by default), which KWin scales with the AnimationDurationFactor of
// kdeglobals: shadow-apply-appearance sets that to 0 for "Reduce motion".
import QtQuick
import QtQuick.Window
import org.kde.kwin 3.0 as KWin
import org.kde.kirigami 2.20 as Kirigami

KWin.TabBoxSwitcher {
    id: tabBox

    // Shadow design tokens (dark); the accent follows the user's choice
    // through the colour scheme KWin uses.
    readonly property color accent: Kirigami.Theme.highlightColor.a > 0 ? Kirigami.Theme.highlightColor : "#22D8F5"
    readonly property color panelColor: "#F20B0F16"
    readonly property color cardColor: "#111722"
    readonly property color borderColor: "#242D40"
    readonly property color textPrimary: "#E9EEF8"
    readonly property color textMuted: "#6B7892"
    readonly property string fontUi: "Inter Variable"
    // The motion of the switcher, in units of the desktop's animation length.
    readonly property int unit: Kirigami.Units.longDuration
    readonly property int openDuration: Math.round(unit * 1.3)      // 260 ms
    readonly property int stateDuration: Math.round(unit * 0.7)     // 140 ms
    readonly property int staggerStep: Math.round(unit * 0.12)      // 24 ms

    Instantiator {
        active: tabBox.visible
        delegate: Window {
            id: window
            flags: Qt.Popup | Qt.FramelessWindowHint | Qt.X11BypassWindowManagerHint
            color: "transparent"
            x: tabBox.screenGeometry.x
            y: tabBox.screenGeometry.y
            width: tabBox.screenGeometry.width
            height: tabBox.screenGeometry.height
            visible: true

            // Opening: the scrim fades and the panel grows to its size, decelerating.
            property real shown: 0
            Component.onCompleted: shown = 1
            Behavior on shown { NumberAnimation { duration: tabBox.openDuration; easing.type: Easing.OutCubic } }

            Rectangle {
                anchors.fill: parent
                color: "#000000"
                opacity: 0.38 * window.shown
            }

            Item {
                id: panel
                readonly property real cardHeight: Math.min(220, tabBox.screenGeometry.height * 0.24)
                readonly property real cardWidth: cardHeight * 1.45
                readonly property real gap: 18
                readonly property real pad: 28
                anchors.centerIn: parent
                width: Math.min(tabBox.screenGeometry.width * 0.92,
                                Math.max(360, cards.count * (cardWidth + gap) - gap + pad * 2))
                height: header.height + cardHeight + 70 + pad * 2
                scale: 0.88 + 0.12 * window.shown
                opacity: window.shown

                // Soft glow around the glass, fading outwards.
                Repeater {
                    model: 4
                    Rectangle {
                        required property int index
                        anchors.fill: parent
                        anchors.margins: -(index + 1) * 5
                        radius: 26 + (index + 1) * 5
                        color: "transparent"
                        border.width: 5
                        border.color: Qt.rgba(tabBox.accent.r, tabBox.accent.g, tabBox.accent.b, 0.06 - index * 0.013)
                    }
                }
                Rectangle {
                    anchors.fill: parent
                    radius: 26
                    color: tabBox.panelColor
                    border.width: 1
                    border.color: Qt.rgba(tabBox.accent.r, tabBox.accent.g, tabBox.accent.b, 0.35)
                    // Thin light sweep along the top edge.
                    Rectangle {
                        anchors.top: parent.top
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.topMargin: 1
                        width: parent.width * 0.6
                        height: 1
                        gradient: Gradient {
                            orientation: Gradient.Horizontal
                            GradientStop { position: 0; color: "transparent" }
                            GradientStop { position: 0.5; color: Qt.rgba(tabBox.accent.r, tabBox.accent.g, tabBox.accent.b, 0.9) }
                            GradientStop { position: 1; color: "transparent" }
                        }
                    }
                }

                Item {
                    id: header
                    x: panel.pad
                    y: panel.pad - 6
                    width: panel.width - panel.pad * 2
                    height: 24
                    Text {
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        text: "SWITCH WINDOWS"
                        color: tabBox.accent
                        font.family: tabBox.fontUi
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                        font.letterSpacing: 2.4
                    }
                    Text {
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        text: (cards.count === 1 ? "1 window" : cards.count + " windows")
                              + (cards.minimizedCount > 0 ? "  ·  " + cards.minimizedCount + " minimised" : "")
                        color: tabBox.textMuted
                        font.family: tabBox.fontUi
                        font.pixelSize: 12
                    }
                }

                ListView {
                    id: cards
                    property int minimizedCount: 0
                    x: panel.pad
                    y: header.y + header.height + 18
                    width: panel.width - panel.pad * 2
                    height: panel.cardHeight + 60
                    orientation: ListView.Horizontal
                    spacing: panel.gap
                    model: tabBox.model
                    currentIndex: tabBox.currentIndex
                    interactive: false
                    clip: false
                    highlightFollowsCurrentItem: false
                    // Keep the selection in view when there are more windows than fit.
                    preferredHighlightBegin: width / 2 - panel.cardWidth / 2
                    preferredHighlightEnd: width / 2 + panel.cardWidth / 2
                    highlightRangeMode: contentWidth > width ? ListView.ApplyRange : ListView.NoHighlightRange
                    highlightMoveDuration: tabBox.unit
                    onCurrentIndexChanged: tabBox.currentIndex = currentIndex

                    delegate: Item {
                        id: card
                        required property int index
                        required property var model
                        readonly property bool isCurrent: ListView.isCurrentItem
                        readonly property bool isMinimized: model.minimized === true
                        width: panel.cardWidth
                        height: cards.height

                        // Staggered entrance.
                        property real enter: 0
                        Component.onCompleted: {
                            enterAnim.start()
                            if (isMinimized) ++cards.minimizedCount
                        }
                        Component.onDestruction: if (isMinimized) --cards.minimizedCount
                        SequentialAnimation {
                            id: enterAnim
                            PauseAnimation { duration: Math.min(card.index, 8) * tabBox.staggerStep }
                            NumberAnimation { target: card; property: "enter"; to: 1; duration: tabBox.openDuration; easing.type: Easing.OutCubic }
                        }
                        opacity: enter
                        transform: Translate { y: (1 - card.enter) * 26 }

                        MouseArea {
                            id: mouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: tabBox.model.activate(card.index)
                            onEntered: tabBox.currentIndex = card.index
                        }

                        Item {
                            id: body
                            width: parent.width
                            height: panel.cardHeight
                            scale: card.isCurrent ? 1.06 : (mouse.containsMouse ? 1.02 : 1.0)
                            Behavior on scale { NumberAnimation { duration: tabBox.stateDuration; easing.type: Easing.OutCubic } }
                            y: card.isCurrent ? -6 : 0
                            Behavior on y { NumberAnimation { duration: tabBox.stateDuration; easing.type: Easing.OutCubic } }

                            // Accent glow under the selected card.
                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: -10
                                radius: 26
                                color: "transparent"
                                border.width: 10
                                border.color: Qt.rgba(tabBox.accent.r, tabBox.accent.g, tabBox.accent.b, 0.16)
                                opacity: card.isCurrent ? 1 : 0
                                Behavior on opacity { NumberAnimation { duration: tabBox.stateDuration } }
                            }

                            Rectangle {
                                id: frame
                                anchors.fill: parent
                                radius: 16
                                color: tabBox.cardColor
                                border.width: card.isCurrent ? 2 : 1
                                border.color: card.isCurrent ? tabBox.accent : tabBox.borderColor
                                Behavior on border.color { ColorAnimation { duration: tabBox.stateDuration } }
                                clip: true

                                KWin.WindowThumbnail {
                                    anchors.fill: parent
                                    anchors.margins: 8
                                    wId: card.model.windowId
                                    opacity: card.isMinimized ? 0.45 : 1
                                }
                                // Minimised: dimmed preview and a badge.
                                Rectangle {
                                    visible: card.isMinimized
                                    anchors.top: parent.top
                                    anchors.right: parent.right
                                    anchors.margins: 10
                                    width: badgeText.implicitWidth + 18
                                    height: 22
                                    radius: 11
                                    color: "#CC06080C"
                                    border.width: 1
                                    border.color: Qt.rgba(tabBox.accent.r, tabBox.accent.g, tabBox.accent.b, 0.55)
                                    Text {
                                        id: badgeText
                                        anchors.centerIn: parent
                                        text: "▾  Minimised"
                                        color: tabBox.accent
                                        font.family: tabBox.fontUi
                                        font.pixelSize: 11
                                        font.weight: Font.Medium
                                    }
                                }
                                // Close from the switcher.
                                Rectangle {
                                    visible: mouse.containsMouse && card.model.closeable === true
                                    anchors.top: parent.top
                                    anchors.left: parent.left
                                    anchors.margins: 10
                                    width: 24
                                    height: 24
                                    radius: 12
                                    color: closeArea.containsMouse ? "#FF4D6A" : "#CC06080C"
                                    Text {
                                        anchors.centerIn: parent
                                        text: "✕"
                                        color: tabBox.textPrimary
                                        font.pixelSize: 12
                                    }
                                    MouseArea {
                                        id: closeArea
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        onClicked: tabBox.model.close(card.index)
                                    }
                                }
                            }

                            // Application icon, overlapping the bottom edge.
                            Rectangle {
                                anchors.horizontalCenter: parent.horizontalCenter
                                anchors.verticalCenter: parent.bottom
                                width: 46
                                height: 46
                                radius: 14
                                color: "#161D2B"
                                border.width: 1
                                border.color: card.isCurrent ? tabBox.accent : tabBox.borderColor
                                Kirigami.Icon {
                                    anchors.centerIn: parent
                                    width: 32
                                    height: 32
                                    source: card.model.icon
                                }
                            }
                        }

                        Text {
                            anchors.top: body.bottom
                            anchors.topMargin: 30
                            anchors.horizontalCenter: parent.horizontalCenter
                            width: parent.width - 8
                            horizontalAlignment: Text.AlignHCenter
                            elide: Text.ElideRight
                            text: card.model.caption
                            textFormat: Text.PlainText
                            color: card.isCurrent ? tabBox.textPrimary : tabBox.textMuted
                            font.family: tabBox.fontUi
                            font.pixelSize: 13
                            font.weight: card.isCurrent ? Font.DemiBold : Font.Normal
                        }
                    }
                }

                // Nothing open.
                Text {
                    visible: cards.count === 0
                    anchors.centerIn: cards
                    text: "No open windows"
                    color: tabBox.textMuted
                    font.family: tabBox.fontUi
                    font.pixelSize: 14
                }
            }

            // Arrow keys move the selection too (Alt+Tab / Alt+Shift+Tab are KWin's).
            Item {
                focus: true
                Keys.onLeftPressed: cards.decrementCurrentIndex()
                Keys.onRightPressed: cards.incrementCurrentIndex()
            }
        }
    }
}
