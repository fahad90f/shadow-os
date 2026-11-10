// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Navigation sidebar.
//
//   NxSidebar {
//       title: "Settings"
//       model: [
//           { key: "wifi", title: "Wi-Fi", icon: "wifi", section: "Network" },
//           { key: "bt",   title: "Bluetooth", icon: "bluetooth", section: "Network", badge: "2" },
//       ]
//       currentKey: "wifi"
//       onActivated: (key) => stack.show(key)
//   }
Rectangle {
    id: root

    property string title: ""
    property var model: []
    property string currentKey: ""
    property alias header: headerSlot.data
    property alias footer: footerSlot.data
    property bool collapsed: false

    signal activated(string key)

    implicitWidth: collapsed ? 60 : Theme.sidebarWidth
    color: Theme.bgSurface
    Behavior on implicitWidth { NumberAnimation { duration: Theme.motionNormal; easing.type: Easing.OutCubic } }

    Rectangle {
        anchors.right: parent.right
        width: 1
        height: parent.height
        color: Theme.borderSubtle
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spaceSm
        anchors.rightMargin: Theme.spaceSm + 1
        spacing: Theme.spaceSm

        Text {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spaceSm
            Layout.topMargin: Theme.spaceSm
            Layout.bottomMargin: Theme.spaceXs
            visible: root.title !== "" && !root.collapsed
            text: root.title
            color: Theme.textPrimary
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeTitle
            font.weight: Theme.weightSemibold
            elide: Text.ElideRight
        }

        ColumnLayout {
            id: headerSlot
            Layout.fillWidth: true
            spacing: Theme.spaceSm
            visible: children.length > 0 && !root.collapsed
        }

        Flickable {
            id: flick
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentHeight: list.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            clip: true
            ScrollBar.vertical: ScrollBar { }

            Column {
                id: list
                width: flick.width
                spacing: 2

                Repeater {
                    model: root.model

                    Column {
                        id: entry
                        required property var modelData
                        required property int index
                        readonly property bool selected: modelData.key === root.currentKey
                        readonly property bool newSection: modelData.section !== undefined && modelData.section !== ""
                            && (index === 0 || root.model[index - 1].section !== modelData.section)

                        width: list.width
                        spacing: 2

                        Text {
                            visible: entry.newSection && !root.collapsed
                            width: parent.width
                            topPadding: entry.index === 0 ? Theme.spaceXs : Theme.spaceMd
                            bottomPadding: Theme.spaceXs
                            leftPadding: Theme.spaceSm
                            text: entry.modelData.section || ""
                            color: Theme.textMuted
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeCaption
                            font.weight: Theme.weightSemibold
                            font.capitalization: Font.AllUppercase
                            font.letterSpacing: 0.8
                        }
                        Item {
                            visible: entry.newSection && root.collapsed && entry.index > 0
                            width: parent.width
                            height: Theme.spaceSm
                        }

                        Rectangle {
                            id: item
                            width: parent.width
                            height: 36
                            radius: Theme.radiusSm
                            color: entry.selected ? Theme.accentFill
                                : tap.pressed ? Theme.pressedOverlay
                                : hover.hovered || item.activeFocus ? Theme.hoverOverlay : "transparent"
                            Behavior on color { ColorAnimation { duration: Theme.motionFast } }
                            activeFocusOnTab: true
                            Accessible.role: Accessible.PageTab
                            Accessible.name: entry.modelData.title
                            Keys.onReturnPressed: root.activated(entry.modelData.key)
                            Keys.onSpacePressed: root.activated(entry.modelData.key)

                            Rectangle {
                                visible: entry.selected
                                x: 0
                                anchors.verticalCenter: parent.verticalCenter
                                width: 3
                                height: 16
                                radius: 1.5
                                color: Theme.accent
                            }

                            HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
                            TapHandler { id: tap; onTapped: root.activated(entry.modelData.key) }

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: root.collapsed ? (parent.width - Theme.icon) / 2 : Theme.spaceMd
                                anchors.rightMargin: Theme.spaceSm
                                spacing: Theme.spaceMd

                                NxIcon {
                                    visible: (entry.modelData.icon || "") !== ""
                                    name: entry.modelData.icon || ""
                                    size: Theme.icon - 2
                                    color: entry.selected ? Theme.accent : Theme.textSecondary
                                }
                                Text {
                                    Layout.fillWidth: true
                                    visible: !root.collapsed
                                    text: entry.modelData.title
                                    color: entry.selected ? Theme.textPrimary : Theme.textSecondary
                                    font.family: Theme.fontUi
                                    font.pixelSize: Theme.fontSizeBody
                                    font.weight: entry.selected ? Theme.weightSemibold : Theme.weightMedium
                                    elide: Text.ElideRight
                                }
                                Rectangle {
                                    visible: !root.collapsed && (entry.modelData.badge || "") !== ""
                                    implicitWidth: Math.max(18, badgeText.implicitWidth + 10)
                                    implicitHeight: 18
                                    radius: 9
                                    color: Theme.accentFill
                                    Text {
                                        id: badgeText
                                        anchors.centerIn: parent
                                        text: entry.modelData.badge || ""
                                        color: Theme.accent
                                        font.family: Theme.fontUi
                                        font.pixelSize: Theme.fontSizeCaption
                                        font.weight: Theme.weightSemibold
                                    }
                                }
                            }

                            ToolTip.visible: root.collapsed && hover.hovered
                            ToolTip.text: entry.modelData.title
                        }
                    }
                }
            }
        }

        ColumnLayout {
            id: footerSlot
            Layout.fillWidth: true
            spacing: Theme.spaceSm
            visible: children.length > 0
        }
    }
}
