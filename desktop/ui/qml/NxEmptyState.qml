// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts

// Centered placeholder for "nothing here", "service unavailable" and errors.
// It always says what is wrong and, where possible, offers the action that
// fixes it — never a blank area.
Item {
    id: root

    property string icon: "info"
    property string title: ""
    property string message: ""
    property string actionText: ""
    property string tone: "neutral"     // "neutral" | "warning" | "danger"

    signal actionTriggered()

    implicitWidth: 360
    implicitHeight: column.implicitHeight + Theme.spaceXxl * 2

    ColumnLayout {
        id: column
        anchors.centerIn: parent
        width: Math.min(420, root.width - Theme.spaceXl * 2)
        spacing: Theme.spaceMd

        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            implicitWidth: 64
            implicitHeight: 64
            radius: 32
            color: root.tone === "danger" ? Theme.dangerFill : root.tone === "warning" ? Theme.warningFill : Theme.accentFill
            NxIcon {
                anchors.centerIn: parent
                name: root.icon
                size: 28
                color: root.tone === "danger" ? Theme.danger : root.tone === "warning" ? Theme.warning : Theme.accent
            }
        }
        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: root.title
            color: Theme.textPrimary
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeTitle
            font.weight: Theme.weightSemibold
            wrapMode: Text.WordWrap
        }
        Text {
            Layout.fillWidth: true
            visible: root.message !== ""
            horizontalAlignment: Text.AlignHCenter
            text: root.message
            color: Theme.textSecondary
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeBody
            wrapMode: Text.WordWrap
        }
        NxButton {
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: Theme.spaceXs
            visible: root.actionText !== ""
            text: root.actionText
            kind: "primary"
            onClicked: root.actionTriggered()
        }
    }
}
