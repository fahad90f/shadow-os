// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts

// Inline message bar.  kind: "info" | "success" | "warning" | "danger"
Rectangle {
    id: root

    property string kind: "info"
    property string title: ""
    property string message: ""
    property string actionText: ""
    property bool dismissible: false

    signal actionTriggered()
    signal dismissed()

    readonly property color tone: kind === "success" ? Theme.success
        : kind === "warning" ? Theme.warning
        : kind === "danger" ? Theme.danger : Theme.accent
    readonly property string glyph: kind === "success" ? "check-circle"
        : kind === "warning" ? "alert-triangle"
        : kind === "danger" ? "x-circle" : "info"

    Layout.fillWidth: true
    implicitHeight: row.implicitHeight + Theme.spaceMd * 2
    radius: Theme.radiusMd
    color: Theme.alpha(tone, 0.10)
    border.width: Theme.borderWidth
    border.color: Theme.alpha(tone, 0.40)
    Accessible.role: Accessible.AlertMessage
    Accessible.name: title + " " + message

    RowLayout {
        id: row
        anchors.fill: parent
        anchors.margins: Theme.spaceMd
        spacing: Theme.spaceMd

        NxIcon {
            Layout.alignment: Qt.AlignTop
            name: root.glyph
            size: Theme.icon
            color: root.tone
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
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
            onClicked: { root.visible = false; root.dismissed() }
        }
    }
}
