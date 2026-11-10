// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick

// Small status pill.  kind: "neutral" | "accent" | "success" | "warning" | "danger"
Rectangle {
    id: root

    property string text: ""
    property string kind: "neutral"
    property bool dot: true

    readonly property color tone: kind === "success" ? Theme.success
        : kind === "warning" ? Theme.warning
        : kind === "danger" ? Theme.danger
        : kind === "accent" ? Theme.accent : Theme.textSecondary

    implicitWidth: row.implicitWidth + Theme.spaceSm * 2
    implicitHeight: 22
    radius: height / 2
    color: Theme.alpha(tone, kind === "neutral" ? 0.10 : 0.14)
    border.width: Theme.borderWidth
    border.color: Theme.alpha(tone, 0.35)

    Row {
        id: row
        anchors.centerIn: parent
        spacing: Theme.spaceXs + 1
        Rectangle {
            visible: root.dot
            anchors.verticalCenter: parent.verticalCenter
            width: 6
            height: 6
            radius: 3
            color: root.tone
        }
        Text {
            text: root.text
            color: root.tone
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeCaption
            font.weight: Theme.weightSemibold
        }
    }
}
