// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import org.shadow.assistant

// One setting: a title, a plain-language note, and its control on the right.
Item {
    id: r
    property string title: ""
    property string note: ""
    property color noteColor: Pe.inkMute
    property bool divider: true
    default property alias trailing: trail.data
    implicitHeight: Math.max(54, textCol.implicitHeight + 26, trail.implicitHeight + 20)
    Accessible.role: Accessible.Grouping
    Accessible.name: title

    Column {
        id: textCol
        anchors.verticalCenter: parent.verticalCenter
        width: parent.width - trail.width - 28
        spacing: 3
        Text {
            textFormat: Text.PlainText
            width: parent.width
            text: r.title
            wrapMode: Text.Wrap
            font.family: Pe.sans
            font.pixelSize: 14
            font.weight: Font.Medium
            color: Pe.ink
        }
        Text {
            textFormat: Text.PlainText
            visible: r.note !== ""
            width: parent.width
            text: r.note
            wrapMode: Text.Wrap
            lineHeight: 1.25
            font.family: Pe.sans
            font.pixelSize: Pe.small
            color: r.noteColor
        }
    }
    Row {
        id: trail
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        spacing: 10
    }
    Rectangle {
        visible: r.divider
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: Pe.line
    }
}
