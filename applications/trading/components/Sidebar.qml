// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui

// The left column of the desk: the brand, the pages, and the "Live now" card.
Rectangle {
    id: root

    property var model: []          // [{key, title, icon}]
    property string currentKey: ""
    signal activated(string key)

    implicitWidth: 196
    color: Theme.bgBase

    // A faint violet light from the top, as on the rest of the desk.
    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0; color: Theme.alpha(Theme.accent, 0.10) }
            GradientStop { position: 0.45; color: Theme.alpha(Theme.accent, 0.0) }
        }
    }
    Rectangle {
        anchors.right: parent.right
        width: Theme.borderWidth
        height: parent.height
        color: Theme.alpha(Theme.accent, 0.25)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spaceMd
        spacing: Theme.spaceXs

        // ---- brand -------------------------------------------------------
        BrandMark {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: 92
            Layout.preferredHeight: 92
        }
        Label {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("SHADOW")
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSizeTitle
            font.weight: Theme.weightSemibold
            font.letterSpacing: 4
        }
        Label {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("TRADING")
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSizeBodyLarge
            font.letterSpacing: 5
        }
        Label {
            Layout.alignment: Qt.AlignHCenter
            Layout.bottomMargin: Theme.spaceLg
            text: qsTr("Trade Smarter. With AI.")
            color: Theme.textMuted
            font.pixelSize: Theme.fontSizeCaption
        }

        // ---- pages ---------------------------------------------------------
        Repeater {
            model: root.model
            Rectangle {
                id: item
                required property var modelData
                readonly property bool selected: modelData.key === root.currentKey

                Layout.fillWidth: true
                implicitHeight: 40
                radius: Theme.radiusMd
                activeFocusOnTab: true
                Accessible.role: Accessible.PageTab
                Accessible.name: modelData.title
                Accessible.selected: selected
                Keys.onReturnPressed: root.activated(modelData.key)
                Keys.onSpacePressed: root.activated(modelData.key)

                color: selected ? "transparent" : hover.hovered ? Theme.hoverOverlay : "transparent"
                gradient: selected ? selectedFill : null
                border.width: activeFocus ? 2 : selected ? Theme.borderWidth : 0
                border.color: activeFocus ? Theme.focusRing : Theme.alpha(Theme.accent, 0.9)

                Gradient {
                    id: selectedFill
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0; color: Theme.alpha(Theme.accent, 0.85) }
                    GradientStop { position: 1; color: Theme.alpha(Theme.accent, 0.35) }
                }
                HoverHandler { id: hover }
                TapHandler { onTapped: root.activated(item.modelData.key) }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spaceMd
                    anchors.rightMargin: Theme.spaceSm
                    spacing: Theme.spaceMd
                    NxIcon {
                        name: item.modelData.icon
                        size: Theme.iconSmall + 2
                        color: item.selected ? Theme.textPrimary : Theme.accent
                    }
                    Label {
                        Layout.fillWidth: true
                        text: item.modelData.title
                        color: item.selected ? Theme.textPrimary : Theme.textSecondary
                        font.pixelSize: Theme.fontSizeBody
                        font.weight: item.selected ? Theme.weightSemibold : Theme.weightRegular
                        elide: Text.ElideRight
                    }
                }
            }
        }

        Item { Layout.fillHeight: true }

        // ---- live now ---------------------------------------------------------
        GlowCard {
            id: live
            Layout.fillWidth: true
            implicitHeight: liveColumn.implicitHeight + padding * 2
            padding: Theme.spaceSm + 2
            tone: trading.dataState === "live" ? Theme.success
                : trading.dataState === "error" ? Theme.danger : Theme.accent
            Accessible.name: liveTitle.text + ", " + liveDetail.text

            // The clock runs only while the window shows it.
            property date now: new Date()
            Timer {
                interval: 1000
                running: live.visible && Qt.application.state === Qt.ApplicationActive
                repeat: true
                triggeredOnStart: true
                onTriggered: live.now = new Date()
            }

            ColumnLayout {
                id: liveColumn
                anchors.left: parent.left
                anchors.right: parent.right
                spacing: 2
                RowLayout {
                    spacing: Theme.spaceSm
                    Rectangle { width: 9; height: 9; radius: 5; color: live.tone }
                    Label {
                        id: liveTitle
                        text: trading.dataState === "live" ? qsTr("Live Now")
                            : trading.dataState === "error" ? qsTr("Offline") : qsTr("Connecting")
                        color: live.tone
                        font.pixelSize: Theme.fontSizeBody
                        font.weight: Theme.weightSemibold
                    }
                }
                Label {
                    id: liveDetail
                    Layout.fillWidth: true
                    Layout.leftMargin: 9 + Theme.spaceSm
                    text: trading.dataState === "live" ? qsTr("Market Open")
                        : trading.dataState === "error" ? qsTr("No market data") : qsTr("Waiting for prices")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSizeSmall
                    elide: Text.ElideRight
                }
                Label {
                    Layout.leftMargin: 9 + Theme.spaceSm
                    text: Qt.formatTime(live.now, "HH:mm:ss") + " (" + Qt.formatDateTime(live.now, "t") + ")"
                    color: Theme.textMuted
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fontSizeCaption
                }
            }
        }
    }
}
