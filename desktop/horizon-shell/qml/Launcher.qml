// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui

// The application launcher: type to search, arrow keys to move, Enter to
// start. Categories on the left narrow the grid.
OverlaySurface {
    id: root

    scope: "shadow-launcher"
    scrimOpacity: 0.55

    signal sessionMenuRequested()

    onOpenChanged: {
        if (open) {
            search.text = ""
            applications.filter = ""
            applications.group = ""
            grid.currentIndex = 0
            search.forceActiveFocus()
        }
    }

    function launchCurrent() {
        if (grid.currentIndex >= 0 && applications.launchAt(grid.currentIndex, root))
            root.hide()
    }

    Rectangle {
        id: card
        anchors.centerIn: parent
        width: Math.min(980, root.width - 96)
        height: Math.min(660, root.height - 140)
        radius: Theme.radiusXl
        color: cardBlur.glass ? Theme.glassPopup : Theme.bgOverlay
        BlurBehind { id: cardBlur; anchors.fill: parent; radius: card.radius; surface: root }
        border.width: Theme.borderWidth
        border.color: Theme.border
        scale: root.open ? 1 : 0.96
        opacity: root.open ? 1 : 0
        Behavior on scale { NumberAnimation { duration: Theme.motionNormal; easing.type: Easing.OutCubic } }
        Behavior on opacity { NumberAnimation { duration: Theme.motionFast } }

        // Swallow clicks so they do not reach the scrim and dismiss the launcher.
        TapHandler { }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: Theme.spaceXl
            spacing: Theme.spaceLg

            NxSearchField {
                id: search
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.controlHeightLarge + 4
                placeholderText: qsTr("Search applications")
                font.pixelSize: Theme.fontSizeBodyLarge
                onTextChanged: { applications.filter = text; grid.currentIndex = applications.count > 0 ? 0 : -1 }
                Keys.onDownPressed: { grid.forceActiveFocus(); if (grid.currentIndex < 0) grid.currentIndex = 0 }
                Keys.onReturnPressed: root.launchCurrent()
                Keys.onEnterPressed: root.launchCurrent()
                Keys.onEscapePressed: (event) => {
                    if (text !== "") text = ""
                    else { root.hide(); root.dismissed() }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: Theme.spaceLg

                // ---- categories -------------------------------------------------
                ListView {
                    id: groupList
                    Layout.preferredWidth: 168
                    Layout.fillHeight: true
                    clip: true
                    spacing: 2
                    visible: search.text === ""
                    model: [qsTr("All applications")].concat(applications.groups)
                    delegate: Rectangle {
                        id: groupItem
                        required property string modelData
                        required property int index
                        readonly property string key: index === 0 ? "" : modelData
                        readonly property bool selected: applications.group === key
                        width: groupList.width
                        height: 34
                        radius: Theme.radiusSm
                        color: selected ? Theme.accentFill : groupHover.hovered ? Theme.hoverOverlay : "transparent"
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            x: Theme.spaceMd
                            width: parent.width - Theme.spaceMd * 2
                            text: groupItem.modelData
                            color: groupItem.selected ? Theme.textPrimary : Theme.textSecondary
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeBody
                            font.weight: groupItem.selected ? Theme.weightSemibold : Theme.weightMedium
                            elide: Text.ElideRight
                        }
                        HoverHandler { id: groupHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: { applications.group = groupItem.key; grid.currentIndex = 0 } }
                    }
                }

                // ---- application grid ---------------------------------------------
                GridView {
                    id: grid
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    cellWidth: Math.floor(width / Math.max(1, Math.floor(width / 128)))
                    cellHeight: 118
                    model: applications
                    boundsBehavior: Flickable.StopAtBounds
                    keyNavigationEnabled: true
                    highlightMoveDuration: 0
                    ScrollBar.vertical: ScrollBar { }

                    Keys.onReturnPressed: root.launchCurrent()
                    Keys.onEnterPressed: root.launchCurrent()
                    Keys.onPressed: (event) => {
                        // Typing while the grid has focus continues the search.
                        if (event.text.length === 1 && event.text.charCodeAt(0) >= 32 && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))) {
                            search.forceActiveFocus()
                            search.text += event.text
                            event.accepted = true
                        } else if (event.key === Qt.Key_Backspace) {
                            search.forceActiveFocus()
                            search.text = search.text.slice(0, -1)
                            event.accepted = true
                        } else if (event.key === Qt.Key_Up && grid.currentIndex < Math.floor(grid.width / grid.cellWidth)) {
                            search.forceActiveFocus()
                            event.accepted = true
                        }
                    }

                    delegate: Item {
                        id: cell
                        required property int index
                        required property string name
                        required property string icon
                        required property string comment
                        readonly property bool current: GridView.isCurrentItem
                        width: grid.cellWidth
                        height: grid.cellHeight
                        Accessible.role: Accessible.Button
                        Accessible.name: name
                        Accessible.description: comment

                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: 4
                            radius: Theme.radiusMd
                            color: cell.current ? Theme.accentFill : cellHover.hovered ? Theme.hoverOverlay : "transparent"
                            border.width: cell.current && grid.activeFocus ? 1 : 0
                            border.color: Theme.alpha(Theme.accent, 0.6)
                        }
                        NxAppIcon {
                            id: cellIcon
                            anchors.horizontalCenter: parent.horizontalCenter
                            y: 14
                            name: cell.icon
                            size: 56
                        }
                        Text {
                            anchors.top: cellIcon.bottom
                            anchors.topMargin: 8
                            anchors.horizontalCenter: parent.horizontalCenter
                            width: parent.width - 16
                            horizontalAlignment: Text.AlignHCenter
                            text: cell.name
                            color: Theme.textPrimary
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeSmall
                            font.weight: Theme.weightMedium
                            elide: Text.ElideRight
                            maximumLineCount: 2
                            wrapMode: Text.Wrap
                        }
                        HoverHandler { id: cellHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler {
                            onTapped: { grid.currentIndex = cell.index; root.launchCurrent() }
                        }
                    }

                    NxEmptyState {
                        anchors.fill: parent
                        visible: grid.count === 0
                        icon: "search"
                        title: search.text !== "" ? qsTr("No application matches “%1”").arg(search.text)
                                                   : qsTr("No applications in this category")
                        message: applications.totalCount === 0
                            ? qsTr("The application database is empty. It is rebuilt automatically when applications are installed.") : ""
                    }
                }
            }

            // ---- footer ---------------------------------------------------------
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceMd
                Rectangle {
                    implicitWidth: 34; implicitHeight: 34; radius: 17
                    color: Theme.accentFill
                    NxIcon { anchors.centerIn: parent; name: "user"; size: Theme.icon; color: Theme.accent }
                }
                ColumnLayout {
                    spacing: 0
                    Text {
                        text: session.fullName
                        color: Theme.textPrimary
                        font.family: Theme.fontUi; font.pixelSize: Theme.fontSizeBody; font.weight: Theme.weightSemibold
                    }
                    Text {
                        text: session.userName + "@" + session.hostName + (session.liveSession ? "  ·  " + qsTr("live session") : "")
                        color: Theme.textMuted
                        font.family: Theme.fontUi; font.pixelSize: Theme.fontSizeCaption
                    }
                }
                Item { Layout.fillWidth: true }
                Text {
                    text: qsTr("%n application(s)", "", applications.count)
                    color: Theme.textMuted
                    font.family: Theme.fontUi; font.pixelSize: Theme.fontSizeSmall
                }
                NxIconButton {
                    iconName: "settings"
                    tooltip: qsTr("Control Center")
                    visible: session.programExists("shadow-control-center")
                    onClicked: { if (session.run("shadow-control-center")) root.hide() }
                }
                NxIconButton {
                    iconName: "power"
                    tooltip: qsTr("Lock, log out or power off")
                    onClicked: { root.hide(); root.sessionMenuRequested() }
                }
            }
        }
    }
}
