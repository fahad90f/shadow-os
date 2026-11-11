// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui

// A labelled row of form controls inside an NxSection. Unlike NxRow the
// controls get all remaining width, so text fields can use Layout.fillWidth.
Item {
    id: root

    default property alias content: controls.data
    property string label: ""
    property string hint: ""        // explanation under the label
    property string error: ""       // validation message under the controls
    property real labelWidth: 220

    readonly property bool isFirst: parent && parent.children.length > 0 && parent.children[0] === root

    Layout.fillWidth: true
    implicitHeight: Math.max(Theme.rowHeight, body.implicitHeight + Theme.spaceSm * 2)

    Rectangle {
        visible: !root.isFirst
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: Theme.spaceLg
        height: 1
        color: Theme.borderSubtle
    }

    RowLayout {
        id: body
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: Theme.spaceLg
        anchors.rightMargin: Theme.spaceLg
        spacing: Theme.spaceMd

        ColumnLayout {
            Layout.preferredWidth: root.labelWidth
            Layout.minimumWidth: root.labelWidth
            Layout.maximumWidth: root.labelWidth
            Layout.alignment: Qt.AlignVCenter
            spacing: 2
            Label {
                Layout.fillWidth: true
                text: root.label
                color: root.enabled ? Theme.textPrimary : Theme.textDisabled
                font.weight: Theme.weightMedium
                elide: Text.ElideRight
            }
            Label {
                Layout.fillWidth: true
                visible: root.hint !== ""
                text: root.hint
                color: Theme.textMuted
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WordWrap
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            spacing: Theme.spaceXs
            RowLayout {
                id: controls
                Layout.fillWidth: true
                spacing: Theme.spaceSm
            }
            Label {
                Layout.fillWidth: true
                visible: root.error !== ""
                text: root.error
                color: Theme.danger
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WordWrap
            }
        }
    }
}
