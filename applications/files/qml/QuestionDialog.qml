// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.files

// Questions asked by running KIO jobs (see Interaction): name conflicts,
// errors that can be skipped or retried, confirmations and message boxes of
// KIO workers. Shows Interaction.current; every button answers it.
Dialog {
    id: dialog

    readonly property var request: Interaction.current
    readonly property string kind: request.kind ?? ""
    readonly property bool multiple: request.multiple === true

    function answer(action) {
        Interaction.answer(request.id, action, applyToAll.checked, newName.text.trim())
    }

    title: {
        if (kind === "conflict") {
            if (request.sameFile) return qsTr("The item would be copied onto itself")
            return request.destIsDir ? qsTr("A folder with this name already exists") : qsTr("A file with this name already exists")
        }
        if (kind === "skip") return qsTr("An item could not be processed")
        if (kind === "delete") {
            return request.mode === "trash" ? qsTr("Move to the Trash?")
                : request.mode === "emptyTrash" ? qsTr("Empty the Trash?") : qsTr("Delete permanently?")
        }
        return request.title ?? ""
    }
    width: Math.min(540, parent ? parent.width - Theme.spaceXxl : 540)
    closePolicy: Popup.NoAutoClose
    footer: null
    visible: Interaction.pending

    // A new question starts with clean controls.
    onRequestChanged: {
        applyToAll.checked = false
        renaming.visible = false
        newName.text = request.suggestedName ?? ""
    }

    contentItem: ColumnLayout {
        spacing: Theme.spaceMd

        // ---- name conflict ----
        ColumnLayout {
            Layout.fillWidth: true
            visible: dialog.kind === "conflict"
            spacing: Theme.spaceMd

            Text {
                Layout.fillWidth: true
                text: dialog.request.sameFile
                    ? qsTr("“%1” is already in “%2”. Keep both to create a copy with a new name.")
                          .arg(dialog.request.destName ?? "").arg(dialog.request.destDir ?? "")
                    : qsTr("“%1” already exists in “%2”.").arg(dialog.request.destName ?? "").arg(dialog.request.destDir ?? "")
                color: Theme.textSecondary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                visible: !dialog.request.sameFile
                spacing: Theme.spaceMd
                Repeater {
                    model: [
                        { title: qsTr("Existing"), size: dialog.request.destSize ?? -1, time: dialog.request.destTime ?? "" },
                        { title: qsTr("Incoming"), size: dialog.request.sourceSize ?? -1, time: dialog.request.sourceTime ?? "" },
                    ]
                    NxCard {
                        id: side
                        required property var modelData
                        Layout.fillWidth: true
                        implicitHeight: sideColumn.implicitHeight + padding * 2
                        padding: Theme.spaceMd
                        ColumnLayout {
                            id: sideColumn
                            anchors.left: parent.left
                            anchors.right: parent.right
                            spacing: 2
                            Text {
                                text: side.modelData.title
                                color: Theme.textMuted
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fontSizeCaption
                                font.weight: Theme.weightSemibold
                                font.capitalization: Font.AllUppercase
                            }
                            Text {
                                Layout.fillWidth: true
                                text: side.modelData.size >= 0 ? Theme.formatBytes(side.modelData.size) : qsTr("Size unknown")
                                color: Theme.textPrimary
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fontSizeBody
                                font.weight: Theme.weightMedium
                            }
                            Text {
                                Layout.fillWidth: true
                                text: side.modelData.time !== "" ? qsTr("Modified %1").arg(side.modelData.time) : qsTr("Date unknown")
                                color: Theme.textSecondary
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fontSizeSmall
                                elide: Text.ElideRight
                            }
                        }
                    }
                }
            }

            RowLayout {
                id: renaming
                Layout.fillWidth: true
                visible: false
                spacing: Theme.spaceSm
                TextField {
                    id: newName
                    Layout.fillWidth: true
                    selectByMouse: true
                    Accessible.name: qsTr("New name")
                    onAccepted: if (renameButton.enabled) renameButton.clicked()
                }
                NxButton {
                    id: renameButton
                    text: qsTr("Save with this name")
                    kind: "primary"
                    enabled: newName.text.trim() !== "" && FileOps.validateName(newName.text) === ""
                        && newName.text.trim() !== (dialog.request.destName ?? "")
                    onClicked: dialog.answer("rename")
                }
            }
        }

        // ---- skip / retry ----
        Text {
            Layout.fillWidth: true
            visible: dialog.kind === "skip"
            text: dialog.request.message ?? ""
            color: Theme.textSecondary
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeBody
            wrapMode: Text.WordWrap
        }

        // ---- delete confirmation (undo of a copy, …) ----
        ColumnLayout {
            Layout.fillWidth: true
            visible: dialog.kind === "delete"
            spacing: Theme.spaceXs
            Text {
                Layout.fillWidth: true
                text: dialog.request.mode === "trash" ? qsTr("These items will be moved to the Trash:")
                    : dialog.request.mode === "emptyTrash" ? qsTr("Everything in the Trash will be deleted permanently.")
                    : qsTr("These items will be deleted permanently and cannot be restored:")
                color: Theme.textSecondary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
                wrapMode: Text.WordWrap
            }
            Repeater {
                model: dialog.kind === "delete" ? (dialog.request.names ?? []) : []
                Text {
                    required property string modelData
                    Layout.fillWidth: true
                    text: "•  " + modelData
                    color: Theme.textPrimary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBody
                    elide: Text.ElideMiddle
                }
            }
            Text {
                Layout.fillWidth: true
                visible: dialog.kind === "delete" && (dialog.request.count ?? 0) > (dialog.request.names ?? []).length
                text: qsTr("… and %1 more").arg((dialog.request.count ?? 0) - (dialog.request.names ?? []).length)
                color: Theme.textMuted
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
            }
        }

        // ---- message box of a KIO worker ----
        ColumnLayout {
            Layout.fillWidth: true
            visible: dialog.kind === "message"
            spacing: Theme.spaceSm
            Text {
                Layout.fillWidth: true
                text: dialog.request.text ?? ""
                textFormat: Text.PlainText
                color: Theme.textSecondary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                visible: text !== ""
                text: dialog.request.details ?? ""
                textFormat: Text.PlainText
                color: Theme.textMuted
                font.family: Theme.fontMono
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WrapAnywhere
            }
        }

        CheckBox {
            id: applyToAll
            visible: (dialog.kind === "conflict" || dialog.kind === "skip") && dialog.multiple
            text: qsTr("Do this for all remaining items")
        }

        // ---- buttons ----
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Theme.spaceXs
            spacing: Theme.spaceSm

            NxButton {
                visible: dialog.kind !== "message" || dialog.request.hasCancel === true
                text: dialog.kind === "conflict" || dialog.kind === "skip" ? qsTr("Stop") : qsTr("Cancel")
                onClicked: dialog.answer("cancel")
            }
            Item { Layout.fillWidth: true }

            // conflict
            NxButton {
                visible: dialog.kind === "conflict" && (dialog.multiple || dialog.request.canSkip === true)
                text: qsTr("Skip")
                onClicked: dialog.answer("skip")
            }
            NxButton {
                visible: dialog.kind === "conflict" && dialog.request.canRename === true && !applyToAll.checked
                text: qsTr("Rename…")
                onClicked: {
                    renaming.visible = !renaming.visible
                    if (renaming.visible) { newName.forceActiveFocus(); newName.selectAll() }
                }
            }
            NxButton {
                visible: dialog.kind === "conflict" && dialog.request.canRename === true
                text: qsTr("Keep both")
                kind: dialog.request.canOverwrite === true ? "secondary" : "primary"
                onClicked: dialog.answer("keepBoth")
            }
            NxButton {
                visible: dialog.kind === "conflict" && dialog.request.canOverwrite === true
                text: dialog.request.destIsDir ? qsTr("Merge") : qsTr("Replace")
                kind: "danger"
                onClicked: dialog.answer("overwrite")
            }

            // skip
            NxButton {
                visible: dialog.kind === "skip" && dialog.request.canRetry === true
                text: qsTr("Retry")
                onClicked: dialog.answer("retry")
            }
            NxButton {
                visible: dialog.kind === "skip"
                text: qsTr("Skip")
                kind: "primary"
                onClicked: dialog.answer("skip")
            }

            // delete
            NxButton {
                visible: dialog.kind === "delete"
                text: dialog.request.mode === "trash" ? qsTr("Move to Trash") : qsTr("Delete")
                kind: "danger"
                onClicked: dialog.answer("confirm")
            }

            // message
            NxButton {
                visible: dialog.kind === "message" && (dialog.request.secondaryText ?? "") !== ""
                text: dialog.request.secondaryText ?? ""
                onClicked: dialog.answer("secondary")
            }
            NxButton {
                visible: dialog.kind === "message"
                text: dialog.request.primaryText ?? ""
                kind: dialog.request.tone === "danger" ? "secondary" : "primary"
                onClicked: dialog.answer("primary")
            }
        }
    }
}
