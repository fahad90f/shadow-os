// SPDX-License-Identifier: GPL-3.0-or-later
// List to choose one entry from (users, sessions, keyboard layouts).
// Keyboard: Up/Down/Home/End/PageUp/PageDown move, Return/Enter/Space
// choose, Escape or Tab close; focus returns to the item that opened it.
//
// Opening: the list unfolds from the item that opened it (160 ms) and its
// rows appear one after the other. Only what is drawn changes: the rows
// exist, have the focus and take keys from the first frame. Closing is a
// 100 ms fade; the focus is back at the opener at once. With "reduce" both
// happen in one frame.
import QtQuick 2.15
import "../tokens.js" as T

FocusScope {
    id: popup

    property real u: 1
    property var entries: []          // [{ title, subtitle, picture, avatar }]
    property int currentIndex: -1
    property Item opener: null
    property bool open: false
    property bool reduce: false
    property real maximumHeight: 400
    property string title: ""
    readonly property real rowHeight: Math.round((popup.hasSubtitles ? 52 : T.rowHeight) * u)
    readonly property real padding: Math.round(T.spaceSm * u)
    readonly property real headerHeight: title !== "" ? Math.round(36 * u) : 0
    property bool hasSubtitles: false
    // On screen: open, or fading out after it was closed. Whoever fills
    // "entries" keeps them for that long.
    readonly property bool shown: open || fadeOut.running
    signal chosen(int index)

    // Milliseconds since the list was opened, up to "settled": the clock of
    // the unfolding. The list unfolds in the first 160 ms; row n (counted
    // from the opener, at most 8 steps) starts 20 ms after row n - 1.
    readonly property real settled: T.motionList + 7 * 20 + 100
    property real openMs: settled
    // Height in the list at which the opener is: where the unfolding starts.
    property real originY: 0
    readonly property real unfolded: {
        var x = 1 - Math.min(1, openMs / T.motionList)
        return 1 - x * x * x
    }
    function rowIn(step) {
        var x = 1 - Math.max(0, Math.min(1, (openMs - 20 * step) / 100))
        return 1 - x * x * x
    }
    property real fade: 1

    visible: shown
    opacity: fade
    width: Math.round(300 * u)
    height: Math.min(maximumHeight, entries.length * rowHeight + 2 * padding + headerHeight)

    function show() {
        fadeOut.stop()
        fade = 1
        open = true
        list.currentIndex = Math.max(0, Math.min(entries.length - 1, currentIndex))
        list.positionViewAtIndex(list.currentIndex, ListView.Contain)
        list.forceActiveFocus()
        originY = opener ? Math.max(0, Math.min(height, opener.mapToItem(popup, 0, opener.height / 2).y)) : 0
        if (reduce)
            openMs = settled
        else
            unfold.restart()
    }
    function hide() {
        if (!open)
            return
        unfold.stop()
        openMs = settled
        // The fade starts first: "shown" then never drops in between.
        if (!reduce)
            fadeOut.restart()
        open = false
        if (opener)
            opener.forceActiveFocus()
    }
    function choose(index) {
        if (index < 0 || index >= entries.length)
            return
        hide()
        chosen(index)
    }

    NumberAnimation { id: unfold; target: popup; property: "openMs"; from: 0; to: popup.settled; duration: popup.settled }
    NumberAnimation {
        id: fadeOut
        target: popup; property: "fade"; from: 1; to: 0; duration: T.motionListExit
        easing.type: Easing.InCubic
    }

    // The window through which the list is seen while it unfolds: it grows
    // from the opener's height to the whole list. The list behind it does
    // not move and has its final size from the start.
    Item {
        id: slit
        readonly property real upper: Math.round(popup.originY * (1 - popup.unfolded))
        readonly property real lower: Math.round(popup.originY + (popup.height - popup.originY) * popup.unfolded)
        y: upper
        width: popup.width
        height: lower - upper
        clip: true

        Item {
            id: body
            y: -slit.upper
            width: popup.width
            height: popup.height

            Rectangle {
                anchors.fill: parent
                radius: Math.round(T.radiusLg * popup.u)
                color: T.bgRaised
                border.width: 1
                border.color: T.border
            }
            // Swallow clicks on the padding so they do not close the list.
            MouseArea { anchors.fill: parent; enabled: popup.open }

            Text {
                visible: popup.title !== ""
                x: popup.padding + Math.round(T.spaceMd * popup.u)
                width: parent.width - 2 * x
                height: popup.headerHeight + popup.padding
                verticalAlignment: Text.AlignVCenter
                text: popup.title
                elide: Text.ElideRight
                color: T.textMuted
                font.family: T.fontUi
                font.pixelSize: Math.round(T.fontSmall * popup.u)
                font.weight: Font.DemiBold
                font.capitalization: Font.AllUppercase
                font.letterSpacing: 0.6 * popup.u
            }

            ListView {
                id: list
                anchors.fill: parent
                anchors.margins: popup.padding
                anchors.topMargin: popup.padding + popup.headerHeight
                clip: true
                focus: true
                model: popup.entries
                boundsBehavior: Flickable.StopAtBounds
                highlightMoveDuration: 0
                keyNavigationWraps: true
                Accessible.role: Accessible.List
                Accessible.name: popup.title

                Keys.onReturnPressed: popup.choose(currentIndex)
                Keys.onEnterPressed: popup.choose(currentIndex)
                Keys.onSpacePressed: popup.choose(currentIndex)
                Keys.onEscapePressed: popup.hide()
                Keys.onTabPressed: popup.hide()
                Keys.onBacktabPressed: popup.hide()
                Keys.onPressed: function (event) {
                    var page = Math.max(1, Math.floor(height / popup.rowHeight) - 1)
                    if (event.key === Qt.Key_Home) currentIndex = 0
                    else if (event.key === Qt.Key_End) currentIndex = count - 1
                    else if (event.key === Qt.Key_PageDown) currentIndex = Math.min(count - 1, currentIndex + page)
                    else if (event.key === Qt.Key_PageUp) currentIndex = Math.max(0, currentIndex - page)
                    else return
                    event.accepted = true
                }

                delegate: Item {
                    id: row
                    required property int index
                    required property var modelData
                    // How many rows away from the opener this row is drawn.
                    readonly property int step: Math.min(7, Math.round(Math.abs(
                        list.y + y - list.contentY + height / 2 - popup.originY) / popup.rowHeight))
                    width: list.width - (scrollbar.visible ? scrollbar.width + Math.round(4 * popup.u) : 0)
                    height: popup.rowHeight
                    opacity: popup.rowIn(step)
                    Accessible.role: Accessible.ListItem
                    Accessible.name: row.modelData.title

                    Rectangle {
                        anchors.fill: parent
                        radius: Math.round(T.radiusSm * popup.u)
                        color: list.currentIndex === row.index ? T.accentFill : rowMouse.containsMouse ? T.hoverFill : "transparent"
                        border.width: list.currentIndex === row.index && list.activeFocus ? 1 : 0
                        border.color: T.accent
                    }
                    Avatar {
                        id: face
                        visible: row.modelData.avatar === true
                        size: Math.round(32 * popup.u)
                        x: Math.round(T.spaceSm * popup.u)
                        anchors.verticalCenter: parent.verticalCenter
                        source: row.modelData.picture !== undefined ? row.modelData.picture : ""
                        name: row.modelData.avatarName !== undefined ? row.modelData.avatarName : ""
                    }
                    Column {
                        anchors.left: parent.left
                        anchors.right: tick.left
                        anchors.leftMargin: face.visible ? face.x + face.width + Math.round(T.spaceMd * popup.u) : Math.round(T.spaceMd * popup.u)
                        anchors.rightMargin: Math.round(T.spaceSm * popup.u)
                        anchors.verticalCenter: parent.verticalCenter
                        Text {
                            width: parent.width
                            text: row.modelData.title
                            elide: Text.ElideRight
                            color: T.textPrimary
                            font.family: T.fontUi
                            font.pixelSize: Math.round(T.fontBody * popup.u)
                            font.weight: popup.currentIndex === row.index ? Font.DemiBold : Font.Normal
                        }
                        Text {
                            width: parent.width
                            visible: text !== ""
                            text: row.modelData.subtitle !== undefined ? row.modelData.subtitle : ""
                            elide: Text.ElideRight
                            color: T.textSecondary
                            font.family: T.fontUi
                            font.pixelSize: Math.round(T.fontCaption * popup.u)
                        }
                    }
                    Image {
                        id: tick
                        visible: popup.currentIndex === row.index
                        anchors.right: parent.right
                        anchors.rightMargin: Math.round(T.spaceMd * popup.u)
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.round(16 * popup.u)
                        height: width
                        source: "../icons/check.svg"
                        sourceSize.width: width
                        sourceSize.height: height
                    }
                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        enabled: popup.open
                        hoverEnabled: true
                        onClicked: popup.choose(row.index)
                    }
                }

                Rectangle {
                    id: scrollbar
                    visible: list.contentHeight > list.height
                    anchors.right: parent.right
                    width: Math.max(3, Math.round(4 * popup.u))
                    radius: width / 2
                    color: T.borderStrong
                    y: list.visibleArea.yPosition * list.height
                    height: Math.max(Math.round(24 * popup.u), list.visibleArea.heightRatio * list.height)
                }
            }
        }
    }
}
