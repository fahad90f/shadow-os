// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui

// File chooser in the NEXUS style (QtQuick.Dialogs' built-in fallback is not
// covered by it).
//   mode "open":   pick an existing file — or several files/folders with `multiple`
//   mode "save":   pick a folder and type a name; replacing a file is spelled out
//   mode "folder": pick a folder
// `fs` is the object that lists directories (listDirectory, isDirectory,
// exists, parentPath, homePath, joinPath).
Dialog {
    id: root

    required property var fs
    property string mode: "open"
    property bool multiple: false
    property bool allowFolders: false       // with `multiple`: folders can be ticked too
    property string folder: ""
    property string suggestedName: ""
    property string acceptText: ""
    property bool showHidden: false
    property var entries: []
    property var checked: ({})              // path → true (multiple selection)
    property int checkedCount: 0
    readonly property string typedPath: nameField.text.trim() === "" ? "" : fs.joinPath(folder, nameField.text.trim())
    readonly property bool replaces: mode === "save" && typedPath !== "" && fs.exists(typedPath) && !fs.isDirectory(typedPath)

    signal chosen(var paths)                // always an array

    function reload() {
        entries = fs.listDirectory(folder, showHidden)
        list.currentIndex = -1
    }
    function openFolder(path) {
        if (!fs.isDirectory(path))
            return false
        folder = path
        pathField.text = path
        reload()
        return true
    }
    function finish(paths) {
        if (!paths || paths.length === 0)
            return
        root.accept()
        root.chosen(paths)
    }
    function toggle(path) {
        const copy = Object.assign({}, checked)
        if (copy[path]) delete copy[path]
        else copy[path] = true
        checked = copy
        checkedCount = Object.keys(copy).length
    }
    function activate(entry) {
        if (entry.directory) openFolder(entry.path)
        else if (mode === "open" && !multiple) finish([entry.path])
        else if (mode === "open") toggle(entry.path)
        else if (mode === "save") nameField.text = entry.name
    }
    function acceptCurrent() {
        if (mode === "folder") { finish([folder]); return }
        if (mode === "save") {
            if (typedPath !== "" && !fs.isDirectory(typedPath)) finish([typedPath])
            return
        }
        if (multiple) { finish(Object.keys(checked)); return }
        const entry = list.currentIndex >= 0 ? entries[list.currentIndex] : null
        if (entry && !entry.directory) finish([entry.path])
    }

    title: mode === "save" ? qsTr("Save as") : mode === "folder" ? qsTr("Choose a folder") : qsTr("Open")
    width: Math.min(680, parent ? parent.width - Theme.spaceXxl : 680)
    height: Math.min(560, parent ? parent.height - Theme.spaceXxl : 560)
    closePolicy: Popup.CloseOnEscape
    footer: null

    onAboutToShow: {
        if (folder === "" || !fs.isDirectory(folder))
            folder = fs.homePath()
        pathField.text = folder
        nameField.text = suggestedName
        checked = ({})
        checkedCount = 0
        reload()
    }
    onOpened: mode === "save" ? nameField.forceActiveFocus() : list.forceActiveFocus()
    onShowHiddenChanged: if (visible) reload()

    contentItem: ColumnLayout {
        spacing: Theme.spaceMd

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceSm
            NxIconButton {
                iconName: "arrow-up"
                tooltip: qsTr("Parent folder")
                enabled: root.folder !== "/"
                onClicked: root.openFolder(root.fs.parentPath(root.folder))
            }
            NxIconButton {
                iconName: "home"
                tooltip: qsTr("Home folder")
                onClicked: root.openFolder(root.fs.homePath())
            }
            TextField {
                id: pathField
                Layout.fillWidth: true
                font.family: Theme.fontMono
                placeholderText: qsTr("Folder path")
                Accessible.name: qsTr("Folder path")
                onAccepted: {
                    const path = text.trim()
                    if (root.openFolder(path))
                        return
                    if (root.mode === "open" && !root.multiple && root.fs.exists(path))
                        root.finish([path])
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
                Accessible.name: qsTr("Contents of %1").arg(root.folder)

                Keys.onReturnPressed: if (currentIndex >= 0) root.activate(root.entries[currentIndex])
                Keys.onSpacePressed: {
                    const entry = currentIndex >= 0 ? root.entries[currentIndex] : null
                    if (entry && root.multiple && (!entry.directory || root.allowFolders)) root.toggle(entry.path)
                }
                Keys.onPressed: (event) => {
                    if (event.key === Qt.Key_Backspace) {
                        root.openFolder(root.fs.parentPath(root.folder))
                        event.accepted = true
                    }
                }

                delegate: Rectangle {
                    id: entryItem
                    required property var modelData
                    required property int index
                    readonly property bool current: ListView.isCurrentItem
                    readonly property bool dimmed: root.mode === "folder" && !modelData.directory
                    width: list.width
                    height: Theme.controlHeight
                    radius: Theme.radiusXs
                    color: current ? Theme.accentFill : hover.hovered ? Theme.hoverOverlay : "transparent"
                    Accessible.role: Accessible.ListItem
                    Accessible.name: modelData.name

                    HoverHandler { id: hover }
                    TapHandler {
                        onTapped: { list.currentIndex = entryItem.index; list.forceActiveFocus() }
                        onDoubleTapped: root.activate(entryItem.modelData)
                    }
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spaceSm
                        anchors.rightMargin: Theme.spaceMd
                        spacing: Theme.spaceSm
                        CheckBox {
                            visible: root.multiple && (!entryItem.modelData.directory || root.allowFolders)
                            checked: root.checked[entryItem.modelData.path] === true
                            Accessible.name: qsTr("Select %1").arg(entryItem.modelData.name)
                            onToggled: root.toggle(entryItem.modelData.path)
                        }
                        NxIcon {
                            name: entryItem.modelData.directory ? "folder" : "file-text"
                            size: Theme.iconSmall
                            color: entryItem.modelData.directory ? Theme.accent : Theme.textSecondary
                            opacity: entryItem.dimmed ? Theme.opacityDisabled : 1
                        }
                        Label {
                            Layout.fillWidth: true
                            text: entryItem.modelData.name
                            elide: Text.ElideMiddle
                            opacity: entryItem.dimmed ? Theme.opacityDisabled : 1
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
                    text: qsTr("This folder is empty or cannot be read")
                    color: Theme.textMuted
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: root.mode === "save"
            spacing: Theme.spaceSm
            Label { text: qsTr("Name") }
            TextField {
                id: nameField
                Layout.fillWidth: true
                placeholderText: qsTr("File name")
                Accessible.name: qsTr("File name")
                onAccepted: if (!root.replaces) root.acceptCurrent()
            }
        }
        Label {
            Layout.fillWidth: true
            visible: root.replaces
            text: qsTr("“%1” already exists here and will be replaced.").arg(nameField.text.trim())
            color: Theme.warning
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceSm
            CheckBox {
                text: qsTr("Show hidden files")
                checked: root.showHidden
                onToggled: root.showHidden = checked
            }
            Label {
                visible: root.multiple
                text: qsTr("%n selected", "", root.checkedCount)
                color: Theme.textMuted
            }
            Item { Layout.fillWidth: true }
            NxButton {
                text: qsTr("Cancel")
                onClicked: root.reject()
            }
            NxButton {
                text: root.acceptText !== "" ? root.acceptText
                    : root.replaces ? qsTr("Replace")
                    : root.mode === "save" ? qsTr("Save")
                    : root.mode === "folder" ? qsTr("Choose this folder") : qsTr("Open")
                kind: root.replaces ? "danger" : "primary"
                enabled: root.mode === "folder" ? true
                    : root.mode === "save" ? (root.typedPath !== "" && !root.fs.isDirectory(root.typedPath))
                    : root.multiple ? root.checkedCount > 0
                    : (list.currentIndex >= 0 && !root.entries[list.currentIndex].directory)
                onClicked: root.acceptCurrent()
            }
        }
    }
}
