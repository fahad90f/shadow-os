// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.files

// NEXUS Files — main window: sidebar, toolbar, tabs, panes, preview panel,
// menus, dialogs and keyboard shortcuts. Panes and dialogs reach this object
// as `shell`.
NxWindow {
    id: window

    title: currentPage ? qsTr("%1 — Files").arg(currentPage.title) : qsTr("Files")
    width: 1240
    height: 800

    // ---- preferences: ~/.config/nexus/files.conf, [View] ---------------------------

    readonly property alias prefs: prefsObject
    QtObject {
        id: prefsObject
        property string viewMode: "grid"        // "grid" | "list"
        property string sortRole: "name"        // "name" | "size" | "modified" | "type"
        property bool sortAscending: true
        property bool foldersFirst: true
        property bool showHidden: false
        property bool previewPanel: false
        property int iconSize: 64
    }
    Settings {
        category: "View"
        property alias viewMode: prefsObject.viewMode
        property alias sortRole: prefsObject.sortRole
        property alias sortAscending: prefsObject.sortAscending
        property alias foldersFirst: prefsObject.foldersFirst
        property alias showHidden: prefsObject.showHidden
        property alias previewPanel: prefsObject.previewPanel
        property alias iconSize: prefsObject.iconSize
    }
    readonly property var iconSizes: [48, 64, 80, 96, 128]

    // ---- tabs ----------------------------------------------------------------------

    ListModel { id: tabs }
    property int currentTab: 0
    property var pages: []                  // TabPage items, in tab order
    readonly property var currentPage: pages[currentTab] ?? null
    readonly property var activePane: currentPage ? currentPage.activePane : null

    function refreshPages() {
        const list = []
        for (let i = 0; i < tabRepeater.count; i++) {
            const item = tabRepeater.itemAt(i)
            if (item) list.push(item)
        }
        window.pages = list
        if (window.currentTab >= list.length) window.currentTab = Math.max(0, list.length - 1)
    }

    function openInNewTab(url, activate, selection) {
        tabs.append({ initialUrl: url.toString(), initialSelection: selection ? selection.toString() : "" })
        window.refreshPages()
        if (activate !== false) {
            window.currentTab = tabs.count - 1
            window.focusActivePane()
        }
    }

    function closeTab(index) {
        if (tabs.count <= 1) { window.close(); return }
        tabs.remove(index)
        if (index < window.currentTab) window.currentTab -= 1
        window.refreshPages()
        window.focusActivePane()
    }

    function selectTab(index) {
        if (index < 0 || index >= tabs.count) return
        window.currentTab = index
        window.focusActivePane()
    }

    // ---- services for panes, sidebar and dialogs -----------------------------------

    function plural(n, one, many) {
        return n === 1 ? one : many.arg(n)
    }

    function focusActivePane() {
        Qt.callLater(() => { if (window.activePane) window.activePane.focusView() })
    }

    function navigate(url) {
        if (!window.activePane) return
        window.activePane.folder.open(url)
        window.focusActivePane()
    }

    function navigateText(text) {
        if (!window.activePane) return
        window.activePane.folder.openLocation(text)
        window.focusActivePane()
    }

    function handleDrop(drop, destUrl) {
        if (!drop.hasUrls) { drop.accepted = false; return }
        const action = FileOps.dropAction(drop.urls, destUrl)
        if (action === "") { drop.accepted = false; return }
        FileOps.drop(drop.urls, destUrl)
        drop.accept(action === "copy" ? Qt.CopyAction : action === "link" ? Qt.LinkAction : Qt.MoveAction)
    }

    function zoom(direction) {
        let i = window.iconSizes.indexOf(prefs.iconSize)
        if (i < 0) i = 1
        prefs.iconSize = window.iconSizes[Math.max(0, Math.min(window.iconSizes.length - 1, i + direction))]
    }

    function toggleSplit() {
        if (window.currentPage) window.currentPage.setSplit(!window.currentPage.split)
    }

    function targetUrls(pane) {
        return pane.folder.selectedCount > 0 ? pane.folder.selectedUrls : [pane.folder.url]
    }

    function showProperties(pane) {
        if (pane && pane.folder.error === "") propertiesDialog.show(window.targetUrls(pane))
    }

    function confirmDelete(urls) {
        deleteDialog.urls = urls
        deleteDialog.open()
    }

    function newFolder(pane) {
        const dir = pane.folder.url
        nameDialog.ask(qsTr("New folder"), qsTr("Name of the new folder in “%1”:").arg(pane.folder.title),
            FileOps.suggestName(dir, qsTr("New Folder")), qsTr("Create"),
            (name) => {
                const problem = FileOps.validateName(name)
                if (problem !== "") return problem
                return pane.folder.nameExists(name.trim()) ? qsTr("“%1” already exists in this folder.").arg(name.trim()) : ""
            },
            (name) => FileOps.createFolder(dir, name), false)
    }

    function newFile(pane) {
        const dir = pane.folder.url
        nameDialog.ask(qsTr("New file"), qsTr("Name of the new empty file in “%1”:").arg(pane.folder.title),
            FileOps.suggestName(dir, qsTr("New File.txt")), qsTr("Create"),
            (name) => {
                const problem = FileOps.validateName(name)
                if (problem !== "") return problem
                return pane.folder.nameExists(name.trim()) ? qsTr("“%1” already exists in this folder.").arg(name.trim()) : ""
            },
            (name) => FileOps.createFile(dir, name), true)
    }

    function connectToServer() {
        connectDialog.show((url) => window.navigate(url))
    }

    function chooseOpenWith(url, mimeType, name) {
        openWithDialog.show(url, mimeType, name)
    }

    function extractHere(pane) {
        const urls = pane.folder.selectedUrls
        for (let i = 0; i < urls.length; i++) Archiver.extract(urls[i], pane.folder.url)
    }

    function extractTo(pane) {
        const urls = pane.folder.selectedUrls
        folderPicker.show(urls.length === 1 ? qsTr("Extract “%1” to").arg(ctx.item.name ?? "") : qsTr("Extract %1 archives to").arg(urls.length),
            qsTr("Extract here"), pane.folder.url,
            (dest) => { for (let i = 0; i < urls.length; i++) Archiver.extract(urls[i], dest) })
    }

    function copyOrMoveTo(pane, move) {
        const urls = pane.folder.selectedUrls
        folderPicker.show(move ? qsTr("Move to") : qsTr("Copy to"), move ? qsTr("Move here") : qsTr("Copy here"), pane.folder.url,
            (dest) => { if (move) FileOps.moveTo(urls, dest); else FileOps.copyTo(urls, dest) })
    }

    function toggleBookmark(url) {
        if (Places.isBookmarked(url)) Places.removeBookmark(url)
        else Places.addBookmark(url, "")
    }

    // ---- context menus -------------------------------------------------------------

    // What the item menu acts on, captured when it opens.
    QtObject {
        id: ctx
        property var pane: null
        property var urls: []
        property var item: ({})             // FolderModel.get() of a single selected item
        property bool allLocal: false
        property bool allArchives: false
        property var place: ({})            // sidebar entry
        property var device: ({})
        property int resultRow: -1
        readonly property bool single: urls.length === 1
        readonly property bool singleDir: single && item.isDir === true
        readonly property bool inTrash: pane !== null && pane.folder.trash
        readonly property bool writable: pane !== null && pane.folder.writable
    }

    function showItemMenu(pane, position) {
        const f = pane.folder
        const rows = f.selectedRows()
        if (rows.length === 0) return
        ctx.pane = pane
        ctx.urls = f.selectedUrls
        ctx.item = rows.length === 1 ? f.get(rows[0]) : ({})
        let local = true, archives = rows.length <= 200
        for (let i = 0; i < rows.length && i < 200; i++) {
            const it = f.get(rows[i])
            local = local && it.localPath !== ""
            archives = archives && it.isArchive === true
        }
        ctx.allLocal = local && rows.length <= 200
        ctx.allArchives = archives
        window.popupMenu(itemMenu, position)
    }

    function showBackgroundMenu(pane, position) {
        ctx.pane = pane
        window.popupMenu(backgroundMenu, position)
    }

    // At the pointer, or at `position` (window coordinates) when the menu was
    // requested with the keyboard.
    function popupMenu(menuItem, position) {
        if (position) menuItem.popup(window.contentItem, position.x, position.y)
        else menuItem.popup()
    }

    function showResultMenu(pane, row) {
        ctx.pane = pane
        ctx.resultRow = row
        resultMenu.popup()
    }

    function showPlaceMenu(entry) {
        ctx.place = entry
        placeMenu.popup()
    }

    function showDeviceMenu(device) {
        ctx.device = device
        deviceMenu.popup()
    }

    // A menu entry that takes no room while it does not apply.
    component Entry: MenuItem {
        property bool shown: true
        // false: listed but greyed out
        property bool usable: true
        property string glyph: ""
        visible: shown
        height: shown ? implicitHeight : 0
        // Hidden entries are disabled too, so arrow keys skip them.
        enabled: shown && usable
        icon.source: glyph !== "" ? "image://nxicon/" + glyph : ""
    }
    component Separator: MenuSeparator {
        property bool shown: true
        visible: shown
        height: shown ? implicitHeight : 0
    }
    // A check-marked entry that mirrors `on`. Clicking a checkable MenuItem
    // toggles `checked` itself, which would detach it from the state it shows;
    // the binding is restored after every click.
    component Toggle: Entry {
        id: toggle
        property bool on: false
        signal flipped()
        checkable: true
        checked: on
        onTriggered: {
            flipped()
            checked = Qt.binding(() => toggle.on)
        }
    }
    component SortEntry: Toggle {
        property string role: ""
        on: window.prefs.sortRole === role
        onFlipped: window.prefs.sortRole = role
    }

    Menu {
        id: itemMenu
        Entry {
            text: ctx.urls.length > 1 ? qsTr("Open %1 items").arg(ctx.urls.length) : qsTr("Open")
            glyph: "folder-open"
            shown: !ctx.inTrash || ctx.singleDir
            onTriggered: ctx.pane.openSelection()
        }
        Entry {
            text: qsTr("Open with…")
            glyph: "apps"
            shown: ctx.single && !ctx.singleDir && !ctx.inTrash
            onTriggered: window.chooseOpenWith(ctx.item.url, ctx.item.mimeType, ctx.item.name)
        }
        Entry {
            text: qsTr("Open in new tab")
            glyph: "plus"
            shown: ctx.singleDir
            onTriggered: window.openInNewTab(ctx.item.url, true)
        }
        Entry {
            text: qsTr("Open with Archive Manager")
            glyph: "archive"
            shown: ctx.single && ctx.allArchives && Archiver.managerAvailable
            onTriggered: Archiver.openInManager(ctx.item.url)
        }
        Separator { shown: ctx.inTrash && ctx.singleDir }
        Entry {
            text: qsTr("Restore")
            glyph: "undo"
            shown: ctx.inTrash
            onTriggered: FileOps.restore(ctx.urls)
        }
        Separator { }
        Entry {
            text: qsTr("Cut")
            glyph: "cut"
            shown: !ctx.inTrash
            onTriggered: ctx.pane.copySelection(true)
        }
        Entry {
            text: qsTr("Copy")
            glyph: "copy"
            onTriggered: ctx.pane.copySelection(false)
        }
        Entry {
            text: qsTr("Paste into folder")
            glyph: "paste"
            shown: ctx.singleDir && !ctx.inTrash && FileOps.clipboardHasUrls
            onTriggered: FileOps.paste(ctx.item.url)
        }
        Entry {
            text: qsTr("Copy to…")
            glyph: "folder"
            shown: !ctx.inTrash
            onTriggered: window.copyOrMoveTo(ctx.pane, false)
        }
        Entry {
            text: qsTr("Move to…")
            glyph: "arrow-right"
            shown: !ctx.inTrash && ctx.writable
            onTriggered: window.copyOrMoveTo(ctx.pane, true)
        }
        Entry {
            text: qsTr("Duplicate")
            glyph: "layers"
            shown: !ctx.inTrash && ctx.writable
            onTriggered: FileOps.duplicate(ctx.urls)
        }
        Entry {
            text: qsTr("Rename…")
            glyph: "edit"
            shown: ctx.single && !ctx.inTrash
            onTriggered: ctx.pane.startRename()
        }
        Separator { shown: Archiver.available && ctx.allLocal && !ctx.inTrash }
        Entry {
            text: qsTr("Extract here")
            glyph: "archive"
            shown: Archiver.available && ctx.allArchives && ctx.allLocal && ctx.writable
            onTriggered: window.extractHere(ctx.pane)
        }
        Entry {
            text: qsTr("Extract to…")
            glyph: "archive"
            shown: Archiver.available && ctx.allArchives && ctx.allLocal && !ctx.inTrash
            onTriggered: window.extractTo(ctx.pane)
        }
        Entry {
            text: qsTr("Compress…")
            glyph: "package"
            shown: Archiver.available && ctx.allLocal && ctx.writable
            onTriggered: compressDialog.show(ctx.urls, ctx.pane.folder.url, ctx.pane.folder)
        }
        Separator { }
        Entry {
            text: qsTr("Move to Trash")
            glyph: "trash"
            shown: !ctx.inTrash && Launcher.protocolAvailable("trash")
            onTriggered: FileOps.trash(ctx.urls)
        }
        Entry {
            text: qsTr("Delete permanently…")
            glyph: "x-circle"
            onTriggered: window.confirmDelete(ctx.urls)
        }
        Separator { }
        Entry {
            text: ctx.singleDir && Places.isBookmarked(ctx.item.url) ? qsTr("Remove from bookmarks") : qsTr("Add to bookmarks")
            glyph: "star"
            shown: ctx.singleDir && !ctx.inTrash
            onTriggered: window.toggleBookmark(ctx.item.url)
        }
        Entry {
            text: qsTr("Open terminal here")
            glyph: "terminal"
            shown: ctx.singleDir && ctx.allLocal && Launcher.terminalAvailable
            onTriggered: Launcher.openTerminal(ctx.item.url)
        }
        Entry {
            text: ctx.urls.length > 1 ? qsTr("Copy paths") : qsTr("Copy path")
            glyph: "link"
            onTriggered: {
                Launcher.copyText(ctx.urls.map(u => FileOps.displayPath(u)).join("\n"))
                window.toaster.show(ctx.urls.length > 1 ? qsTr("Paths copied to the clipboard") : qsTr("Path copied to the clipboard"), "success")
            }
        }
        Entry {
            text: qsTr("Properties")
            glyph: "info"
            onTriggered: window.showProperties(ctx.pane)
        }
    }

    Menu {
        id: sortMenu
        title: qsTr("Sort by")
        SortEntry { role: "name"; text: qsTr("Name") }
        SortEntry { role: "size"; text: qsTr("Size") }
        SortEntry { role: "modified"; text: qsTr("Date modified") }
        SortEntry { role: "type"; text: qsTr("Type") }
        MenuSeparator { }
        Toggle {
            text: qsTr("Ascending")
            on: prefs.sortAscending
            onFlipped: prefs.sortAscending = true
        }
        Toggle {
            text: qsTr("Descending")
            on: !prefs.sortAscending
            onFlipped: prefs.sortAscending = false
        }
        MenuSeparator { }
        Toggle {
            text: qsTr("Folders first")
            on: prefs.foldersFirst
            onFlipped: prefs.foldersFirst = !prefs.foldersFirst
        }
    }

    Menu {
        id: backgroundMenu
        Entry {
            text: qsTr("New folder…")
            glyph: "folder"
            shown: ctx.writable
            onTriggered: window.newFolder(ctx.pane)
        }
        Entry {
            text: qsTr("New file…")
            glyph: "file"
            shown: ctx.writable
            onTriggered: window.newFile(ctx.pane)
        }
        Separator { shown: ctx.writable }
        Entry {
            text: FileOps.clipboardCount > 1 ? qsTr("Paste %1 items").arg(FileOps.clipboardCount) : qsTr("Paste")
            glyph: "paste"
            shown: ctx.writable
            usable: FileOps.clipboardHasUrls
            onTriggered: ctx.pane.paste()
        }
        Entry {
            text: qsTr("Select all")
            glyph: "check-circle"
            onTriggered: ctx.pane.selectAll()
        }
        Separator { }
        Menu {
            title: qsTr("Sort by")
            icon.source: "image://nxicon/sort"
            SortEntry { role: "name"; text: qsTr("Name") }
            SortEntry { role: "size"; text: qsTr("Size") }
            SortEntry { role: "modified"; text: qsTr("Date modified") }
            SortEntry { role: "type"; text: qsTr("Type") }
            MenuSeparator { }
            MenuItem {
                text: prefs.sortAscending ? qsTr("Switch to descending") : qsTr("Switch to ascending")
                onTriggered: prefs.sortAscending = !prefs.sortAscending
            }
            Toggle {
                text: qsTr("Folders first")
                on: prefs.foldersFirst
                onFlipped: prefs.foldersFirst = !prefs.foldersFirst
            }
        }
        Entry {
            text: prefs.viewMode === "grid" ? qsTr("Show as list") : qsTr("Show as grid")
            glyph: prefs.viewMode === "grid" ? "list" : "grid"
            onTriggered: prefs.viewMode = prefs.viewMode === "grid" ? "list" : "grid"
        }
        Toggle {
            text: qsTr("Show hidden files")
            on: prefs.showHidden
            onFlipped: prefs.showHidden = !prefs.showHidden
        }
        Entry {
            text: qsTr("Reload")
            glyph: "refresh"
            onTriggered: ctx.pane.folder.reload()
        }
        Separator { }
        Entry {
            text: qsTr("Empty the Trash…")
            glyph: "trash"
            shown: ctx.inTrash
            usable: ctx.pane !== null && ctx.pane.folder.count > 0
            onTriggered: emptyTrashDialog.open()
        }
        Entry {
            text: ctx.pane !== null && Places.isBookmarked(ctx.pane.folder.url) ? qsTr("Remove from bookmarks") : qsTr("Add to bookmarks")
            glyph: "star"
            shown: !ctx.inTrash
            onTriggered: window.toggleBookmark(ctx.pane.folder.url)
        }
        Entry {
            text: qsTr("Open terminal here")
            glyph: "terminal"
            shown: ctx.pane !== null && ctx.pane.folder.local && Launcher.terminalAvailable
            onTriggered: Launcher.openTerminal(ctx.pane.folder.url)
        }
        Entry {
            text: qsTr("Copy path")
            glyph: "link"
            onTriggered: {
                Launcher.copyText(ctx.pane.folder.location)
                window.toaster.show(qsTr("Path copied to the clipboard"), "success")
            }
        }
        Entry {
            text: qsTr("Properties")
            glyph: "info"
            shown: ctx.pane !== null && ctx.pane.folder.error === ""
            onTriggered: propertiesDialog.show([ctx.pane.folder.url])
        }
    }

    Menu {
        id: resultMenu
        Entry {
            text: qsTr("Open")
            glyph: "folder-open"
            onTriggered: ctx.pane.openResult(ctx.resultRow)
        }
        Entry {
            text: qsTr("Show in its folder")
            glyph: "target"
            onTriggered: ctx.pane.revealResult(ctx.resultRow)
        }
        Entry {
            text: qsTr("Copy path")
            glyph: "link"
            onTriggered: {
                Launcher.copyText(FileOps.displayPath(ctx.pane.results.get(ctx.resultRow).url))
                window.toaster.show(qsTr("Path copied to the clipboard"), "success")
            }
        }
        Entry {
            text: qsTr("Properties")
            glyph: "info"
            onTriggered: propertiesDialog.show([ctx.pane.results.get(ctx.resultRow).url])
        }
    }

    Menu {
        id: placeMenu
        Entry {
            text: qsTr("Open")
            glyph: "folder-open"
            onTriggered: window.navigate(ctx.place.url)
        }
        Entry {
            text: qsTr("Open in new tab")
            glyph: "plus"
            onTriggered: window.openInNewTab(ctx.place.url, true)
        }
        Separator { shown: ctx.place.kind === "bookmark" || ctx.place.kind === "server" || ctx.place.kind === "trash" }
        Entry {
            text: qsTr("Rename…")
            glyph: "edit"
            shown: ctx.place.kind === "bookmark"
            onTriggered: {
                const url = ctx.place.url
                nameDialog.ask(qsTr("Rename bookmark"), qsTr("Label shown in the sidebar:"), ctx.place.title, qsTr("Rename"),
                    null, (name) => Places.renameBookmark(url, name), false)
            }
        }
        Entry {
            text: qsTr("Remove from bookmarks")
            glyph: "minus"
            shown: ctx.place.kind === "bookmark"
            onTriggered: Places.removeBookmark(ctx.place.url)
        }
        Entry {
            text: qsTr("Remove from sidebar")
            glyph: "minus"
            shown: ctx.place.kind === "server"
            onTriggered: Places.removeServer(ctx.place.url)
        }
        Entry {
            text: qsTr("Empty the Trash…")
            glyph: "trash"
            shown: ctx.place.kind === "trash"
            usable: !Places.trashEmpty
            onTriggered: emptyTrashDialog.open()
        }
        Separator { shown: ctx.place.kind !== "trash" && ctx.place.kind !== "server" }
        Entry {
            text: qsTr("Open terminal here")
            glyph: "terminal"
            shown: ctx.place.kind !== "trash" && ctx.place.kind !== "server" && Launcher.terminalAvailable
            onTriggered: Launcher.openTerminal(ctx.place.url)
        }
        Entry {
            text: qsTr("Properties")
            glyph: "info"
            shown: ctx.place.kind !== "trash" && ctx.place.kind !== "server"
            onTriggered: propertiesDialog.show([ctx.place.url])
        }
    }

    Menu {
        id: deviceMenu
        Entry {
            text: qsTr("Open")
            glyph: "folder-open"
            shown: ctx.device.mounted === true
            onTriggered: window.navigate(ctx.device.url)
        }
        Entry {
            text: qsTr("Open in new tab")
            glyph: "plus"
            shown: ctx.device.mounted === true
            onTriggered: window.openInNewTab(ctx.device.url, true)
        }
        Entry {
            text: qsTr("Mount")
            glyph: "harddrive"
            shown: ctx.device.mounted !== true
            usable: ctx.device.busy !== true
            onTriggered: Devices.mount(ctx.device.udi)
        }
        Entry {
            text: ctx.device.removable === true ? qsTr("Safely remove") : qsTr("Unmount")
            glyph: "eject"
            shown: ctx.device.mounted === true && ctx.device.path !== "/"
            usable: ctx.device.busy !== true
            onTriggered: Devices.unmount(ctx.device.udi)
        }
        Entry {
            text: qsTr("Eject the disc")
            glyph: "eject"
            shown: ctx.device.optical === true && ctx.device.mounted !== true
            usable: ctx.device.busy !== true
            onTriggered: Devices.eject(ctx.device.udi)
        }
        Separator { shown: ctx.device.mounted === true }
        Entry {
            text: qsTr("Properties")
            glyph: "info"
            shown: ctx.device.mounted === true
            onTriggered: propertiesDialog.show([ctx.device.url])
        }
    }

    Menu {
        id: mainMenu
        Entry {
            text: qsTr("New tab")
            glyph: "plus"
            onTriggered: if (window.activePane) window.openInNewTab(window.activePane.folder.url, true)
        }
        Entry {
            text: qsTr("New folder…")
            glyph: "folder"
            usable: window.activePane !== null && window.activePane.folder.writable
            onTriggered: window.newFolder(window.activePane)
        }
        Entry {
            text: qsTr("New file…")
            glyph: "file"
            usable: window.activePane !== null && window.activePane.folder.writable
            onTriggered: window.newFile(window.activePane)
        }
        Separator { }
        Entry {
            text: FileOps.undoAvailable ? FileOps.undoText : qsTr("Undo")
            glyph: "undo"
            usable: FileOps.undoAvailable
            onTriggered: FileOps.undo()
        }
        Entry {
            text: qsTr("Select all")
            glyph: "check-circle"
            onTriggered: if (window.activePane) window.activePane.selectAll()
        }
        Separator { }
        Toggle {
            text: qsTr("Show hidden files")
            on: prefs.showHidden
            onFlipped: prefs.showHidden = !prefs.showHidden
        }
        Toggle {
            text: qsTr("Split view")
            on: window.currentPage !== null && window.currentPage.split
            onFlipped: window.toggleSplit()
        }
        Toggle {
            text: qsTr("Preview panel")
            on: prefs.previewPanel
            onFlipped: prefs.previewPanel = !prefs.previewPanel
        }
        Entry {
            text: qsTr("Larger icons")
            glyph: "zoom-in"
            usable: prefs.viewMode === "grid" && prefs.iconSize < window.iconSizes[window.iconSizes.length - 1]
            onTriggered: window.zoom(1)
        }
        Entry {
            text: qsTr("Smaller icons")
            glyph: "zoom-out"
            usable: prefs.viewMode === "grid" && prefs.iconSize > window.iconSizes[0]
            onTriggered: window.zoom(-1)
        }
        Separator { }
        Entry {
            text: qsTr("Open terminal here")
            glyph: "terminal"
            shown: Launcher.terminalAvailable
            usable: window.activePane !== null && window.activePane.folder.local
            onTriggered: Launcher.openTerminal(window.activePane.folder.url)
        }
        Entry {
            text: qsTr("Connect to server…")
            glyph: "globe"
            shown: Launcher.networkAvailable
            onTriggered: window.connectToServer()
        }
        Entry {
            text: qsTr("Empty the Trash…")
            glyph: "trash"
            usable: !Places.trashEmpty
            onTriggered: emptyTrashDialog.open()
        }
    }

    // ---- dialogs -------------------------------------------------------------------

    readonly property bool dialogOpen: propertiesDialog.visible || nameDialog.visible || connectDialog.visible
        || compressDialog.visible || folderPicker.visible || openWithDialog.visible || questionDialog.visible
        || deleteDialog.visible || emptyTrashDialog.visible

    PropertiesDialog { id: propertiesDialog }
    NameDialog { id: nameDialog }
    ConnectDialog { id: connectDialog }
    CompressDialog { id: compressDialog }
    FolderPickerDialog { id: folderPicker }
    OpenWithDialog { id: openWithDialog }
    QuestionDialog { id: questionDialog }

    NxConfirmDialog {
        id: deleteDialog
        property var urls: []
        title: urls.length === 1 ? qsTr("Delete permanently?") : qsTr("Delete %1 items permanently?").arg(urls.length)
        message: urls.length === 1
            ? qsTr("“%1” will be deleted immediately. It is not moved to the Trash and cannot be restored.").arg(FileOps.displayPath(urls[0]))
            : qsTr("The %1 selected items will be deleted immediately. They are not moved to the Trash and cannot be restored.").arg(urls.length)
        confirmText: qsTr("Delete")
        destructive: true
        onConfirmed: FileOps.deletePermanently(urls)
        onClosed: window.focusActivePane()
    }

    NxConfirmDialog {
        id: emptyTrashDialog
        title: qsTr("Empty the Trash?")
        message: qsTr("Everything in the Trash will be deleted permanently. This cannot be undone.")
        confirmText: qsTr("Empty Trash")
        destructive: true
        onConfirmed: FileOps.emptyTrash()
        onClosed: window.focusActivePane()
    }

    // ---- back-end notifications ----------------------------------------------------

    Connections {
        target: Interaction
        function onErrorOccurred(title, message) {
            window.toaster.show(message !== "" ? title + " — " + message : title, "danger")
        }
        function onNotice(message, kind) {
            window.toaster.show(message, kind)
        }
    }
    Connections {
        target: Launcher
        function onOpenWithRequested(url, mimeType) {
            const text = url.toString()
            const name = decodeURIComponent(text.substring(text.lastIndexOf("/") + 1))
            window.toaster.show(qsTr("No application is set for this type of file. Choose one."), "info")
            window.chooseOpenWith(url, mimeType, name)
        }
    }

    // ---- keyboard shortcuts --------------------------------------------------------

    component Key: Shortcut {
        context: Qt.WindowShortcut
        autoRepeat: false
    }

    Key { sequences: ["Ctrl+T"]; enabled: !window.dialogOpen; onActivated: if (window.activePane) window.openInNewTab(window.activePane.folder.url, true) }
    Key { sequences: ["Ctrl+W"]; enabled: !window.dialogOpen; onActivated: window.closeTab(window.currentTab) }
    Key { sequences: ["Ctrl+Tab", "Ctrl+PgDown"]; enabled: !window.dialogOpen; onActivated: window.selectTab((window.currentTab + 1) % Math.max(1, tabs.count)) }
    Key { sequences: ["Ctrl+Shift+Tab", "Ctrl+PgUp"]; enabled: !window.dialogOpen; onActivated: window.selectTab((window.currentTab + tabs.count - 1) % Math.max(1, tabs.count)) }
    Key { sequences: ["Ctrl+L"]; enabled: !window.dialogOpen; onActivated: locationBar.edit() }
    Key { sequences: ["Ctrl+H", "Alt+."]; enabled: !window.dialogOpen; onActivated: prefs.showHidden = !prefs.showHidden }
    Key { sequences: ["F3"]; enabled: !window.dialogOpen; onActivated: window.toggleSplit() }
    Key { sequences: ["Ctrl+F"]; enabled: !window.dialogOpen; onActivated: { filterField.forceActiveFocus(); filterField.selectAll() } }
    Key { sequences: ["F5", "Ctrl+R"]; enabled: !window.dialogOpen; onActivated: if (window.activePane) window.activePane.folder.reload() }
    Key { sequences: ["Alt+Left"]; enabled: !window.dialogOpen; onActivated: if (window.activePane) window.activePane.folder.goBack() }
    Key { sequences: ["Alt+Right"]; enabled: !window.dialogOpen; onActivated: if (window.activePane) window.activePane.folder.goForward() }
    Key { sequences: ["Alt+Up"]; enabled: !window.dialogOpen; onActivated: if (window.activePane) window.activePane.folder.goUp() }
    Key { sequences: ["Alt+Home"]; enabled: !window.dialogOpen; onActivated: window.navigate(Places.homeUrl) }
    Key { sequences: ["Ctrl+Shift+N"]; enabled: !window.dialogOpen && window.activePane !== null && window.activePane.folder.writable; onActivated: window.newFolder(window.activePane) }
    Key { sequences: ["Ctrl+Z"]; enabled: !window.dialogOpen && FileOps.undoAvailable; onActivated: FileOps.undo() }
    Key { sequences: ["Alt+P", "F11"]; enabled: !window.dialogOpen; onActivated: prefs.previewPanel = !prefs.previewPanel }
    Key { sequences: ["Ctrl+1"]; enabled: !window.dialogOpen; onActivated: prefs.viewMode = "grid" }
    Key { sequences: ["Ctrl+2"]; enabled: !window.dialogOpen; onActivated: prefs.viewMode = "list" }
    Key { sequences: ["Ctrl++", "Ctrl+="]; enabled: !window.dialogOpen; autoRepeat: true; onActivated: window.zoom(1) }
    Key { sequences: ["Ctrl+-"]; enabled: !window.dialogOpen; autoRepeat: true; onActivated: window.zoom(-1) }
    Key { sequences: ["Ctrl+0"]; enabled: !window.dialogOpen; onActivated: prefs.iconSize = 64 }
    Key { sequences: ["F4"]; enabled: !window.dialogOpen && Launcher.terminalAvailable && window.activePane !== null && window.activePane.folder.local; onActivated: Launcher.openTerminal(window.activePane.folder.url) }
    Key { sequences: ["F6"]; enabled: !window.dialogOpen && window.currentPage !== null && window.currentPage.split; onActivated: window.currentPage.switchPane() }

    // ---- start-up ------------------------------------------------------------------

    Component.onCompleted: {
        if (window.iconSizes.indexOf(prefs.iconSize) < 0) prefs.iconSize = 64
        const locations = Launcher.startLocations
        for (let i = 0; i < locations.length; i++) window.openInNewTab(locations[i], false)
        const select = Launcher.selectOnStart
        if (select.toString() !== "") window.openInNewTab(select, false, select)
        if (tabs.count === 0) window.openInNewTab(Places.homeUrl, false)
        window.currentTab = select.toString() !== "" ? tabs.count - 1 : 0
        window.refreshPages()
        window.focusActivePane()
    }

    onActiveChanged: if (active) Places.refresh()

    // ---- layout --------------------------------------------------------------------

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Sidebar {
            Layout.fillHeight: true
            shell: window
            currentUrl: window.activePane ? window.activePane.folder.url : ""
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // Toolbar
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: Theme.headerHeight
                color: Theme.bgBase

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spaceSm
                    anchors.rightMargin: Theme.spaceSm
                    spacing: Theme.spaceXs

                    NxIconButton {
                        iconName: "arrow-left"
                        tooltip: qsTr("Back (Alt+Left)")
                        focusPolicy: Qt.TabFocus
                        enabled: window.activePane !== null && window.activePane.folder.canGoBack
                        onClicked: window.activePane.folder.goBack()
                    }
                    NxIconButton {
                        iconName: "arrow-right"
                        tooltip: qsTr("Forward (Alt+Right)")
                        focusPolicy: Qt.TabFocus
                        enabled: window.activePane !== null && window.activePane.folder.canGoForward
                        onClicked: window.activePane.folder.goForward()
                    }
                    NxIconButton {
                        iconName: "arrow-up"
                        tooltip: qsTr("Parent folder (Alt+Up)")
                        focusPolicy: Qt.TabFocus
                        enabled: window.activePane !== null && window.activePane.folder.canGoUp
                        onClicked: window.activePane.folder.goUp()
                    }
                    NxIconButton {
                        iconName: "refresh"
                        tooltip: qsTr("Reload (F5)")
                        focusPolicy: Qt.TabFocus
                        enabled: window.activePane !== null
                        onClicked: window.activePane.folder.reload()
                    }

                    LocationBar {
                        id: locationBar
                        Layout.fillWidth: true
                        Layout.leftMargin: Theme.spaceXs
                        Layout.rightMargin: Theme.spaceXs
                        shell: window
                        folder: window.activePane ? window.activePane.folder : null
                    }

                    NxSearchField {
                        id: filterField
                        Layout.preferredWidth: Math.max(Theme.sidebarWidth - Theme.spaceXxxl * 2, Math.min(Theme.sidebarWidth, window.width * 0.18))
                        placeholderText: qsTr("Filter — Enter searches subfolders")
                        enabled: window.activePane !== null && window.activePane.folder.error === ""
                        // The field shows the active pane's filter.
                        property var boundPane: window.activePane
                        onBoundPaneChanged: text = boundPane ? (boundPane.searching ? boundPane.results.query : boundPane.filterText) : ""
                        onTextEdited: {
                            if (!window.activePane) return
                            if (window.activePane.searching) window.activePane.leaveSearch()
                            window.activePane.filterText = text
                        }
                        // The clear button empties the field without an edit signal.
                        onTextChanged: {
                            const pane = window.activePane
                            if (text !== "" || !pane) return
                            if (pane.searching) pane.leaveSearch()
                            if (pane.filterText !== "") pane.filterText = ""
                        }
                        onAccepted: if (window.activePane && text.trim() !== "") window.activePane.startSearch(text)
                        Keys.onDownPressed: window.focusActivePane()
                        Connections {
                            target: window.activePane
                            function onFilterTextChanged() {
                                if (!window.activePane.searching && filterField.text !== window.activePane.filterText)
                                    filterField.text = window.activePane.filterText
                            }
                            function onSearchingChanged() {
                                if (!window.activePane.searching && window.activePane.filterText === "") filterField.text = ""
                            }
                        }
                    }

                    NxIconButton {
                        iconName: "grid"
                        tooltip: qsTr("Grid view (Ctrl+1)")
                        focusPolicy: Qt.TabFocus
                        checked: prefs.viewMode === "grid"
                        onClicked: prefs.viewMode = "grid"
                    }
                    NxIconButton {
                        iconName: "list"
                        tooltip: qsTr("Details view (Ctrl+2)")
                        focusPolicy: Qt.TabFocus
                        checked: prefs.viewMode !== "grid"
                        onClicked: prefs.viewMode = "list"
                    }
                    NxIconButton {
                        id: sortButton
                        iconName: "sort"
                        tooltip: qsTr("Sort order")
                        focusPolicy: Qt.TabFocus
                        onClicked: sortMenu.popup(sortButton, 0, sortButton.height + Theme.spaceXs)
                    }
                    NxIconButton {
                        iconName: "columns"
                        tooltip: qsTr("Split view (F3)")
                        focusPolicy: Qt.TabFocus
                        checked: window.currentPage !== null && window.currentPage.split
                        onClicked: window.toggleSplit()
                    }
                    NxIconButton {
                        iconName: "sidebar"
                        tooltip: qsTr("Preview panel (Alt+P)")
                        focusPolicy: Qt.TabFocus
                        checked: prefs.previewPanel
                        onClicked: prefs.previewPanel = !prefs.previewPanel
                    }

                    // Operations indicator
                    Rectangle {
                        id: opsPill
                        readonly property var jobs: FileOps.jobs
                        readonly property bool failedOnly: jobs.count > 0 && jobs.activeCount === 0
                        visible: jobs.count > 0
                        implicitWidth: opsRow.implicitWidth + Theme.spaceMd * 2
                        implicitHeight: Theme.controlHeight
                        radius: Theme.radiusSm
                        color: opsMouse.containsMouse || opsPopup.visible ? Theme.hoverOverlay : "transparent"
                        border.width: Theme.borderWidth
                        border.color: failedOnly ? Theme.alpha(Theme.danger, 0.6) : Theme.alpha(Theme.accent, 0.5)
                        activeFocusOnTab: true
                        Accessible.role: Accessible.Button
                        Accessible.name: failedOnly ? qsTr("Failed operations") : qsTr("Running operations")
                        Keys.onReturnPressed: opsPopup.open()
                        Keys.onSpacePressed: opsPopup.open()

                        RowLayout {
                            id: opsRow
                            anchors.centerIn: parent
                            spacing: Theme.spaceSm
                            BusyIndicator {
                                visible: opsPill.jobs.activeCount > 0
                                running: visible
                                implicitWidth: Theme.iconSmall + 2
                                implicitHeight: Theme.iconSmall + 2
                                padding: 0
                            }
                            NxIcon {
                                visible: opsPill.failedOnly
                                name: "alert-triangle"
                                size: Theme.iconSmall
                                color: Theme.danger
                            }
                            Text {
                                color: opsPill.failedOnly ? Theme.danger : Theme.textPrimary
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fontSizeSmall
                                font.weight: Theme.weightMedium
                                text: {
                                    const j = opsPill.jobs
                                    if (opsPill.failedOnly) return window.plural(j.count, qsTr("1 operation failed"), qsTr("%1 operations failed"))
                                    const what = window.plural(j.activeCount, qsTr("1 operation"), qsTr("%1 operations"))
                                    return j.overallProgress >= 0 ? what + " · " + Math.round(j.overallProgress * 100) + " %" : what
                                }
                            }
                        }
                        MouseArea {
                            id: opsMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: opsPopup.visible ? opsPopup.close() : opsPopup.open()
                        }

                        Popup {
                            id: opsPopup
                            x: opsPill.width - width
                            y: opsPill.height + Theme.spaceXs
                            width: Theme.sidebarWidth * 1.6
                            padding: Theme.spaceSm

                            Connections {
                                target: opsPill.jobs
                                function onCountChanged() {
                                    if (opsPill.jobs.count === 0) opsPopup.close()
                                }
                            }

                            contentItem: ColumnLayout {
                                spacing: Theme.spaceXs
                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: Theme.spaceSm
                                    Text {
                                        Layout.fillWidth: true
                                        text: qsTr("File operations")
                                        color: Theme.textPrimary
                                        font.family: Theme.fontUi
                                        font.pixelSize: Theme.fontSizeBody
                                        font.weight: Theme.weightSemibold
                                    }
                                    NxButton {
                                        visible: opsPill.jobs.count > opsPill.jobs.activeCount
                                        kind: "ghost"
                                        text: qsTr("Clear failed")
                                        onClicked: opsPill.jobs.clearFinished()
                                    }
                                }
                                ListView {
                                    id: opsList
                                    Layout.fillWidth: true
                                    implicitHeight: Math.min(contentHeight, Theme.rowHeightLarge * 5)
                                    model: opsPill.jobs
                                    clip: true
                                    spacing: Theme.spaceXs
                                    boundsBehavior: Flickable.StopAtBounds
                                    ScrollBar.vertical: ScrollBar { }

                                    delegate: Rectangle {
                                        id: op
                                        required property int opId
                                        required property string title
                                        required property string detail
                                        required property real progress
                                        required property string status
                                        required property string error
                                        required property bool cancellable
                                        width: opsList.width
                                        height: opColumn.implicitHeight + Theme.spaceSm * 2
                                        radius: Theme.radiusSm
                                        color: Theme.bgRaised
                                        border.width: Theme.borderWidth
                                        border.color: op.status === "error" ? Theme.alpha(Theme.danger, 0.5) : Theme.borderSubtle

                                        ColumnLayout {
                                            id: opColumn
                                            anchors.left: parent.left
                                            anchors.right: parent.right
                                            anchors.top: parent.top
                                            anchors.margins: Theme.spaceSm
                                            spacing: Theme.spaceXs
                                            RowLayout {
                                                Layout.fillWidth: true
                                                spacing: Theme.spaceSm
                                                Text {
                                                    Layout.fillWidth: true
                                                    text: op.status === "error" ? qsTr("Failed: %1").arg(op.title) : op.title
                                                    color: op.status === "error" ? Theme.danger : Theme.textPrimary
                                                    font.family: Theme.fontUi
                                                    font.pixelSize: Theme.fontSizeBody
                                                    font.weight: Theme.weightMedium
                                                    elide: Text.ElideMiddle
                                                }
                                                NxIconButton {
                                                    visible: op.cancellable || op.status === "error"
                                                    iconName: "close"
                                                    iconSize: Theme.iconSmall
                                                    tooltip: op.status === "error" ? qsTr("Dismiss") : qsTr("Cancel")
                                                    implicitWidth: Theme.iconLarge
                                                    implicitHeight: Theme.iconLarge
                                                    padding: 0
                                                    onClicked: {
                                                        if (op.status === "error") opsPill.jobs.dismiss(op.opId)
                                                        else opsPill.jobs.cancel(op.opId)
                                                    }
                                                }
                                            }
                                            ProgressBar {
                                                Layout.fillWidth: true
                                                visible: op.status === "running"
                                                indeterminate: op.progress < 0
                                                value: Math.max(0, op.progress)
                                            }
                                            Text {
                                                Layout.fillWidth: true
                                                visible: text !== ""
                                                text: op.status === "error" ? op.error : op.detail
                                                color: Theme.textSecondary
                                                font.family: Theme.fontUi
                                                font.pixelSize: Theme.fontSizeSmall
                                                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                                                maximumLineCount: op.status === "error" ? 6 : 1
                                                elide: Text.ElideRight
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    NxIconButton {
                        id: menuButton
                        iconName: "menu"
                        tooltip: qsTr("Menu")
                        focusPolicy: Qt.TabFocus
                        onClicked: mainMenu.popup(menuButton, menuButton.width - mainMenu.width, menuButton.height + Theme.spaceXs)
                    }
                }
            }

            // Tab bar (shown from the second tab on)
            Rectangle {
                Layout.fillWidth: true
                visible: tabs.count > 1
                implicitHeight: Theme.controlHeight + Theme.spaceXs
                color: Theme.bgSurface

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spaceSm
                    anchors.rightMargin: Theme.spaceSm
                    spacing: Theme.spaceXs

                    ListView {
                        id: tabStrip
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        orientation: ListView.Horizontal
                        spacing: Theme.spaceXs
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        model: tabs
                        currentIndex: window.currentTab

                        delegate: Rectangle {
                            id: tab
                            required property int index
                            readonly property var page: window.pages[index] ?? null
                            readonly property bool current: index === window.currentTab
                            width: Math.max(Theme.sidebarWidth * 0.5, Math.min(Theme.sidebarWidth * 0.85, (tabStrip.width - Theme.spaceXs * (tabs.count - 1)) / Math.max(1, tabs.count)))
                            height: tabStrip.height - Theme.spaceXs
                            y: Theme.spaceXs
                            radius: Theme.radiusSm
                            color: current ? Theme.bgRaised
                                : tabDrop.containsDrag ? Theme.accentFill
                                : tabMouse.containsMouse ? Theme.hoverOverlay : "transparent"
                            border.width: current ? Theme.borderWidth : 0
                            border.color: Theme.border
                            activeFocusOnTab: true
                            Accessible.role: Accessible.PageTab
                            Accessible.name: page ? page.title : ""
                            Keys.onReturnPressed: window.selectTab(index)
                            Keys.onSpacePressed: window.selectTab(index)

                            MouseArea {
                                id: tabMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                                onClicked: (mouse) => {
                                    if (mouse.button === Qt.MiddleButton) window.closeTab(tab.index)
                                    else window.selectTab(tab.index)
                                }
                            }
                            // Dragging files over a tab switches to it; dropping copies or moves into its folder.
                            DropArea {
                                id: tabDrop
                                anchors.fill: parent
                                keys: ["text/uri-list"]
                                onEntered: tabHover.restart()
                                onExited: tabHover.stop()
                                onDropped: (drop) => {
                                    tabHover.stop()
                                    if (tab.page) window.handleDrop(drop, tab.page.activePane.folder.url)
                                }
                            }
                            Timer {
                                id: tabHover
                                interval: 700
                                onTriggered: window.selectTab(tab.index)
                            }
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: Theme.spaceMd
                                anchors.rightMargin: Theme.spaceXs
                                spacing: Theme.spaceSm
                                NxIcon {
                                    name: tab.page ? tab.page.symbol : "folder"
                                    size: Theme.iconSmall
                                    color: tab.current ? Theme.accent : Theme.textMuted
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: tab.page ? tab.page.title : ""
                                    color: tab.current ? Theme.textPrimary : Theme.textSecondary
                                    font.family: Theme.fontUi
                                    font.pixelSize: Theme.fontSizeBody
                                    font.weight: tab.current ? Theme.weightSemibold : Theme.weightMedium
                                    elide: Text.ElideRight
                                }
                                NxIconButton {
                                    iconName: "close"
                                    iconSize: Theme.iconSmall - 2
                                    implicitWidth: Theme.iconLarge
                                    implicitHeight: Theme.iconLarge
                                    padding: 0
                                    tooltip: qsTr("Close tab (Ctrl+W)")
                                    focusPolicy: Qt.NoFocus
                                    onClicked: window.closeTab(tab.index)
                                }
                            }
                        }
                    }
                    NxIconButton {
                        iconName: "plus"
                        iconSize: Theme.iconSmall
                        tooltip: qsTr("New tab (Ctrl+T)")
                        focusPolicy: Qt.TabFocus
                        onClicked: if (window.activePane) window.openInNewTab(window.activePane.folder.url, true)
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: Theme.borderWidth
                color: Theme.borderSubtle
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                StackLayout {
                    id: stack
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: window.currentTab

                    Repeater {
                        id: tabRepeater
                        model: tabs
                        TabPage {
                            shell: window
                        }
                        onItemAdded: Qt.callLater(window.refreshPages)
                        onItemRemoved: Qt.callLater(window.refreshPages)
                    }
                }

                PreviewPanel {
                    Layout.fillHeight: true
                    visible: prefs.previewPanel
                    shell: window
                    pane: visible ? window.activePane : null
                }
            }
        }
    }
}
