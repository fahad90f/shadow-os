// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui

// One repair action inside an NxSection: what it does, what it changes, the
// button(s) that start it, and how the last run ended.
Item {
    id: root

    property string icon: "wrench"
    property string title: ""
    // What the action does, in plain words.
    property string explanation: ""
    // What it changes on the installed system ("Nothing" for read-only actions).
    property string changes: ""
    // Why it cannot be started right now; empty when it can.
    property string unavailableReason: ""
    // { status: "ok" | "failed" | "warning", message } of the last run, or undefined.
    property var result: undefined
    property bool running: false
    default property alias buttons: buttonRow.data

    readonly property bool isFirst: parent && parent.children.length > 0 && parent.children[0] === root

    Layout.fillWidth: true
    implicitHeight: body.implicitHeight + Theme.spaceMd * 2
    Accessible.role: Accessible.Grouping
    Accessible.name: title
    Accessible.description: explanation

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

        NxIcon {
            Layout.alignment: Qt.AlignTop
            Layout.topMargin: 2
            name: root.icon
            size: Theme.icon
            color: Theme.textSecondary
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceXxs

            Text {
                Layout.fillWidth: true
                text: root.title
                color: Theme.textPrimary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
                font.weight: Theme.weightMedium
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                text: root.explanation
                color: Theme.textSecondary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                visible: root.changes !== ""
                text: qsTr("Changes: %1").arg(root.changes)
                color: Theme.textMuted
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                visible: root.unavailableReason !== ""
                text: root.unavailableReason
                color: Theme.warning
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spaceXxs
                visible: root.result !== undefined && !root.running
                spacing: Theme.spaceSm
                NxBadge {
                    Layout.alignment: Qt.AlignTop
                    text: !root.result ? "" : root.result.status === "ok" ? qsTr("Done")
                          : root.result.status === "warning" ? qsTr("Done, with remarks") : qsTr("Failed")
                    kind: !root.result ? "neutral" : root.result.status === "ok" ? "success"
                          : root.result.status === "warning" ? "warning" : "danger"
                }
                Text {
                    Layout.fillWidth: true
                    text: root.result ? root.result.message : ""
                    color: Theme.textSecondary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                    wrapMode: Text.WordWrap
                }
            }
        }

        Row {
            id: buttonRow
            Layout.alignment: Qt.AlignTop
            spacing: Theme.spaceSm
        }
    }
}
