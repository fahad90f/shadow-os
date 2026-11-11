// SPDX-License-Identifier: GPL-3.0-or-later
// List to choose one entry from (users, sessions, keyboard layouts).
// Keyboard: Up/Down/Home/End/PageUp/PageDown move, Return/Enter/Space
// choose, Escape or Tab close; focus returns to the item that opened it.
import QtQuick 2.15
import "../tokens.js" as T

FocusScope {
    id: popup

    property real u: 1
    property var entries: []          // [{ title, subtitle, picture, avatar }]
    property int currentIndex: -1
    property Item opener: null
    property bool open: false
    property real maximumHeight: 400
    property string title: ""
    readonly property real rowHeight: Math.round((popup.hasSubtitles ? 52 : T.rowHeight) * u)
    readonly property real padding: Math.round(T.spaceSm * u)
    readonly property real headerHeight: title !== "" ? Math.round(36 * u) : 0
    property bool hasSubtitles: false
    signal chosen(int index)

    visible: open
    width: Math.round(300 * u)
    height: Math.min(maximumHeight, entries.length * rowHeight + 2 * padding + headerHeight)

    function show() {
        open = true
        list.currentIndex = Math.max(0, Math.min(entries.length - 1, currentIndex))
        list.positionViewAtIndex(list.currentIndex, ListView.Contain)
        list.forceActiveFocus()
    }
    function hide() {
        if (!open)
            return
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

    Rectangle {
        anchors.fill: parent
        radius: Math.round(T.radiusLg * popup.u)
        color: T.bgRaised
        border.width: 1
        border.color: T.border
    }
    // Swallow clicks on the padding so they do not close the list.
    MouseArea { anchors.fill: parent }

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
            width: list.width - (scrollbar.visible ? scrollbar.width + Math.round(4 * popup.u) : 0)
            height: popup.rowHeight
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
