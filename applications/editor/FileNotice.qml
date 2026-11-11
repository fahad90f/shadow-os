// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.nexus.ui

// Message bar above the text of one file. Unlike NxBanner it does not hide
// itself when dismissed: visibility stays bound to the document's state, so
// the same bar can come back when the file changes again.
Rectangle {
    id: root

    property string kind: "info"            // "info" | "warning"
    property string title: ""
    property string message: ""
    property string actionText: ""
    property bool dismissible: false
    readonly property color tone: kind === "warning" ? Theme.warning : Theme.accent

    signal actionTriggered()
    signal dismissed()

    Layout.fillWidth: true
    implicitHeight: row.implicitHeight + Theme.spaceSm * 2
    radius: Theme.radiusMd
    color: Theme.alpha(tone, 0.10)
    border.width: Theme.borderWidth
    border.color: Theme.alpha(tone, 0.40)
    Accessible.role: Accessible.AlertMessage
    Accessible.name: title + " " + message

    RowLayout {
        id: row
        anchors.fill: parent
        anchors.leftMargin: Theme.spaceMd
        anchors.rightMargin: Theme.spaceSm
        anchors.topMargin: Theme.spaceSm
        anchors.bottomMargin: Theme.spaceSm
        spacing: Theme.spaceMd

        NxIcon {
            name: root.kind === "warning" ? "alert-triangle" : "info"
            size: Theme.icon
            color: root.tone
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceXxs
            Text {
                Layout.fillWidth: true
                visible: root.title !== ""
                text: root.title
                color: Theme.textPrimary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
                font.weight: Theme.weightSemibold
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                visible: root.message !== ""
                text: root.message
                color: Theme.textSecondary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
                wrapMode: Text.WordWrap
            }
        }
        NxButton {
            visible: root.actionText !== ""
            text: root.actionText
            onClicked: root.actionTriggered()
        }
        NxIconButton {
            visible: root.dismissible
            iconName: "close"
            iconSize: Theme.iconSmall
            tooltip: qsTr("Dismiss")
            onClicked: root.dismissed()
        }
    }
}
