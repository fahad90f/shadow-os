// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.archivemanager

// NEXUS Archive Manager — main window: toolbar, the contents of the open
// archive, progress bar and the dialogs for extracting and creating.
NxWindow {
    id: window

    title: session.isOpen ? qsTr("%1 — Archive Manager").arg(session.archiveName) : qsTr("Archive Manager")
    width: 1080
    height: 720

    property var selected: ({})             // tree path → true
    property int selectedCount: 0
    property bool batch: launch.mode !== "browse"
    property bool batchStarted: false
    readonly property var selectedPaths: Object.keys(selected)

    QtObject {
        id: prefs
        property string lastFolder: ""
        property string lastDestination: ""
        property string conflict: "rename"
        property string createFormat: "zip"
        property string createLevel: "normal"
    }
    Settings {
        category: "ArchiveManager"
        property alias lastFolder: prefs.lastFolder
        property alias lastDestination: prefs.lastDestination
        property alias conflict: prefs.conflict
        property alias createFormat: prefs.createFormat
        property alias createLevel: prefs.createLevel
    }

    function clearSelection() { selected = ({}); selectedCount = 0 }
    function toggleSelected(path) {
        const copy = Object.assign({}, selected)
        if (copy[path]) delete copy[path]
        else copy[path] = true
        selected = copy
        selectedCount = Object.keys(copy).length
    }
    function selectAll(on) {
        const copy = {}
        if (on) for (const entry of session.entries) copy[entry.path] = true
        selected = copy
        selectedCount = Object.keys(copy).length
    }
    function openArchive(path) {
        prefs.lastFolder = ArchiveTools.parentPath(path)
        session.open(path)
    }
    function showExtract(selection) {
        extractDialog.selection = selection
        extractDialog.destination = prefs.lastDestination !== "" && ArchiveTools.isDirectory(prefs.lastDestination)
            ? prefs.lastDestination : ArchiveTools.parentPath(session.archivePath)
        extractDialog.subfolder = selection.length === 0 && session.rootItemCount !== 1
        extractDialog.acknowledged = false
        extractDialog.open()
    }
    function showCreate(inputs, output) {
        createDialog.setInputs(inputs)
        if (output && output !== "") {
            createDialog.folder = ArchiveTools.parentPath(output)
            const format = launch.format !== "" ? launch.format : ArchiveTools.formatForName(ArchiveTools.fileName(output))
            if (format !== "") createDialog.selectFormat(format)
            createDialog.name = ArchiveTools.baseName(ArchiveTools.fileName(output))
        } else {
            createDialog.folder = inputs.length > 0 ? ArchiveTools.parentPath(inputs[0])
                : prefs.lastFolder !== "" ? prefs.lastFolder : ArchiveTools.homePath()
            createDialog.selectFormat(prefs.createFormat)
            createDialog.name = ArchiveTools.suggestedName(inputs)
        }
        createDialog.open()
    }
    function activate(entry) {
        if (entry.directory) session.enter(entry.path)
        else session.preview(entry.path)
    }
    function showResult(title, ok, message, details) {
        resultDialog.title = title
        resultDialog.ok = ok
        resultDialog.message = message
        resultDialog.details = details
        resultDialog.open()
    }

    ArchiveSession {
        id: session
        onCurrentDirChanged: window.clearSelection()
        onEntriesChanged: window.clearSelection()
        onPasswordRequired: (wrong) => { passwordDialog.wrong = wrong; passwordDialog.open() }
        onListed: {
            if (launch.mode !== "extract" || window.batchStarted) return
            window.batchStarted = true
            if (launch.destination !== "") {
                const estimate = session.extractionEstimate([], launch.destination)
                if (estimate.enough && estimate.writable && !estimate.suspicious) {
                    session.extract(launch.destination, [], { conflict: launch.conflict })
                    return
                }
                window.showExtract([])
                extractDialog.destination = launch.destination
                extractDialog.subfolder = false
            } else {
                window.showExtract([])
            }
        }
        onFinished: (kind, ok, message, details) => {
            if (kind === "list") {
                if (!ok) window.showResult(qsTr("The archive could not be opened"), false, message, details)
                return
            }
            if (kind === "create" && ok) {
                window.toaster.show(message, "success")
                if (window.batch && launch.mode === "create") { quitTimer.start(); return }
                window.openArchive(createDialog.output)
                return
            }
            const titles = {
                extract: ok ? qsTr("Extraction finished") : qsTr("Extraction failed"),
                test: ok ? qsTr("The archive is intact") : qsTr("The archive has errors"),
                add: ok ? qsTr("Files added") : qsTr("Adding files failed"),
                "delete": ok ? qsTr("Entries deleted") : qsTr("Deleting failed"),
                preview: ok ? qsTr("Preview") : qsTr("The file was not opened"),
                create: qsTr("The archive was not created"),
            }
            if (ok && details.length === 0 && kind !== "test") {
                window.toaster.show(message, "success")
                if (kind === "extract" && launch.mode === "extract") quitTimer.start()
            } else {
                window.showResult(titles[kind], ok, message, details)
            }
        }
    }
    // Batch runs started by the file manager close on success.
    Timer { id: quitTimer; interval: 1500; onTriggered: Qt.quit() }

    Component.onCompleted: {
        if (launch.mode === "create") window.showCreate(launch.inputs, launch.archive)
        else if (launch.archive !== "") window.openArchive(launch.archive)
    }

    Shortcut { sequences: [StandardKey.Open]; onActivated: openPicker.show() }
    Shortcut { sequences: [StandardKey.New]; onActivated: window.showCreate([], "") }
    Shortcut { sequence: "Ctrl+E"; enabled: session.isOpen && !session.busy; onActivated: window.showExtract(window.selectedPaths) }
    Shortcut { sequence: "Ctrl+A"; enabled: session.isOpen; onActivated: window.selectAll(true) }
    Shortcut { sequence: "Ctrl+Q"; onActivated: window.close() }

    DropArea {
        anchors.fill: parent
        onDropped: (drop) => {
            if (!drop.hasUrls || session.busy) return
            const paths = []
            for (const url of drop.urls) {
                const path = ArchiveTools.localPath(url)
                if (path !== "") paths.push(path)
            }
            if (paths.length === 0) {
                window.toaster.show(qsTr("Only local files can be dropped here"), "warning")
                return
            }
            drop.accept()
            if (paths.length === 1 && ArchiveTools.looksLikeArchive(paths[0]) && !session.isOpen) {
                window.openArchive(paths[0])
            } else if (session.isOpen && session.canModify) {
                addConfirm.paths = paths
                addConfirm.open()
            } else if (session.isOpen) {
                window.toaster.show(session.readOnlyReason, "warning")
            } else {
                window.showCreate(paths, "")
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---- toolbar ---------------------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.headerHeight
            color: Theme.bgSurface

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.spaceMd
                anchors.rightMargin: Theme.spaceMd
                spacing: Theme.spaceXs

                NxIconButton { iconName: "folder-open"; tooltip: qsTr("Open archive… (Ctrl+O)"); enabled: !session.busy; onClicked: openPicker.show() }
                NxIconButton { iconName: "plus"; tooltip: qsTr("New archive… (Ctrl+N)"); enabled: !session.busy; onClicked: window.showCreate([], "") }
                NxDivider { vertical: true; Layout.preferredHeight: Theme.icon; Layout.margins: Theme.spaceXs }
                NxButton {
                    text: window.selectedCount > 0 ? qsTr("Extract selected…") : qsTr("Extract all…")
                    kind: "primary"
                    iconName: "download"
                    enabled: session.isOpen && !session.busy && session.entryCount > 0
                    onClicked: window.showExtract(window.selectedPaths)
                }
                NxIconButton {
                    iconName: "upload"
                    tooltip: session.isOpen && !session.canModify ? session.readOnlyReason : qsTr("Add files or folders…")
                    enabled: session.isOpen && session.canModify && !session.busy
                    onClicked: addPicker.show()
                }
                NxIconButton {
                    iconName: "trash"
                    tooltip: session.isOpen && !session.canModify ? session.readOnlyReason : qsTr("Delete selected entries from the archive")
                    enabled: session.isOpen && session.canModify && !session.busy && window.selectedCount > 0
                    onClicked: deleteConfirm.open()
                }
                NxIconButton {
                    iconName: "check-circle"
                    tooltip: qsTr("Test integrity")
                    enabled: session.isOpen && !session.busy
                    onClicked: session.test()
                }
                Item { Layout.fillWidth: true }
                NxBadge { visible: session.isOpen && session.encrypted; text: qsTr("Encrypted"); kind: "accent" }
                NxBadge { visible: session.isOpen; text: session.formatLabel; kind: "neutral"; dot: false }
            }
        }
        NxDivider { Layout.fillWidth: true }

        // ---- path ------------------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spaceMd
            Layout.rightMargin: Theme.spaceMd
            Layout.topMargin: Theme.spaceXs
            Layout.bottomMargin: Theme.spaceXs
            visible: session.isOpen
            spacing: Theme.spaceXxs

            NxIconButton {
                iconName: "arrow-up"
                tooltip: qsTr("Parent folder (Backspace)")
                enabled: session.currentDir !== ""
                onClicked: session.goUp()
            }
            ToolButton {
                text: session.archiveName
                icon.source: "image://nxicon/archive"
                onClicked: session.enter("")
            }
            Repeater {
                model: session.currentDir === "" ? [] : session.currentDir.split("/")
                RowLayout {
                    id: crumb
                    required property string modelData
                    required property int index
                    spacing: Theme.spaceXxs
                    NxIcon { name: "chevron-right"; size: Theme.iconSmall; color: Theme.textMuted }
                    ToolButton {
                        text: crumb.modelData
                        onClicked: session.enter(session.currentDir.split("/").slice(0, crumb.index + 1).join("/"))
                    }
                }
            }
            Item { Layout.fillWidth: true }
        }

        // ---- warnings --------------------------------------------------------------
        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spaceMd
            Layout.rightMargin: Theme.spaceMd
            Layout.bottomMargin: Theme.spaceSm
            spacing: Theme.spaceSm
            visible: session.isOpen && (session.hazardCount > 0 || session.bombSuspected || session.listWarning !== "")

            Notice {
                visible: session.bombSuspected
                kind: "danger"
                title: qsTr("Possible decompression bomb")
                message: qsTr("This %1 archive unpacks to %2 — %3 times its size. Extract it only if you expect that much data.")
                    .arg(Theme.formatBytes(session.archiveSize)).arg(Theme.formatBytes(session.totalSize))
                    .arg(session.archiveSize > 0 ? Math.round(session.totalSize / session.archiveSize) : 0)
            }
            Notice {
                visible: session.hazardCount > 0
                kind: "warning"
                title: qsTr("%n entries are not extracted as stored", "", session.hazardCount)
                message: qsTr("They use absolute paths, “..”, links that leave the archive, device nodes or set-user-ID bits. "
                              + "Nothing is ever written outside the folder you extract to; the marked entries are skipped or made safe.")
            }
            Notice {
                visible: session.listWarning !== ""
                kind: "warning"
                message: session.listWarning
            }
        }

        // ---- contents --------------------------------------------------------------
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            NxEmptyState {
                anchors.fill: parent
                visible: !session.isOpen && !session.busy
                icon: "archive"
                title: qsTr("No archive open")
                message: qsTr("Open a ZIP, 7-Zip, tar, CPIO, ISO or RAR file — or drop files here to pack them into a new archive.")
                actionText: qsTr("Open archive…")
                onActionTriggered: openPicker.show()
            }
            ColumnLayout {
                anchors.centerIn: parent
                visible: !session.isOpen && session.busy
                spacing: Theme.spaceMd
                BusyIndicator { Layout.alignment: Qt.AlignHCenter; running: parent.visible }
                Label { text: qsTr("Reading the archive…"); color: Theme.textSecondary }
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.spaceMd
                anchors.rightMargin: Theme.spaceMd
                visible: session.isOpen
                spacing: Theme.spaceXxs

                // header
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Theme.controlHeight
                    radius: Theme.radiusSm
                    color: Theme.bgSurface

                    component HeaderCell: Item {
                        id: cell
                        property string role: ""
                        property string label: ""
                        property bool alignRight: false
                        Layout.fillHeight: true
                        Accessible.role: Accessible.ColumnHeader
                        Accessible.name: label
                        Row {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: cell.alignRight ? undefined : parent.left
                            anchors.right: cell.alignRight ? parent.right : undefined
                            spacing: Theme.spaceXs
                            Label {
                                text: cell.label
                                color: table.sortRole === cell.role ? Theme.textPrimary : Theme.textMuted
                                font.pixelSize: Theme.fontSizeSmall
                                font.weight: Theme.weightSemibold
                            }
                            NxIcon {
                                visible: table.sortRole === cell.role
                                name: table.sortAscending ? "chevron-up" : "chevron-down"
                                size: Theme.iconSmall
                                color: Theme.accent
                            }
                        }
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                        TapHandler {
                            onTapped: {
                                table.sortAscending = table.sortRole === cell.role ? !table.sortAscending : true
                                table.sortRole = cell.role
                                session.sortBy(table.sortRole, table.sortAscending)
                            }
                        }
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spaceSm
                        anchors.rightMargin: Theme.spaceLg
                        spacing: Theme.spaceSm
                        CheckBox {
                            Accessible.name: qsTr("Select all")
                            checked: window.selectedCount > 0 && window.selectedCount === session.entries.length
                            onToggled: window.selectAll(checked)
                        }
                        HeaderCell { role: "name"; label: qsTr("Name"); Layout.fillWidth: true }
                        HeaderCell { role: "size"; label: qsTr("Size"); alignRight: true; Layout.preferredWidth: 96 }
                        Item {
                            Layout.preferredWidth: 96
                            Layout.fillHeight: true
                            Label {
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("Packed")
                                color: Theme.textMuted
                                font.pixelSize: Theme.fontSizeSmall
                                font.weight: Theme.weightSemibold
                            }
                        }
                        HeaderCell { role: "modified"; label: qsTr("Modified"); Layout.preferredWidth: 150; Layout.leftMargin: Theme.spaceMd }
                    }
                }

                ListView {
                    id: table
                    property string sortRole: "name"
                    property bool sortAscending: true
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: session.entries
                    boundsBehavior: Flickable.StopAtBounds
                    activeFocusOnTab: true
                    keyNavigationEnabled: true
                    highlightMoveDuration: 0
                    reuseItems: true
                    currentIndex: -1
                    ScrollBar.vertical: ScrollBar { }
                    Accessible.name: qsTr("Archive contents")

                    Keys.onReturnPressed: if (currentIndex >= 0) window.activate(session.entries[currentIndex])
                    Keys.onSpacePressed: if (currentIndex >= 0) window.toggleSelected(session.entries[currentIndex].path)
                    Keys.onPressed: (event) => {
                        if (event.key === Qt.Key_Backspace) { session.goUp(); event.accepted = true }
                        else if (event.key === Qt.Key_Delete && session.canModify && window.selectedCount > 0) {
                            deleteConfirm.open(); event.accepted = true
                        }
                    }

                    delegate: Rectangle {
                        id: rowItem
                        required property var modelData
                        required property int index
                        readonly property bool isSelected: window.selected[modelData.path] === true
                        width: table.width
                        height: Theme.controlHeight + Theme.spaceXxs
                        radius: Theme.radiusXs
                        color: isSelected ? Theme.accentFill
                            : (ListView.isCurrentItem && table.activeFocus) || rowHover.hovered ? Theme.hoverOverlay : "transparent"
                        Accessible.role: Accessible.ListItem
                        Accessible.name: modelData.name

                        HoverHandler { id: rowHover }
                        TapHandler {
                            onTapped: { table.currentIndex = rowItem.index; table.forceActiveFocus() }
                            onDoubleTapped: window.activate(rowItem.modelData)
                        }
                        TapHandler {
                            acceptedButtons: Qt.RightButton
                            onTapped: {
                                table.currentIndex = rowItem.index
                                rowMenu.entry = rowItem.modelData
                                rowMenu.popup()
                            }
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.spaceSm
                            anchors.rightMargin: Theme.spaceLg
                            spacing: Theme.spaceSm

                            CheckBox {
                                checked: rowItem.isSelected
                                Accessible.name: qsTr("Select %1").arg(rowItem.modelData.name)
                                onToggled: window.toggleSelected(rowItem.modelData.path)
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spaceSm
                                NxIcon {
                                    name: rowItem.modelData.directory ? "folder"
                                        : rowItem.modelData.kind === "symlink" || rowItem.modelData.kind === "hardlink" ? "link"
                                        : rowItem.modelData.kind === "file" ? "file" : "alert-triangle"
                                    size: Theme.iconSmall
                                    color: rowItem.modelData.directory ? Theme.accent : Theme.textSecondary
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: rowItem.modelData.link !== ""
                                        ? qsTr("%1  →  %2").arg(rowItem.modelData.name).arg(rowItem.modelData.link) : rowItem.modelData.name
                                    elide: Text.ElideMiddle
                                }
                                NxIcon {
                                    visible: rowItem.modelData.encrypted
                                    name: "lock"
                                    size: Theme.iconSmall
                                    color: Theme.textMuted
                                }
                                Item {
                                    visible: rowItem.modelData.hazard
                                    implicitWidth: Theme.iconSmall
                                    implicitHeight: Theme.iconSmall
                                    Accessible.name: hazardTip.text
                                    NxIcon { anchors.fill: parent; name: "alert-triangle"; size: Theme.iconSmall; color: Theme.warning }
                                    HoverHandler { id: hazardHover }
                                    ToolTip {
                                        id: hazardTip
                                        visible: hazardHover.hovered
                                        text: rowItem.modelData.hazards !== "" ? rowItem.modelData.hazards
                                            : qsTr("Contains entries that are not extracted as stored")
                                    }
                                }
                            }
                            Label {
                                Layout.preferredWidth: 96
                                horizontalAlignment: Text.AlignRight
                                text: rowItem.modelData.directory
                                    ? qsTr("%n items", "", rowItem.modelData.items) : Theme.formatBytes(rowItem.modelData.size)
                                color: Theme.textSecondary
                                font.pixelSize: Theme.fontSizeSmall
                            }
                            Label {
                                Layout.preferredWidth: 96
                                horizontalAlignment: Text.AlignRight
                                text: rowItem.modelData.directory || rowItem.modelData.packed < 0 ? "—" : Theme.formatBytes(rowItem.modelData.packed)
                                color: Theme.textMuted
                                font.pixelSize: Theme.fontSizeSmall
                            }
                            Label {
                                Layout.preferredWidth: 150
                                Layout.leftMargin: Theme.spaceMd
                                text: rowItem.modelData.modified !== "" ? rowItem.modelData.modified : "—"
                                color: Theme.textMuted
                                font.pixelSize: Theme.fontSizeSmall
                                elide: Text.ElideRight
                            }
                        }
                    }

                    Label {
                        anchors.centerIn: parent
                        visible: table.count === 0 && !session.busy
                        text: session.entryCount === 0 ? qsTr("This archive is empty") : qsTr("This folder is empty")
                        color: Theme.textMuted
                    }
                }
            }
        }

        // ---- progress --------------------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: progressRow.implicitHeight + Theme.spaceMd * 2
            visible: session.busy && session.jobKind !== "list"
            color: Theme.bgRaised
            NxDivider { anchors.top: parent.top; width: parent.width }
            RowLayout {
                id: progressRow
                anchors.fill: parent
                anchors.margins: Theme.spaceMd
                spacing: Theme.spaceMd
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceXs
                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            text: session.jobKind === "extract" ? qsTr("Extracting…")
                                : session.jobKind === "create" ? qsTr("Creating archive…")
                                : session.jobKind === "test" ? qsTr("Testing…")
                                : session.jobKind === "add" ? qsTr("Adding files…")
                                : session.jobKind === "delete" ? qsTr("Deleting entries…")
                                : session.jobKind === "preview" ? qsTr("Extracting for preview…") : ""
                            font.weight: Theme.weightSemibold
                        }
                        Label {
                            Layout.fillWidth: true
                            text: session.progressText
                            color: Theme.textMuted
                            elide: Text.ElideMiddle
                            font.pixelSize: Theme.fontSizeSmall
                        }
                        Label {
                            visible: session.progress >= 0
                            text: qsTr("%1 %").arg(Math.round(session.progress * 100))
                            color: Theme.textSecondary
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.fontSizeSmall
                        }
                    }
                    ProgressBar {
                        Layout.fillWidth: true
                        indeterminate: session.progress < 0
                        value: Math.max(0, session.progress)
                    }
                }
                NxButton { text: qsTr("Cancel"); onClicked: session.cancel() }
            }
        }

        // ---- status bar ------------------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.controlHeight
            color: Theme.bgSurface
            NxDivider { anchors.top: parent.top; width: parent.width }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.spaceMd
                anchors.rightMargin: Theme.spaceMd
                spacing: Theme.spaceLg
                Label {
                    visible: session.isOpen
                    text: window.selectedCount > 0 ? qsTr("%n selected", "", window.selectedCount)
                        : qsTr("%n entries", "", session.entryCount)
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSizeSmall
                }
                Label {
                    visible: session.isOpen
                    text: qsTr("%1 unpacked, %2 packed").arg(Theme.formatBytes(session.totalSize)).arg(Theme.formatBytes(session.archiveSize))
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSizeSmall
                }
                Item { Layout.fillWidth: true }
                Label {
                    visible: session.isOpen && !session.canModify
                    text: qsTr("Read-only: %1").arg(session.readOnlyReason)
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSizeSmall
                    elide: Text.ElideRight
                    Layout.maximumWidth: window.width / 2
                }
            }
        }
    }

    Menu {
        id: rowMenu
        property var entry: null
        MenuItem {
            text: rowMenu.entry && rowMenu.entry.directory ? qsTr("Open folder") : qsTr("Open a temporary copy")
            enabled: !session.busy
            onTriggered: window.activate(rowMenu.entry)
        }
        MenuItem {
            text: qsTr("Extract…")
            enabled: !session.busy
            onTriggered: window.showExtract([rowMenu.entry.path])
        }
        MenuItem {
            text: qsTr("Delete from archive")
            enabled: session.canModify && !session.busy
            onTriggered: {
                window.clearSelection()
                window.toggleSelected(rowMenu.entry.path)
                deleteConfirm.open()
            }
        }
    }

    // ---- dialogs -------------------------------------------------------------------
    PathPicker {
        id: openPicker
        fs: ArchiveTools
        mode: "open"
        title: qsTr("Open archive")
        function show() { folder = prefs.lastFolder; open() }
        onChosen: (paths) => window.openArchive(paths[0])
    }
    PathPicker {
        id: addPicker
        fs: ArchiveTools
        mode: "open"
        multiple: true
        allowFolders: true
        title: qsTr("Add to %1").arg(session.archiveName)
        acceptText: qsTr("Add")
        function show() { folder = prefs.lastFolder; open() }
        onChosen: (paths) => session.addFiles(paths)
    }
    PathPicker {
        id: folderPicker
        property var apply: null
        fs: ArchiveTools
        mode: "folder"
        onChosen: (paths) => { if (apply) apply(paths[0]) }
    }
    PathPicker {
        id: inputPicker
        fs: ArchiveTools
        mode: "open"
        multiple: true
        allowFolders: true
        title: qsTr("Choose files and folders")
        acceptText: qsTr("Add")
        onChosen: (paths) => createDialog.addInputs(paths)
    }

    NxConfirmDialog {
        id: deleteConfirm
        title: qsTr("Delete from the archive?")
        message: qsTr("%n selected entries (folders with everything inside) will be removed from “%1”. This cannot be undone.", "", window.selectedCount)
            .arg(session.archiveName)
        confirmText: qsTr("Delete")
        destructive: true
        onConfirmed: session.deleteEntries(window.selectedPaths)
    }
    NxConfirmDialog {
        id: addConfirm
        property var paths: []
        title: qsTr("Add to the archive?")
        message: qsTr("%n dropped items will be added to the top level of “%1”.", "", paths.length).arg(session.archiveName)
        confirmText: qsTr("Add")
        onConfirmed: session.addFiles(paths)
    }

    Dialog {
        id: passwordDialog
        property bool wrong: false
        title: qsTr("Password required")
        width: Math.min(440, window.width - Theme.spaceXxl)
        closePolicy: Popup.CloseOnEscape
        footer: null
        onOpened: { passwordField.text = ""; passwordField.forceActiveFocus() }
        onRejected: session.cancelPassword()
        function submit() {
            const password = passwordField.text
            passwordField.text = ""
            accept()
            session.submitPassword(password)
        }
        contentItem: ColumnLayout {
            spacing: Theme.spaceMd
            Label {
                Layout.fillWidth: true
                text: passwordDialog.wrong ? qsTr("That password did not work. Try again.")
                                           : qsTr("“%1” is encrypted. The password is kept only while the archive is open.").arg(session.archiveName)
                color: passwordDialog.wrong ? Theme.danger : Theme.textSecondary
                wrapMode: Text.WordWrap
            }
            NxPasswordField {
                id: passwordField
                Layout.fillWidth: true
                placeholderText: qsTr("Password")
                Accessible.name: qsTr("Password")
                onAccepted: if (text !== "") passwordDialog.submit()
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceSm
                Item { Layout.fillWidth: true }
                NxButton { text: qsTr("Cancel"); onClicked: passwordDialog.reject() }
                NxButton { text: qsTr("Unlock"); kind: "primary"; enabled: passwordField.text !== ""; onClicked: passwordDialog.submit() }
            }
        }
    }

    Dialog {
        id: resultDialog
        property bool ok: true
        property string message: ""
        property var details: []
        width: Math.min(620, window.width - Theme.spaceXxl)
        footer: null
        onClosed: if (ok && launch.mode === "extract" && window.batch) Qt.quit()
        contentItem: ColumnLayout {
            spacing: Theme.spaceMd
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceMd
                NxIcon {
                    Layout.alignment: Qt.AlignTop
                    name: resultDialog.ok ? (resultDialog.details.length > 0 ? "alert-triangle" : "check-circle") : "x-circle"
                    size: Theme.iconLarge
                    color: resultDialog.ok ? (resultDialog.details.length > 0 ? Theme.warning : Theme.success) : Theme.danger
                }
                Label {
                    objectName: "resultMessage"
                    Layout.fillWidth: true
                    text: resultDialog.message
                    color: Theme.textSecondary
                    wrapMode: Text.Wrap
                }
            }
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(260, detailList.contentHeight + Theme.spaceSm * 2)
                visible: resultDialog.details.length > 0
                radius: Theme.radiusSm
                color: Theme.bgInput
                border.width: Theme.borderWidth
                border.color: Theme.border
                clip: true
                ListView {
                    id: detailList
                    anchors.fill: parent
                    anchors.margins: Theme.spaceSm
                    model: resultDialog.details
                    spacing: Theme.spaceXs
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar { }
                    delegate: Label {
                        required property string modelData
                        width: detailList.width - Theme.spaceMd
                        text: modelData
                        color: Theme.textSecondary
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fontSizeSmall
                        wrapMode: Text.WrapAnywhere
                    }
                }
            }
            NxButton {
                Layout.alignment: Qt.AlignRight
                text: qsTr("Close")
                kind: "primary"
                onClicked: resultDialog.close()
            }
        }
    }

    Dialog {
        id: extractDialog
        property var selection: []
        property string destination: ""
        property bool subfolder: false
        property bool acknowledged: false
        readonly property string target: subfolder && session.suggestedFolderName !== ""
            ? ArchiveTools.joinPath(destination, session.suggestedFolderName) : destination
        readonly property var estimate: visible ? session.extractionEstimate(selection, target) : ({})
        readonly property bool ready: destination !== "" && estimate.enough === true && estimate.writable === true
            && (!estimate.suspicious || acknowledged)

        title: selection.length > 0 ? qsTr("Extract %n selected entries", "", selection.length) : qsTr("Extract everything")
        width: Math.min(600, window.width - Theme.spaceXxl)
        closePolicy: Popup.CloseOnEscape
        footer: null
        onRejected: if (launch.mode === "extract" && window.batch) Qt.quit()

        function start() {
            prefs.lastDestination = destination
            prefs.conflict = conflictBox.currentValue
            accept()
            session.extract(target, selection, { conflict: conflictBox.currentValue })
        }

        contentItem: ColumnLayout {
            spacing: Theme.spaceMd

            Label { text: qsTr("Extract to"); color: Theme.textSecondary }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceSm
                TextField {
                    id: destinationField
                    Layout.fillWidth: true
                    font.family: Theme.fontMono
                    text: extractDialog.destination
                    Accessible.name: qsTr("Destination folder")
                    onTextEdited: extractDialog.destination = text
                }
                NxButton {
                    text: qsTr("Browse…")
                    onClicked: {
                        folderPicker.folder = ArchiveTools.isDirectory(extractDialog.destination) ? extractDialog.destination : ""
                        folderPicker.apply = (path) => extractDialog.destination = path
                        folderPicker.open()
                    }
                }
            }
            CheckBox {
                visible: extractDialog.selection.length === 0
                text: qsTr("Put the files in a new folder “%1”").arg(session.suggestedFolderName)
                checked: extractDialog.subfolder
                onToggled: extractDialog.subfolder = checked
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceMd
                Label { text: qsTr("If a file already exists"); color: Theme.textSecondary }
                ComboBox {
                    id: conflictBox
                    Layout.fillWidth: true
                    textRole: "label"
                    valueRole: "key"
                    Accessible.name: qsTr("If a file already exists")
                    model: [
                        { key: "rename", label: qsTr("Keep both (save the new one under another name)") },
                        { key: "skip", label: qsTr("Keep the existing file") },
                        { key: "overwrite", label: qsTr("Replace the existing file") },
                    ]
                    Component.onCompleted: currentIndex = Math.max(0, indexOfValue(prefs.conflict))
                }
            }
            Label {
                Layout.fillWidth: true
                text: extractDialog.estimate.entries === undefined ? ""
                    : qsTr("%n entries, %1 unpacked. Free space in the destination: %2.", "", extractDialog.estimate.entries)
                        .arg(Theme.formatBytes(extractDialog.estimate.expanded))
                        .arg(extractDialog.estimate.free >= 0 ? Theme.formatBytes(extractDialog.estimate.free) : qsTr("unknown"))
                color: Theme.textMuted
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WordWrap
            }
            Notice {
                visible: extractDialog.estimate.enough === false
                kind: "danger"
                title: qsTr("Not enough free space")
                message: qsTr("Choose another folder or free some space first.")
            }
            Notice {
                visible: extractDialog.estimate.writable === false && extractDialog.destination !== ""
                kind: "danger"
                title: qsTr("You cannot write to this folder")
                message: qsTr("Choose a folder you own.")
            }
            Notice {
                visible: extractDialog.estimate.suspicious === true
                kind: "danger"
                title: qsTr("Possible decompression bomb")
                message: qsTr("The archive expands to %1 times its size (%2). Archives built to fill a disk look like this.")
                    .arg(Math.round(extractDialog.estimate.ratio || 0)).arg(Theme.formatBytes(session.totalSize))
            }
            CheckBox {
                visible: extractDialog.estimate.suspicious === true
                text: qsTr("I expect this much data — extract anyway")
                checked: extractDialog.acknowledged
                onToggled: extractDialog.acknowledged = checked
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceSm
                Item { Layout.fillWidth: true }
                NxButton { text: qsTr("Cancel"); onClicked: extractDialog.reject() }
                NxButton {
                    text: qsTr("Extract")
                    kind: "primary"
                    enabled: extractDialog.ready
                    onClicked: extractDialog.start()
                }
            }
        }
    }

    Dialog {
        id: createDialog
        property string folder: ""
        property string name: ""
        property var inputs: []
        property int formatIndex: 0
        readonly property var formats: ArchiveTools.createFormats
        readonly property var format: formats[formatIndex] ?? ({})
        readonly property string output: folder !== "" && name.trim() !== ""
            ? ArchiveTools.joinPath(folder, name.trim() + "." + format.extension) : ""
        readonly property bool replaces: output !== "" && ArchiveTools.exists(output)
        readonly property bool passwordsMatch: passwordA.text === passwordB.text
        readonly property bool ready: inputs.length > 0 && output !== "" && ArchiveTools.isDirectory(folder)
            && format.available === true && (!format.encrypt || passwordsMatch)

        function setInputs(paths) { inputs = paths.slice() }
        function addInputs(paths) {
            const list = inputs.slice()
            for (const path of paths) if (list.indexOf(path) < 0) list.push(path)
            inputs = list
            if (name === "" || name === qsTr("Archive")) name = ArchiveTools.suggestedName(list)
        }
        function removeInput(index) {
            const list = inputs.slice()
            list.splice(index, 1)
            inputs = list
        }
        function selectFormat(id) {
            for (let i = 0; i < formats.length; i++) if (formats[i].id === id) { formatIndex = i; return }
        }
        function start() {
            prefs.createFormat = format.id
            prefs.createLevel = levelBox.currentValue
            prefs.lastFolder = folder
            const options = {
                format: format.id,
                level: levelBox.currentValue,
                password: format.encrypt ? passwordA.text : "",
                encryptNames: format.id === "7z" && namesBox.checked,
            }
            const target = output, list = inputs
            passwordA.text = ""
            passwordB.text = ""
            accept()
            session.create(target, list, options)
        }

        title: qsTr("New archive")
        width: Math.min(640, window.width - Theme.spaceXxl)
        closePolicy: Popup.CloseOnEscape
        footer: null
        onRejected: if (launch.mode === "create" && window.batch) Qt.quit()

        contentItem: ColumnLayout {
            spacing: Theme.spaceMd

            RowLayout {
                Layout.fillWidth: true
                Label { Layout.fillWidth: true; text: qsTr("Contents (%n items)", "", createDialog.inputs.length); color: Theme.textSecondary }
                NxButton {
                    text: qsTr("Add files or folders…")
                    iconName: "plus"
                    onClicked: {
                        inputPicker.folder = createDialog.inputs.length > 0
                            ? ArchiveTools.parentPath(createDialog.inputs[0]) : prefs.lastFolder
                        inputPicker.open()
                    }
                }
            }
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 132
                radius: Theme.radiusSm
                color: Theme.bgInput
                border.width: Theme.borderWidth
                border.color: Theme.border
                clip: true
                ListView {
                    id: inputList
                    anchors.fill: parent
                    anchors.margins: Theme.spaceXs
                    model: createDialog.inputs
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar { }
                    delegate: RowLayout {
                        id: inputRow
                        required property string modelData
                        required property int index
                        width: inputList.width
                        spacing: Theme.spaceSm
                        NxIcon {
                            Layout.leftMargin: Theme.spaceSm
                            name: ArchiveTools.isDirectory(inputRow.modelData) ? "folder" : "file"
                            size: Theme.iconSmall
                            color: Theme.textSecondary
                        }
                        Label { Layout.fillWidth: true; text: inputRow.modelData; elide: Text.ElideMiddle; font.pixelSize: Theme.fontSizeSmall }
                        NxIconButton {
                            iconName: "close"
                            iconSize: Theme.iconSmall
                            tooltip: qsTr("Remove from the list")
                            onClicked: createDialog.removeInput(inputRow.index)
                        }
                    }
                    Label {
                        anchors.centerIn: parent
                        visible: inputList.count === 0
                        text: qsTr("Nothing chosen yet")
                        color: Theme.textMuted
                    }
                }
            }

            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: Theme.spaceMd
                rowSpacing: Theme.spaceSm

                Label { text: qsTr("Save in"); color: Theme.textSecondary }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceSm
                    TextField {
                        Layout.fillWidth: true
                        font.family: Theme.fontMono
                        text: createDialog.folder
                        Accessible.name: qsTr("Folder")
                        onTextEdited: createDialog.folder = text
                    }
                    NxButton {
                        text: qsTr("Browse…")
                        onClicked: {
                            folderPicker.folder = createDialog.folder
                            folderPicker.apply = (path) => createDialog.folder = path
                            folderPicker.open()
                        }
                    }
                }
                Label { text: qsTr("Name"); color: Theme.textSecondary }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceSm
                    TextField {
                        Layout.fillWidth: true
                        text: createDialog.name
                        Accessible.name: qsTr("Archive name")
                        onTextEdited: createDialog.name = text
                    }
                    Label { text: "." + (createDialog.format.extension ?? ""); color: Theme.textMuted; font.family: Theme.fontMono }
                }
                Label { text: qsTr("Format"); color: Theme.textSecondary }
                ComboBox {
                    Layout.fillWidth: true
                    model: createDialog.formats
                    textRole: "label"
                    currentIndex: createDialog.formatIndex
                    Accessible.name: qsTr("Format")
                    onActivated: (index) => createDialog.formatIndex = index
                }
                Label { text: qsTr("Compression"); color: Theme.textSecondary; visible: levelBox.visible }
                ComboBox {
                    id: levelBox
                    Layout.fillWidth: true
                    visible: createDialog.format.compressed === true
                    textRole: "label"
                    valueRole: "key"
                    Accessible.name: qsTr("Compression level")
                    model: {
                        const levels = [
                            { key: "fast", label: qsTr("Fast — larger file") },
                            { key: "normal", label: qsTr("Normal") },
                            { key: "best", label: qsTr("Best — smaller file, slower") },
                        ]
                        if (createDialog.format.store) levels.unshift({ key: "store", label: qsTr("None — store only") })
                        return levels
                    }
                    // The stored preference where this format offers it, else "normal".
                    onModelChanged: {
                        let wanted = model.findIndex((level) => level.key === prefs.createLevel)
                        if (wanted < 0) wanted = model.findIndex((level) => level.key === "normal")
                        currentIndex = Math.max(0, wanted)
                    }
                }
                Label { text: qsTr("Password"); color: Theme.textSecondary; visible: createDialog.format.encrypt === true }
                NxPasswordField {
                    id: passwordA
                    Layout.fillWidth: true
                    visible: createDialog.format.encrypt === true
                    placeholderText: qsTr("Leave empty for no encryption")
                    Accessible.name: qsTr("Password")
                }
                Label { text: qsTr("Repeat"); color: Theme.textSecondary; visible: passwordB.visible }
                NxPasswordField {
                    id: passwordB
                    Layout.fillWidth: true
                    visible: createDialog.format.encrypt === true && passwordA.text !== ""
                    placeholderText: qsTr("Repeat the password")
                    Accessible.name: qsTr("Repeat the password")
                }
            }
            CheckBox {
                id: namesBox
                visible: createDialog.format.id === "7z" && passwordA.text !== ""
                text: qsTr("Also encrypt the file names")
            }
            Label {
                Layout.fillWidth: true
                visible: text !== ""
                text: createDialog.format.available === false
                        ? qsTr("The program “%1” needed for this format is not installed.").arg(createDialog.format.tool)
                    : createDialog.format.encrypt !== true
                        ? qsTr("This format has no encryption. Use ZIP or 7-Zip to protect an archive with a password.")
                    : passwordA.text === "" ? qsTr("Encryption if a password is set: %1").arg(createDialog.format.cipher)
                    : !createDialog.passwordsMatch ? qsTr("The two passwords are different.")
                    : qsTr("Encryption: %1").arg(createDialog.format.cipher)
                color: createDialog.format.available === false || (passwordA.text !== "" && !createDialog.passwordsMatch && createDialog.format.encrypt === true)
                    ? Theme.danger : Theme.textMuted
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WordWrap
            }
            Label {
                Layout.fillWidth: true
                visible: createDialog.replaces
                text: qsTr("“%1” already exists and will be replaced.").arg(ArchiveTools.fileName(createDialog.output))
                color: Theme.warning
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceSm
                Item { Layout.fillWidth: true }
                NxButton { text: qsTr("Cancel"); onClicked: createDialog.reject() }
                NxButton {
                    text: createDialog.replaces ? qsTr("Replace") : qsTr("Create")
                    kind: createDialog.replaces ? "danger" : "primary"
                    enabled: createDialog.ready
                    onClicked: createDialog.start()
                }
            }
        }
    }
}
