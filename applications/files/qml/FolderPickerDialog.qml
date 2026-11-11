// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.files

// Chooses a destination folder ("Extract to…", "Copy to…", "Move to…").
// A small folder browser on its own FolderModel: sub-folders only.
Dialog {
    id: dialog

    property string confirmText: qsTr("Choose")
    // function(url)
    property var handler: null

    function show(title, confirmText, startUrl, handler) {
        dialog.title = title
        dialog.confirmText = confirmText
        dialog.handler = handler
        folders.open(startUrl)
        open()
    }

    width: Math.min(560, parent ? parent.width - Theme.spaceXxl : 560)
    height: Math.min(520, parent ? parent.height - Theme.spaceXxl : 520)
    closePolicy: Popup.CloseOnEscape
    footer: null
    onOpened: view.forceActiveFocus()

    FolderModel {
        id: folders
        foldersOnly: true
        showHidden: false
        onUrlChanged: view.currentIndex = -1
    }

    contentItem: ColumnLayout {
        spacing: Theme.spaceSm

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceXs
            NxIconButton {
                iconName: "arrow-up"
                tooltip: qsTr("Parent folder")
                enabled: folders.canGoUp
                onClicked: folders.goUp()
            }
            NxIconButton {
                iconName: "home"
                tooltip: qsTr("Home")
                onClicked: folders.open(Places.homeUrl)
            }
            TextField {
                id: pathField
                Layout.fillWidth: true
                text: folders.location
                selectByMouse: true
                font.family: Theme.fontMono
                font.pixelSize: Theme.fontSizeSmall
                Accessible.name: qsTr("Folder")
                onAccepted: folders.openLocation(text)
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: Theme.radiusSm
            color: Theme.bgInput
            border.width: Theme.borderWidth
            border.color: Theme.border
            clip: true

            ListView {
                id: view
                anchors.fill: parent
                anchors.margins: Theme.borderWidth
                model: folders
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                activeFocusOnTab: true
                ScrollBar.vertical: ScrollBar { }
                Accessible.role: Accessible.List
                Accessible.name: qsTr("Sub-folders")
                Keys.onReturnPressed: if (currentIndex >= 0) folders.open(folders.urlAt(currentIndex))
                Keys.onPressed: (event) => {
                    if (event.key === Qt.Key_Backspace) { folders.goUp(); event.accepted = true }
                }

                delegate: Rectangle {
                    id: row
                    required property int index
                    required property string name
                    required property url url
                    required property string iconName
                    width: view.width
                    height: Theme.controlHeight
                    radius: Theme.radiusXs
                    color: view.currentIndex === index ? Theme.accentFill
                        : rowMouse.containsMouse ? Theme.hoverOverlay : "transparent"
                    Accessible.role: Accessible.ListItem
                    Accessible.name: name

                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: { view.currentIndex = row.index; view.forceActiveFocus() }
                        onDoubleClicked: folders.open(row.url)
                    }
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spaceMd
                        anchors.rightMargin: Theme.spaceMd
                        spacing: Theme.spaceSm
                        FileIcon {
                            name: row.iconName
                            size: Theme.icon
                        }
                        Text {
                            Layout.fillWidth: true
                            text: row.name
                            color: Theme.textPrimary
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeBody
                            elide: Text.ElideMiddle
                        }
                    }
                }
            }

            BusyIndicator {
                anchors.centerIn: parent
                running: visible
                visible: folders.loading && folders.count === 0
            }
            Text {
                anchors.centerIn: parent
                width: parent.width - Theme.spaceXl * 2
                visible: !folders.loading && folders.count === 0
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: folders.error !== "" ? folders.error + "\n" + folders.errorDetail : qsTr("This folder has no sub-folders.")
                color: folders.error !== "" ? Theme.danger : Theme.textMuted
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
            }
        }

        Text {
            Layout.fillWidth: true
            text: !folders.writable && folders.error === "" ? qsTr("You cannot write to this folder.")
                : view.currentIndex >= 0 ? qsTr("Destination: %1").arg(FileOps.displayPath(folders.urlAt(view.currentIndex)))
                : qsTr("Destination: %1").arg(folders.location)
            color: !folders.writable && folders.error === "" ? Theme.warning : Theme.textSecondary
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
            elide: Text.ElideMiddle
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceSm
            NxButton {
                text: qsTr("New folder")
                iconName: "plus"
                enabled: folders.writable
                onClicked: newFolderName.visible = !newFolderName.visible
            }
            TextField {
                id: newFolderName
                Layout.fillWidth: true
                visible: false
                placeholderText: qsTr("Folder name, then Enter")
                Accessible.name: qsTr("New folder name")
                onVisibleChanged: if (visible) { text = ""; forceActiveFocus() }
                onAccepted: {
                    if (FileOps.validateName(text) === "" && !folders.nameExists(text.trim())) {
                        FileOps.createFolder(folders.url, text)
                        visible = false
                    }
                }
                Keys.onEscapePressed: visible = false
            }
            Item { Layout.fillWidth: true; visible: !newFolderName.visible }
            NxButton {
                text: qsTr("Cancel")
                onClicked: dialog.close()
            }
            NxButton {
                text: dialog.confirmText
                kind: "primary"
                enabled: folders.error === "" && (view.currentIndex >= 0 || folders.writable)
                onClicked: {
                    const target = view.currentIndex >= 0 ? folders.urlAt(view.currentIndex) : folders.url
                    const fn = dialog.handler
                    dialog.close()
                    if (fn) fn(target)
                }
            }
        }
    }
}
