// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui

// What the assistant changed on disk, newest first, with an undo for each
// file action. The assistant runs without asking, so this is the way back.
NxPage {
    id: page
    property var controller
    title: qsTr("History & undo")
    subtitle: qsTr("Files the assistant created, changed, moved or deleted")
    maxContentWidth: 820

    function reload() {
        actionModel.clear();
        if (!controller) return;
        const entries = controller.actionLog();
        for (let i = 0; i < entries.length; ++i)
            actionModel.append(entries[i]);
    }

    actions: [
        NxButton {
            text: qsTr("Refresh")
            iconName: "refresh"
            onClicked: page.reload()
        }
    ]

    Component.onCompleted: reload()
    Connections {
        target: controller
        function onActionLogChanged() { page.reload() }
    }

    ListModel { id: actionModel }

    NxEmptyState {
        anchors.centerIn: parent
        visible: actionModel.count === 0
        icon: "undo"
        title: qsTr("Nothing to undo yet")
        message: qsTr("When the assistant writes, moves or deletes a file, it shows up here and you can undo it.")
    }

    ListView {
        anchors.fill: parent
        visible: actionModel.count > 0
        model: actionModel
        spacing: Theme.spaceSm
        clip: true

        delegate: NxCard {
            width: ListView.view.width
            RowLayout {
                anchors.fill: parent
                anchors.margins: Theme.spaceMd
                spacing: Theme.spaceMd

                NxIcon {
                    name: model.op === "delete_path" ? "trash"
                        : model.op === "move_path" ? "arrow-right"
                        : model.op === "copy_path" ? "copy"
                        : model.op === "make_directory" ? "folder"
                        : "file-text"
                    size: Theme.iconLarge
                    color: model.op === "delete_path" ? Theme.danger : Theme.accent
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2
                    Text {
                        Layout.fillWidth: true
                        text: model.summary
                        color: Theme.textPrimary
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeBody
                        wrapMode: Text.Wrap
                    }
                    Text {
                        Layout.fillWidth: true
                        text: model.time + "  ·  " + model.op
                        color: Theme.textMuted
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fontSizeCaption
                        elide: Text.ElideRight
                    }
                }

                NxBadge {
                    visible: model.undone === true
                    text: qsTr("undone")
                    kind: "neutral"
                }
                NxButton {
                    visible: model.undone !== true
                    text: qsTr("Undo")
                    iconName: "undo"
                    onClicked: controller.undoAction(model.id)
                }
            }
        }
    }
}
