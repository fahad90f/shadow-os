// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui

// A framed list to choose one entry from. Keyboard: Tab reaches the list,
// Up/Down/Home/End/PageUp/PageDown move, Return or Space choose.
//   model: [{ <keyRole>, <titleRole>, <subtitleRole>, <trailingRole> }]
Rectangle {
    id: root

    property var model: []
    property string keyRole: "code"
    property string titleRole: "name"
    property string subtitleRole: ""
    property string trailingRole: ""
    property string selectedKey: ""
    property string emptyText: qsTr("Nothing matches.")
    property string accessibleName: ""
    property bool monoTrailing: true

    signal picked(string key, var entry)

    function positionAtSelection() {
        for (let i = 0; i < model.length; ++i) {
            if (model[i][keyRole] === selectedKey) {
                view.currentIndex = i
                view.positionViewAtIndex(i, ListView.Center)
                return
            }
        }
        view.currentIndex = model.length > 0 ? 0 : -1
        view.positionViewAtBeginning()
    }
    function focusList() { view.forceActiveFocus() }
    onModelChanged: Qt.callLater(positionAtSelection)

    Layout.fillWidth: true
    Layout.fillHeight: true
    color: Theme.bgRaised
    radius: Theme.radiusMd
    border.width: view.activeFocus ? Theme.focusRing : Theme.borderWidth
    border.color: view.activeFocus ? Theme.accent : Theme.borderSubtle
    clip: true

    ListView {
        id: view
        anchors.fill: parent
        anchors.margins: Theme.borderWidth + 1
        model: root.model
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        activeFocusOnTab: true
        keyNavigationEnabled: true
        highlightMoveDuration: 0
        ScrollBar.vertical: ScrollBar { }
        Accessible.role: Accessible.List
        Accessible.name: root.accessibleName

        Keys.onReturnPressed: choose(currentIndex)
        Keys.onEnterPressed: choose(currentIndex)
        Keys.onSpacePressed: choose(currentIndex)
        Keys.onPressed: (event) => {
            if (event.key === Qt.Key_Home) { currentIndex = 0; event.accepted = true }
            else if (event.key === Qt.Key_End) { currentIndex = count - 1; event.accepted = true }
            else if (event.key === Qt.Key_PageDown) { currentIndex = Math.min(count - 1, currentIndex + 8); event.accepted = true }
            else if (event.key === Qt.Key_PageUp) { currentIndex = Math.max(0, currentIndex - 8); event.accepted = true }
        }
        function choose(index) {
            if (index >= 0 && index < root.model.length)
                root.picked(root.model[index][root.keyRole], root.model[index])
        }

        delegate: Rectangle {
            id: row
            required property var modelData
            required property int index
            readonly property bool selected: modelData[root.keyRole] === root.selectedKey
            readonly property bool current: ListView.isCurrentItem && view.activeFocus
            readonly property string subtitleText: root.subtitleRole !== "" ? String(modelData[root.subtitleRole] ?? "") : ""

            width: ListView.view.width
            height: subtitleText !== "" ? Theme.rowHeightLarge : Theme.rowHeight
            radius: Theme.radiusSm
            color: selected ? Theme.accentFill : tap.pressed ? Theme.pressedOverlay
                 : hover.hovered || current ? Theme.hoverOverlay : "transparent"
            border.width: current ? Theme.borderWidth : 0
            border.color: Theme.accent
            Accessible.role: Accessible.ListItem
            Accessible.name: String(modelData[root.titleRole]) + (subtitleText !== "" ? ", " + subtitleText : "")
            Accessible.selected: selected

            HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
            TapHandler {
                id: tap
                onTapped: { view.currentIndex = row.index; view.forceActiveFocus(); view.choose(row.index) }
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.spaceMd
                anchors.rightMargin: Theme.spaceMd + (view.contentHeight > view.height ? Theme.spaceSm : 0)
                spacing: Theme.spaceMd

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 1
                    Text {
                        Layout.fillWidth: true
                        text: String(row.modelData[root.titleRole])
                        color: row.selected ? Theme.accent : Theme.textPrimary
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeBody
                        font.weight: row.selected ? Theme.weightSemibold : Theme.weightRegular
                        elide: Text.ElideRight
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: row.subtitleText !== ""
                        text: row.subtitleText
                        color: Theme.textMuted
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeSmall
                        elide: Text.ElideRight
                    }
                }
                Text {
                    visible: root.trailingRole !== ""
                    text: root.trailingRole !== "" ? String(row.modelData[root.trailingRole] ?? "") : ""
                    color: Theme.textMuted
                    font.family: root.monoTrailing ? Theme.fontMono : Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                }
                NxIcon {
                    name: "check"
                    size: Theme.iconSmall
                    color: Theme.accent
                    opacity: row.selected ? 1 : 0
                }
            }
        }

        Text {
            anchors.centerIn: parent
            width: parent.width - Theme.spaceXl * 2
            visible: view.count === 0
            horizontalAlignment: Text.AlignHCenter
            text: root.emptyText
            color: Theme.textMuted
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeBody
            wrapMode: Text.WordWrap
        }
    }
}
