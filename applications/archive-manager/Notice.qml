// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.shadow.ui

// Inline warning whose visibility stays bound to the state it describes
// (NxBanner hides itself for good once dismissed).
Rectangle {
    id: root

    property string kind: "warning"         // "warning" | "danger"
    property string title: ""
    property string message: ""
    readonly property color tone: kind === "danger" ? Theme.danger : Theme.warning

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
        anchors.margins: Theme.spaceSm
        anchors.leftMargin: Theme.spaceMd
        anchors.rightMargin: Theme.spaceMd
        spacing: Theme.spaceMd

        NxIcon {
            Layout.alignment: Qt.AlignTop
            name: root.kind === "danger" ? "shield-alert" : "alert-triangle"
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
    }
}
