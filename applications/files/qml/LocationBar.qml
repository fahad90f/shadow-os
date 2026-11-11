// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.files

// Path bar of the active pane: clickable breadcrumbs that turn into an
// editable location field (Ctrl+L, the pencil button, or a click on the empty
// part of the bar). The field accepts paths, "~" and any KIO URL.
Item {
    id: bar

    required property var shell
    // FolderModel of the active pane.
    property var folder: null
    property bool editing: false

    function edit() {
        bar.editing = true
        field.text = bar.folder ? bar.folder.location : ""
        field.forceActiveFocus()
        field.selectAll()
    }

    function stopEditing() {
        if (!bar.editing) return
        bar.editing = false
        bar.shell.focusActivePane()
    }

    implicitHeight: Theme.controlHeight
    implicitWidth: 320

    Rectangle {
        anchors.fill: parent
        radius: Theme.radiusSm
        color: Theme.bgInput
        border.width: bar.editing ? Theme.focusRing : Theme.borderWidth
        border.color: bar.editing ? Theme.accent : hover.hovered ? Theme.borderStrong : Theme.border
        Behavior on border.color { ColorAnimation { duration: Theme.motionFast } }
    }
    HoverHandler { id: hover }

    // Click on the free part of the bar: edit the location.
    MouseArea {
        anchors.fill: parent
        visible: !bar.editing
        cursorShape: Qt.IBeamCursor
        onClicked: bar.edit()
    }

    ListView {
        id: crumbs
        anchors.left: parent.left
        anchors.right: editButton.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.leftMargin: Theme.spaceXs
        visible: !bar.editing
        orientation: ListView.Horizontal
        interactive: false
        clip: true
        model: bar.folder ? bar.folder.crumbs : []
        // Long paths keep their end visible.
        onModelChanged: Qt.callLater(crumbs.positionViewAtEnd)
        onWidthChanged: Qt.callLater(crumbs.positionViewAtEnd)
        onContentWidthChanged: Qt.callLater(crumbs.positionViewAtEnd)

        delegate: Row {
            id: crumb
            required property var modelData
            required property int index
            readonly property bool last: index === crumbs.count - 1
            height: crumbs.height
            spacing: 0

            NxIcon {
                visible: crumb.index > 0
                anchors.verticalCenter: parent.verticalCenter
                name: "chevron-right"
                size: Theme.iconSmall - 4
                color: Theme.textMuted
            }
            Rectangle {
                id: chip
                anchors.verticalCenter: parent.verticalCenter
                width: chipRow.implicitWidth + Theme.spaceSm * 2
                height: parent.height - Theme.spaceSm
                radius: Theme.radiusXs
                color: chipDrop.containsDrag ? Theme.accentFill
                    : chipMouse.pressed ? Theme.pressedOverlay
                    : chipMouse.containsMouse ? Theme.hoverOverlay : "transparent"
                activeFocusOnTab: true
                Accessible.role: Accessible.Button
                Accessible.name: crumb.modelData.label
                Keys.onReturnPressed: bar.shell.navigate(crumb.modelData.url)
                Keys.onSpacePressed: bar.shell.navigate(crumb.modelData.url)

                Row {
                    id: chipRow
                    anchors.centerIn: parent
                    spacing: Theme.spaceXs + 2
                    NxIcon {
                        visible: (crumb.modelData.icon || "") !== ""
                        anchors.verticalCenter: parent.verticalCenter
                        name: crumb.modelData.icon || ""
                        size: Theme.iconSmall
                        color: crumb.last ? Theme.accent : Theme.textSecondary
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: crumb.modelData.label
                        color: crumb.last ? Theme.textPrimary : Theme.textSecondary
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeBody
                        font.weight: crumb.last ? Theme.weightSemibold : Theme.weightMedium
                    }
                }
                MouseArea {
                    id: chipMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                    cursorShape: Qt.PointingHandCursor
                    onClicked: (mouse) => {
                        if (mouse.button === Qt.MiddleButton) bar.shell.openInNewTab(crumb.modelData.url, false)
                        else bar.shell.navigate(crumb.modelData.url)
                    }
                }
                DropArea {
                    id: chipDrop
                    anchors.fill: parent
                    keys: ["text/uri-list"]
                    onEntered: (drag) => { drag.accepted = FileOps.dropAction(drag.urls, crumb.modelData.url) !== "" }
                    onDropped: (drop) => bar.shell.handleDrop(drop, crumb.modelData.url)
                }
            }
        }
    }

    // Shown when the beginning of a long path is scrolled out of view.
    Rectangle {
        visible: !bar.editing && crumbs.contentX > crumbs.originX + 1
        anchors.left: parent.left
        anchors.leftMargin: Theme.borderWidth
        anchors.verticalCenter: parent.verticalCenter
        width: ellipsis.implicitWidth + Theme.spaceSm * 2
        height: parent.height - Theme.borderWidth * 2
        radius: Theme.radiusSm
        color: Theme.bgInput
        Text {
            id: ellipsis
            anchors.centerIn: parent
            text: "…"
            color: Theme.textMuted
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeBody
        }
    }

    TextField {
        id: field
        anchors.left: parent.left
        anchors.right: editButton.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        visible: bar.editing
        background: null
        selectByMouse: true
        font.family: Theme.fontMono
        font.pixelSize: Theme.fontSizeSmall
        placeholderText: qsTr("Path or address, e.g. /usr/share or sftp://host/folder")
        Accessible.name: qsTr("Location")
        onAccepted: {
            const text = field.text
            bar.stopEditing()
            bar.shell.navigateText(text)
        }
        Keys.onEscapePressed: bar.stopEditing()
        onActiveFocusChanged: if (!activeFocus && bar.editing) bar.editing = false
    }

    NxIconButton {
        id: editButton
        anchors.right: parent.right
        anchors.rightMargin: 2
        anchors.verticalCenter: parent.verticalCenter
        width: parent.height - 4
        height: parent.height - 4
        iconName: bar.editing ? "check" : "edit"
        iconSize: Theme.iconSmall
        tooltip: bar.editing ? qsTr("Go to this location") : qsTr("Edit location (Ctrl+L)")
        focusPolicy: Qt.NoFocus
        onClicked: {
            if (bar.editing) field.accepted()
            else bar.edit()
        }
    }
}
