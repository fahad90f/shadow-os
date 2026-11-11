// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.networkcenter

// Chooses one existing file. A small ShadowUI dialog rather than
// QtQuick.Dialogs' FileDialog, whose built-in fallback is not covered by the
// SHADOW style.
Dialog {
    id: root

    property var nameFilters: ["*"]             // e.g. ["*.conf", "*.ovpn"]
    property string filterLabel: ""             // what the filters mean, for the caption
    property string folder: NetUtil.homePath()
    property bool allFiles: false
    property string selected: ""                // path of the highlighted file
    readonly property var entries: NetUtil.listDirectory(folder, nameFilters, allFiles)

    signal fileChosen(string path)

    function openFolder(path) {
        if (!NetUtil.isDirectory(path))
            return false
        folder = path
        selected = ""
        list.currentIndex = -1
        pathField.text = path
        return true
    }
    function choose(path) {
        if (path === "" || NetUtil.isDirectory(path))
            return
        root.accept()
        root.fileChosen(path)
    }

    title: qsTr("Choose a file")
    width: Math.min(640, parent ? parent.width - Theme.spaceXxl : 640)
    height: Math.min(520, parent ? parent.height - Theme.spaceXxl : 520)
    closePolicy: Popup.CloseOnEscape
    footer: null
    onOpened: {
        pathField.text = folder
        selected = ""
        list.currentIndex = -1
        list.forceActiveFocus()
    }

    contentItem: ColumnLayout {
        spacing: Theme.spaceMd

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceSm
            NxIconButton {
                iconName: "arrow-up"
                tooltip: qsTr("Parent folder")
                enabled: root.folder !== "/"
                onClicked: root.openFolder(NetUtil.parentPath(root.folder))
            }
            NxIconButton {
                iconName: "home"
                tooltip: qsTr("Home folder")
                onClicked: root.openFolder(NetUtil.homePath())
            }
            TextField {
                id: pathField
                Layout.fillWidth: true
                font.family: Theme.fontMono
                placeholderText: qsTr("Folder or file path")
                Accessible.name: qsTr("Folder or file path")
                // A typed folder is opened; a typed file is chosen.
                onAccepted: {
                    const path = text.trim()
                    if (!root.openFolder(path))
                        root.choose(path)
                }
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
                id: list
                anchors.fill: parent
                anchors.margins: Theme.spaceXs
                model: root.entries
                currentIndex: -1
                boundsBehavior: Flickable.StopAtBounds
                activeFocusOnTab: true
                keyNavigationEnabled: true
                highlightMoveDuration: 0
                ScrollBar.vertical: ScrollBar { }
                Accessible.name: qsTr("Files in %1").arg(root.folder)

                onCurrentIndexChanged: {
                    const entry = currentIndex >= 0 ? root.entries[currentIndex] : null
                    root.selected = entry && !entry.directory ? entry.path : ""
                }
                Keys.onReturnPressed: {
                    const entry = currentIndex >= 0 ? root.entries[currentIndex] : null
                    if (!entry) return
                    if (entry.directory) root.openFolder(entry.path)
                    else root.choose(entry.path)
                }
                Keys.onPressed: (event) => {
                    if (event.key === Qt.Key_Backspace) {
                        root.openFolder(NetUtil.parentPath(root.folder))
                        event.accepted = true
                    }
                }

                delegate: Rectangle {
                    id: entryItem
                    required property var modelData
                    required property int index
                    readonly property bool current: ListView.isCurrentItem
                    width: list.width
                    height: 34
                    radius: Theme.radiusXs
                    color: current ? Theme.accentFill : hover.hovered ? Theme.hoverOverlay : "transparent"
                    Accessible.role: Accessible.ListItem
                    Accessible.name: modelData.name

                    HoverHandler { id: hover }
                    TapHandler {
                        onTapped: { list.currentIndex = entryItem.index; list.forceActiveFocus() }
                        onDoubleTapped: entryItem.modelData.directory ? root.openFolder(entryItem.modelData.path)
                                                                      : root.choose(entryItem.modelData.path)
                    }
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spaceSm
                        anchors.rightMargin: Theme.spaceMd
                        spacing: Theme.spaceSm
                        NxIcon {
                            name: entryItem.modelData.directory ? "folder" : "file-text"
                            size: Theme.iconSmall
                            color: entryItem.modelData.directory ? Theme.accent : Theme.textSecondary
                        }
                        Label {
                            Layout.fillWidth: true
                            text: entryItem.modelData.name
                            elide: Text.ElideMiddle
                        }
                        Label {
                            visible: !entryItem.modelData.directory
                            text: Theme.formatBytes(entryItem.modelData.size)
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSizeSmall
                        }
                    }
                }

                Label {
                    anchors.centerIn: parent
                    visible: list.count === 0
                    text: root.allFiles ? qsTr("This folder is empty or cannot be read")
                                        : qsTr("No folders or matching files here")
                    color: Theme.textMuted
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceSm
            CheckBox {
                text: root.filterLabel !== "" ? qsTr("All files (not only %1)").arg(root.filterLabel) : qsTr("All files, including hidden ones")
                checked: root.allFiles
                onToggled: root.allFiles = checked
            }
            Item { Layout.fillWidth: true }
            NxButton {
                text: qsTr("Cancel")
                onClicked: root.reject()
            }
            NxButton {
                text: qsTr("Open")
                kind: "primary"
                enabled: root.selected !== ""
                onClicked: root.choose(root.selected)
            }
        }
    }
}
