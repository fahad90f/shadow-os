// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.files

// One view of a folder: grid or details list, selection, keyboard handling,
// inline rename, drag and drop, the filter / search bar and the status bar.
// A tab holds one pane, or two side by side in split view.
FocusScope {
    id: pane

    // The main window: preferences, menus, dialogs and tabs.
    required property var shell
    // This pane receives the toolbar's and the keyboard's actions.
    property bool active: true
    // Split view: show which folder this pane displays and which pane is active.
    property bool showHeader: false

    readonly property alias folder: folderModel
    readonly property alias results: searchModel
    readonly property string viewMode: shell.prefs.viewMode
    readonly property int iconSize: shell.prefs.iconSize
    readonly property Flickable view: searching ? resultsView : (viewMode === "grid" ? grid : list)

    property int currentIndex: -1
    property int anchorIndex: -1
    property url currentUrl
    property int renamingRow: -1
    property string filterText: ""
    property bool searching: false
    property string typeBuffer: ""
    property var bandBase: []

    signal activated()

    // ---- model ---------------------------------------------------------------------

    FolderModel {
        id: folderModel
        showHidden: pane.shell.prefs.showHidden
        sortRole: pane.shell.prefs.sortRole
        sortAscending: pane.shell.prefs.sortAscending
        foldersFirst: pane.shell.prefs.foldersFirst
        thumbnails: pane.viewMode === "grid"
        filter: pane.filterText

        onUrlChanged: {
            pane.currentIndex = -1
            pane.anchorIndex = -1
            pane.currentUrl = ""
            pane.renamingRow = -1
            pane.filterText = ""
            pane.leaveSearch()
        }
        onRevealRow: (row) => {
            pane.currentIndex = row
            pane.anchorIndex = row
            pane.currentUrl = folderModel.urlAt(row)
            pane.view.positionViewAtIndex(row, pane.viewMode === "grid" ? GridView.Contain : ListView.Contain)
        }
        // Rows move when the folder changes; keep "current" on the same item.
        onModelReset: pane.relocateCurrent()
        onRowsInserted: pane.relocateCurrent()
        onRowsRemoved: pane.relocateCurrent()
    }

    SearchModel { id: searchModel }

    Connections {
        target: FileOps
        function onItemsCreated(dir, urls) {
            if (pane.active && !pane.searching && FileOps.sameLocation(dir, folderModel.url))
                folderModel.selectWhenAvailable(urls)
        }
    }

    Timer {
        id: typeTimer
        interval: 1000
        onTriggered: pane.typeBuffer = ""
    }

    // ---- helpers -------------------------------------------------------------------

    function activate() {
        if (!pane.active) pane.activated()
    }

    function focusView() {
        pane.view.forceActiveFocus()
    }

    function relocateCurrent() {
        if (pane.renamingRow >= 0) pane.renamingRow = -1
        if (pane.currentUrl.toString() === "") { pane.currentIndex = -1; return }
        pane.currentIndex = folderModel.indexOfUrl(pane.currentUrl)
        if (pane.anchorIndex >= folderModel.count) pane.anchorIndex = pane.currentIndex
    }

    function setCurrent(row, select) {
        if (row < 0 || row >= folderModel.count) return
        pane.currentIndex = row
        pane.currentUrl = folderModel.urlAt(row)
        if (select) {
            folderModel.selectOnly(row)
            pane.anchorIndex = row
        }
        pane.view.positionViewAtIndex(row, pane.viewMode === "grid" ? GridView.Contain : ListView.Contain)
    }

    // ---- pointer -------------------------------------------------------------------

    function itemPressed(row, button, modifiers) {
        pane.activate()
        pane.focusView()
        if (pane.renamingRow >= 0 && pane.renamingRow !== row) pane.renamingRow = -1
        if (button === Qt.LeftButton) {
            if (modifiers & Qt.ControlModifier) {
                folderModel.toggle(row)
                pane.anchorIndex = row
            } else if (modifiers & Qt.ShiftModifier) {
                folderModel.selectRange(pane.anchorIndex >= 0 ? pane.anchorIndex : row, row, false)
            } else if (!folderModel.isSelected(row)) {
                folderModel.selectOnly(row)
                pane.anchorIndex = row
            }
            // Pressing an item that is already selected keeps the selection:
            // it may be the start of a drag. It collapses on release (itemClicked).
        } else if (button === Qt.RightButton) {
            if (!folderModel.isSelected(row)) {
                folderModel.selectOnly(row)
                pane.anchorIndex = row
            }
        }
        pane.currentIndex = row
        pane.currentUrl = folderModel.urlAt(row)
    }

    function itemClicked(row, button, modifiers) {
        if (button === Qt.LeftButton) {
            if (!(modifiers & (Qt.ControlModifier | Qt.ShiftModifier)) && folderModel.selectedCount > 1) {
                folderModel.selectOnly(row)
                pane.anchorIndex = row
            }
        } else if (button === Qt.MiddleButton) {
            if (folderModel.isDirAt(row)) pane.shell.openInNewTab(folderModel.urlAt(row), false)
        } else if (button === Qt.RightButton) {
            pane.shell.showItemMenu(pane)
        }
    }

    function selectInBand(x, y, w, h) {
        let rows = []
        const n = folderModel.count
        if (pane.viewMode === "grid") {
            const cw = grid.cellWidth, ch = grid.cellHeight, cols = grid.columns
            const c0 = Math.max(0, Math.floor(x / cw)), c1 = Math.min(cols - 1, Math.floor((x + w) / cw))
            const r0 = Math.max(0, Math.floor(y / ch)), r1 = Math.floor((y + h) / ch)
            for (let r = r0; r <= r1; r++) {
                for (let c = c0; c <= c1; c++) {
                    const i = r * cols + c
                    if (i < n) rows.push(i)
                }
            }
        } else {
            const rh = list.rowHeight
            const r0 = Math.max(0, Math.floor(y / rh)), r1 = Math.min(n - 1, Math.floor((y + h) / rh))
            for (let r = r0; r <= r1; r++) rows.push(r)
        }
        folderModel.selectRows(pane.bandBase.concat(rows))
    }

    function handleDrop(drop, destUrl) {
        if (!drop.hasUrls) { drop.accepted = false; return }
        const action = FileOps.dropAction(drop.urls, destUrl)
        if (action === "") { drop.accepted = false; return }
        FileOps.drop(drop.urls, destUrl)
        drop.accept(action === "copy" ? Qt.CopyAction : action === "link" ? Qt.LinkAction : Qt.MoveAction)
    }

    // ---- actions -------------------------------------------------------------------

    function openRow(row) {
        const item = folderModel.get(row)
        if (item.url === undefined) return
        if (folderModel.trash && !item.isDir) {
            pane.shell.toaster.show(qsTr("Restore “%1” from the Trash to open it.").arg(item.name), "info")
            return
        }
        if (item.isDir) folderModel.open(item.url)
        else Launcher.openFile(item.url, item.mimeType)
    }

    function openSelection() {
        const urls = folderModel.selectedUrls
        if (urls.length === 0) return
        if (urls.length === 1) { pane.openRow(folderModel.indexOfUrl(urls[0])); return }
        // Several items: folders open in tabs, files with their applications.
        let opened = 0
        for (let i = 0; i < urls.length && opened < 12; i++) {
            const item = folderModel.get(folderModel.indexOfUrl(urls[i]))
            if (item.isDir) pane.shell.openInNewTab(item.url, false)
            else if (!folderModel.trash) Launcher.openFile(item.url, item.mimeType)
            opened++
        }
    }

    function copySelection(cut) {
        const urls = folderModel.selectedUrls
        if (urls.length === 0) return
        FileOps.copyToClipboard(urls, cut)
        pane.shell.toaster.show(cut ? pane.shell.plural(urls.length, qsTr("1 item will be moved when pasted"), qsTr("%1 items will be moved when pasted"))
                                    : pane.shell.plural(urls.length, qsTr("1 item copied"), qsTr("%1 items copied")), "info")
    }

    function paste() {
        if (!FileOps.clipboardHasUrls) return
        if (!folderModel.writable) {
            pane.shell.toaster.show(qsTr("Nothing can be pasted here: this folder is not writable."), "warning")
            return
        }
        FileOps.paste(folderModel.url)
    }

    function trashSelection() {
        const urls = folderModel.selectedUrls
        if (urls.length === 0) return
        if (folderModel.trash || !Launcher.protocolAvailable("trash")) pane.shell.confirmDelete(urls)
        else FileOps.trash(urls)
    }

    function deleteSelection() {
        const urls = folderModel.selectedUrls
        if (urls.length > 0) pane.shell.confirmDelete(urls)
    }

    function startRename() {
        if (folderModel.trash || folderModel.selectedCount !== 1) return
        const row = folderModel.indexOfUrl(folderModel.selectedUrls[0])
        if (row < 0) return
        pane.setCurrent(row, false)
        pane.renamingRow = row
    }

    function finishRename(row, newName) {
        if (pane.renamingRow !== row) return
        pane.renamingRow = -1
        const item = folderModel.get(row)
        const name = newName.trim()
        pane.focusView()
        if (item.url === undefined || name === "" || name === item.name) return
        const problem = FileOps.validateName(name)
        if (problem !== "") { pane.shell.toaster.show(problem, "warning"); return }
        FileOps.rename(item.url, name)
    }

    function selectAll() {
        folderModel.selectAll()
    }

    // ---- search --------------------------------------------------------------------

    function startSearch(text) {
        const query = text.trim()
        if (query === "") return
        // `searching` first: the toolbar field keeps showing the query.
        pane.searching = true
        pane.filterText = ""
        searchModel.start(folderModel.url, query, pane.shell.prefs.showHidden)
        resultsView.currentIndex = -1
    }

    function leaveSearch() {
        if (!pane.searching) return
        searchModel.clear()
        pane.searching = false
    }

    function openResult(row) {
        const hit = searchModel.get(row)
        if (hit.url === undefined) return
        if (hit.isDir) folderModel.open(hit.url)
        else Launcher.openFile(hit.url, "")
    }

    function revealResult(row) {
        const hit = searchModel.get(row)
        if (hit.url !== undefined) folderModel.reveal(hit.url)
    }

    // ---- keyboard ------------------------------------------------------------------

    function moveCurrent(target, modifiers) {
        const n = folderModel.count
        if (n === 0) return
        const row = Math.max(0, Math.min(n - 1, target))
        if (modifiers & Qt.ShiftModifier) {
            if (pane.anchorIndex < 0) pane.anchorIndex = Math.max(0, pane.currentIndex)
            folderModel.selectRange(pane.anchorIndex, row, false)
            pane.setCurrent(row, false)
        } else if (modifiers & Qt.ControlModifier) {
            pane.setCurrent(row, false)
        } else {
            pane.setCurrent(row, true)
        }
    }

    function handleKey(event) {
        const mods = event.modifiers
        const ctrl = (mods & Qt.ControlModifier) !== 0
        const shift = (mods & Qt.ShiftModifier) !== 0
        const alt = (mods & Qt.AltModifier) !== 0
        const cur = pane.currentIndex
        const cols = pane.viewMode === "grid" ? grid.columns : 1
        const rowSize = pane.viewMode === "grid" ? grid.cellHeight : list.rowHeight
        const page = Math.max(1, Math.floor(pane.view.height / rowSize)) * cols
        event.accepted = true
        switch (event.key) {
        case Qt.Key_Left:
            if (alt) { event.accepted = false; return }
            if (cols > 1) pane.moveCurrent(cur < 0 ? 0 : cur - 1, mods)
            return
        case Qt.Key_Right:
            if (alt) { event.accepted = false; return }
            if (cols > 1) pane.moveCurrent(cur < 0 ? 0 : cur + 1, mods)
            return
        case Qt.Key_Up:
            if (alt) { event.accepted = false; return }
            pane.moveCurrent(cur < 0 ? 0 : (cur - cols >= 0 ? cur - cols : cur), mods)
            return
        case Qt.Key_Down:
            pane.moveCurrent(cur < 0 ? 0 : (cur + cols < folderModel.count ? cur + cols
                : (Math.floor(cur / cols) < Math.floor((folderModel.count - 1) / cols) ? folderModel.count - 1 : cur)), mods)
            return
        case Qt.Key_Home:
            if (alt) { event.accepted = false; return }
            pane.moveCurrent(0, mods)
            return
        case Qt.Key_End:
            pane.moveCurrent(folderModel.count - 1, mods)
            return
        case Qt.Key_PageUp:
            pane.moveCurrent(cur < 0 ? 0 : cur - page, mods)
            return
        case Qt.Key_PageDown:
            pane.moveCurrent(cur < 0 ? 0 : cur + page, mods)
            return
        case Qt.Key_Return:
        case Qt.Key_Enter:
            if (alt) pane.shell.showProperties(pane)
            else if (folderModel.selectedCount > 0) pane.openSelection()
            else if (cur >= 0) pane.openRow(cur)
            return
        case Qt.Key_Backspace:
            folderModel.goUp()
            return
        case Qt.Key_Delete:
            if (shift) pane.deleteSelection()
            else pane.trashSelection()
            return
        case Qt.Key_F2:
            pane.startRename()
            return
        case Qt.Key_Space:
            if (cur >= 0) { folderModel.toggle(cur); pane.anchorIndex = cur }
            return
        case Qt.Key_Escape:
            if (pane.filterText !== "") pane.filterText = ""
            else folderModel.clearSelection()
            return
        case Qt.Key_Menu: {
            // Keyboard: the menu opens at the current item, not at the pointer.
            const item = cur >= 0 ? pane.view.itemAtIndex(cur) : null
            const at = item ? item.mapToItem(null, item.width / 2, item.height / 2)
                            : pane.view.mapToItem(null, Theme.spaceXl, Theme.spaceXl)
            if (folderModel.selectedCount > 0) pane.shell.showItemMenu(pane, at)
            else pane.shell.showBackgroundMenu(pane, at)
            return
        }
        case Qt.Key_A:
            if (ctrl) { pane.selectAll(); return }
            break
        case Qt.Key_C:
            if (ctrl) { pane.copySelection(false); return }
            break
        case Qt.Key_X:
            if (ctrl) { pane.copySelection(true); return }
            break
        case Qt.Key_V:
            if (ctrl) { pane.paste(); return }
            break
        }
        // Type-ahead: jump to the first item whose name starts with what was typed.
        if (!ctrl && !alt && event.text.length === 1 && event.text.charCodeAt(0) >= 32 && event.text.charCodeAt(0) !== 127) {
            const fresh = pane.typeBuffer === ""
            pane.typeBuffer += event.text
            typeTimer.restart()
            const row = folderModel.findByPrefix(pane.typeBuffer, fresh ? cur + 1 : Math.max(0, cur))
            if (row >= 0) pane.setCurrent(row, true)
            return
        }
        event.accepted = false
    }

    // ---- drag source ---------------------------------------------------------------

    // Mouse handling of an item: press / click / double click, and the start of
    // a drag once the pointer has moved far enough with the left button down.
    component ItemMouseArea: MouseArea {
        id: area
        property real pressX: 0
        property real pressY: 0
        property bool armed: false
        signal dragStarted()

        acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
        hoverEnabled: true
        // After onPressed handlers of the instance: remember where the press was.
        onPressedChanged: {
            if (pressed && pressedButtons === Qt.LeftButton) {
                armed = true
                pressX = mouseX
                pressY = mouseY
            } else {
                armed = false
            }
        }
        onPositionChanged: (mouse) => {
            if (!armed || !pressed) return
            if (Math.abs(mouse.x - pressX) + Math.abs(mouse.y - pressY) >= Application.styleHints.startDragDistance) {
                armed = false
                area.dragStarted()
            }
        }
    }

    // The drag itself belongs to the pane, not to the item under the pointer:
    // the platform runs its own event loop until the drop, and the item (a view
    // delegate) may be gone by then — for example because it was just moved.
    Item {
        id: dragSource
        Drag.dragType: Drag.Automatic
        Drag.supportedActions: Qt.CopyAction | Qt.MoveAction | Qt.LinkAction
        Drag.hotSpot: Qt.point(Theme.iconLarge, Theme.iconLarge)
    }

    function requestDrag(iconName) {
        if (dragSource.Drag.active || folderModel.selectedCount === 0) return
        dragSource.Drag.imageSource = "image://nxtheme/" + encodeURIComponent(iconName)
        // Started from the event loop, outside the delegate's mouse handler.
        Qt.callLater(pane.beginDrag)
    }

    function beginDrag() {
        if (dragSource.Drag.active || folderModel.selectedCount === 0) return
        dragSource.Drag.mimeData = { "text/uri-list": FileOps.uriList(folderModel.selectedUrls) }
        dragSource.Drag.active = true       // returns when the drag has ended
    }

    // ---- inline rename editor ------------------------------------------------------

    component RenameField: TextField {
        id: editor
        required property string originalName
        required property bool isFolder
        signal done(string newName)
        signal cancelled()
        property bool finished: false

        text: originalName
        selectByMouse: true
        font.pixelSize: Theme.fontSizeSmall
        leftPadding: Theme.spaceXs
        rightPadding: Theme.spaceXs
        Accessible.name: qsTr("New name")

        Component.onCompleted: {
            forceActiveFocus()
            // Select the name without its extension.
            const dot = isFolder ? -1 : originalName.lastIndexOf(".")
            if (dot > 0) select(0, dot)
            else selectAll()
        }
        onAccepted: { finished = true; done(text) }
        Keys.onEscapePressed: { finished = true; cancelled() }
        onActiveFocusChanged: {
            if (!activeFocus && !finished) { finished = true; done(text) }
        }
    }

    // ---- layout --------------------------------------------------------------------

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Split view header: which folder, and whether this pane is the active one.
        Rectangle {
            id: header
            Layout.fillWidth: true
            visible: pane.showHeader
            implicitHeight: Theme.controlHeight
            color: pane.active ? Theme.accentFill : Theme.bgSurface

            Rectangle {
                anchors.top: parent.top
                width: parent.width
                height: Theme.focusRing
                color: Theme.accent
                visible: pane.active
            }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.spaceMd
                anchors.rightMargin: Theme.spaceMd
                spacing: Theme.spaceSm
                NxIcon {
                    name: folderModel.symbol
                    size: Theme.iconSmall
                    color: pane.active ? Theme.accent : Theme.textMuted
                }
                Text {
                    Layout.fillWidth: true
                    text: folderModel.location
                    color: pane.active ? Theme.textPrimary : Theme.textSecondary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                    font.weight: pane.active ? Theme.weightSemibold : Theme.weightMedium
                    elide: Text.ElideLeft
                }
            }
            TapHandler {
                onTapped: { pane.activate(); pane.focusView() }
            }
        }

        // Filter / search bar.
        Rectangle {
            id: filterBar
            Layout.fillWidth: true
            visible: pane.searching || pane.filterText !== ""
            implicitHeight: Theme.controlHeight + Theme.spaceSm
            color: Theme.bgSurface

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.spaceMd
                anchors.rightMargin: Theme.spaceSm
                spacing: Theme.spaceSm

                BusyIndicator {
                    visible: pane.searching && searchModel.running
                    running: visible
                    implicitWidth: Theme.icon
                    implicitHeight: Theme.icon
                    padding: 0
                }
                NxIcon {
                    visible: !(pane.searching && searchModel.running)
                    name: pane.searching ? "file-search" : "filter"
                    size: Theme.iconSmall
                    color: Theme.accent
                }
                Text {
                    Layout.fillWidth: true
                    color: Theme.textSecondary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                    elide: Text.ElideRight
                    text: {
                        if (!pane.searching)
                            return qsTr("Showing names that contain “%1” in this folder").arg(pane.filterText)
                        const where = folderModel.title
                        if (searchModel.running)
                            return qsTr("Searching “%1” and its subfolders for “%2” — %3 found, %4 checked")
                                .arg(where).arg(searchModel.query).arg(searchModel.count).arg(searchModel.scanned)
                        if (searchModel.error !== "")
                            return qsTr("The search stopped: %1").arg(searchModel.error)
                        if (searchModel.truncated)
                            return qsTr("More than %1 items in “%2” match “%3”; only the first %1 are shown")
                                .arg(searchModel.count).arg(where).arg(searchModel.query)
                        return pane.shell.plural(searchModel.count, qsTr("1 result"), qsTr("%1 results"))
                            + " " + qsTr("for “%1” in “%2” and its subfolders").arg(searchModel.query).arg(where)
                    }
                }
                NxButton {
                    visible: !pane.searching
                    kind: "ghost"
                    iconName: "file-search"
                    text: qsTr("Search subfolders")
                    focusPolicy: Qt.TabFocus
                    onClicked: pane.startSearch(pane.filterText)
                }
                NxButton {
                    visible: pane.searching && searchModel.running
                    kind: "ghost"
                    iconName: "stop"
                    text: qsTr("Stop")
                    focusPolicy: Qt.TabFocus
                    onClicked: searchModel.cancel()
                }
                NxIconButton {
                    iconName: "close"
                    iconSize: Theme.iconSmall
                    tooltip: pane.searching ? qsTr("Close search results") : qsTr("Clear filter")
                    focusPolicy: Qt.TabFocus
                    onClicked: {
                        if (pane.searching) pane.leaveSearch()
                        else pane.filterText = ""
                        pane.focusView()
                    }
                }
            }
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: Theme.borderWidth
                color: Theme.borderSubtle
            }
        }

        // The views.
        Item {
            id: viewArea
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            // Drops on the background go into the folder being shown.
            DropArea {
                id: paneDrop
                anchors.fill: parent
                enabled: !pane.searching
                keys: ["text/uri-list"]
                onDropped: (drop) => pane.handleDrop(drop, folderModel.url)
            }

            // Background: clear the selection, rubber band, context menu.
            MouseArea {
                id: background
                anchors.fill: parent
                enabled: !pane.searching
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                property real originX: 0
                property real originY: 0        // in content coordinates
                property bool banding: false

                onPressed: (mouse) => {
                    pane.activate()
                    pane.focusView()
                    pane.renamingRow = -1
                    if (mouse.button !== Qt.LeftButton) return
                    const keep = (mouse.modifiers & (Qt.ControlModifier | Qt.ShiftModifier)) !== 0
                    if (!keep) folderModel.clearSelection()
                    pane.bandBase = keep ? folderModel.selectedRows() : []
                    originX = mouse.x - pane.view.x
                    originY = mouse.y - pane.view.y + pane.view.contentY - pane.view.originY
                    banding = false
                }
                onPositionChanged: (mouse) => {
                    if (!(pressedButtons & Qt.LeftButton)) return
                    const cx = mouse.x - pane.view.x
                    const cy = mouse.y - pane.view.y + pane.view.contentY - pane.view.originY
                    if (!banding && Math.abs(cx - originX) + Math.abs(cy - originY) < Theme.spaceSm) return
                    banding = true
                    band.contentLeft = Math.min(cx, originX)
                    band.width = Math.abs(cx - originX)
                    band.contentTop = Math.min(cy, originY)
                    band.height = Math.abs(cy - originY)
                    pane.selectInBand(band.contentLeft, band.contentTop, band.width, band.height)
                }
                onReleased: banding = false
                onCanceled: banding = false
                onClicked: (mouse) => {
                    if (mouse.button === Qt.RightButton) pane.shell.showBackgroundMenu(pane)
                }
            }

            // ---- grid view ----
            GridView {
                id: grid
                anchors.fill: parent
                anchors.leftMargin: Theme.spaceSm
                anchors.topMargin: Theme.spaceSm
                visible: pane.viewMode === "grid" && !pane.searching
                model: visible ? folderModel : null
                readonly property int columns: Math.max(1, Math.floor((width - Theme.spaceSm) / (pane.iconSize + Theme.spaceXxxl + Theme.spaceLg)))
                cellWidth: Math.floor((width - Theme.spaceSm) / columns)
                cellHeight: pane.iconSize + Theme.spaceXxxl + Theme.spaceMd
                interactive: false
                keyNavigationEnabled: false
                focus: visible
                activeFocusOnTab: true
                boundsBehavior: Flickable.StopAtBounds
                cacheBuffer: cellHeight * 2
                ScrollBar.vertical: ScrollBar { }
                Accessible.role: Accessible.List
                Accessible.name: qsTr("Files in %1").arg(folderModel.title)

                Keys.onPressed: (event) => pane.handleKey(event)
                onActiveFocusChanged: if (activeFocus) pane.activate()

                WheelScroll {
                    flickable: grid
                    onZoomRequested: (direction) => pane.shell.zoom(direction)
                }

                delegate: Item {
                    id: tile
                    required property int index
                    required property string name
                    required property url url
                    required property bool isDir
                    required property bool isLink
                    required property bool isHidden
                    required property string iconName
                    required property bool selected
                    required property string thumbnail
                    required property bool readable

                    // A delegate that is being released has index -1.
                    readonly property bool current: index >= 0 && pane.currentIndex === index
                    readonly property bool renaming: index >= 0 && pane.renamingRow === index
                    readonly property bool cut: FileOps.cutRevision >= 0 && FileOps.isCut(url)

                    width: grid.cellWidth
                    height: grid.cellHeight
                    Accessible.role: Accessible.ListItem
                    Accessible.name: name
                    Accessible.selected: selected

                    Rectangle {
                        id: tileBackground
                        anchors.fill: parent
                        anchors.rightMargin: Theme.spaceSm
                        anchors.bottomMargin: Theme.spaceSm
                        radius: Theme.radiusLg
                        color: tile.selected ? Theme.accentFill
                            : tileDrop.containsDrag ? Theme.accentFill
                            : tileMouse.containsMouse ? Theme.hoverOverlay : "transparent"
                        border.width: tile.selected || tileDrop.containsDrag || (tile.current && grid.activeFocus) ? Theme.borderWidth : 0
                        border.color: tileDrop.containsDrag ? Theme.accent
                            : tile.selected ? Theme.alpha(Theme.accent, 0.55) : Theme.borderStrong
                        Behavior on color { ColorAnimation { duration: Theme.motionFast } }
                    }

                    Item {
                        id: iconBox
                        anchors.top: tileBackground.top
                        anchors.topMargin: Theme.spaceMd
                        anchors.horizontalCenter: tileBackground.horizontalCenter
                        width: Math.min(tileBackground.width - Theme.spaceMd, pane.iconSize + Theme.spaceXl)
                        height: pane.iconSize
                        opacity: tile.cut ? Theme.opacityDisabled : tile.isHidden ? 0.6 : 1

                        NxAppIcon {
                            anchors.centerIn: parent
                            visible: tile.thumbnail === ""
                            name: tile.iconName
                            size: pane.iconSize
                        }
                        // Backdrop and frame: drawings with a transparent background
                        // stay visible whatever their colour and the theme.
                        Rectangle {
                            visible: thumb.visible && thumb.status === Image.Ready
                            anchors.centerIn: parent
                            width: thumb.paintedWidth + 2
                            height: thumb.paintedHeight + 2
                            radius: Theme.radiusXs
                            color: Theme.alpha(Theme.textSecondary, 0.3)
                            border.width: Theme.borderWidth
                            border.color: Theme.borderStrong
                        }
                        Image {
                            id: thumb
                            anchors.fill: parent
                            anchors.margins: 1
                            visible: tile.thumbnail !== ""
                            source: tile.thumbnail
                            sourceSize: Qt.size(Math.round(parent.width * Screen.devicePixelRatio),
                                                Math.round(parent.height * Screen.devicePixelRatio))
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                            smooth: true
                        }
                        Rectangle {
                            visible: tile.isLink || !tile.readable
                            anchors.left: parent.left
                            anchors.bottom: parent.bottom
                            width: Theme.icon
                            height: Theme.icon
                            radius: Theme.radiusXs
                            color: Theme.bgOverlay
                            border.width: Theme.borderWidth
                            border.color: Theme.border
                            NxIcon {
                                anchors.centerIn: parent
                                name: tile.readable ? "link" : "lock"
                                size: Theme.iconSmall - 2
                                color: tile.readable ? Theme.accent : Theme.warning
                            }
                        }
                    }

                    Text {
                        id: tileLabel
                        anchors.top: iconBox.bottom
                        anchors.topMargin: Theme.spaceSm
                        anchors.left: tileBackground.left
                        anchors.right: tileBackground.right
                        anchors.leftMargin: Theme.spaceSm
                        anchors.rightMargin: Theme.spaceSm
                        visible: !tile.renaming
                        text: tile.name
                        color: tile.isHidden || tile.cut ? Theme.textSecondary : Theme.textPrimary
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeSmall
                        font.weight: tile.selected ? Theme.weightMedium : Theme.weightRegular
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                        maximumLineCount: 2
                        elide: Text.ElideRight
                    }

                    ItemMouseArea {
                        id: tileMouse
                        anchors.fill: tileBackground
                        onPressed: (mouse) => pane.itemPressed(tile.index, mouse.button, mouse.modifiers)
                        onClicked: (mouse) => pane.itemClicked(tile.index, mouse.button, mouse.modifiers)
                        onDoubleClicked: (mouse) => { if (mouse.button === Qt.LeftButton) pane.openRow(tile.index) }
                        onDragStarted: if (!tile.renaming) pane.requestDrag(tile.iconName)
                    }
                    DropArea {
                        id: tileDrop
                        anchors.fill: tileBackground
                        enabled: tile.isDir
                        keys: ["text/uri-list"]
                        onEntered: (drag) => { drag.accepted = FileOps.dropAction(drag.urls, tile.url) !== "" }
                        onDropped: (drop) => pane.handleDrop(drop, tile.url)
                    }

                    Loader {
                        anchors.top: iconBox.bottom
                        anchors.topMargin: Theme.spaceXs
                        anchors.left: tileBackground.left
                        anchors.right: tileBackground.right
                        anchors.leftMargin: Theme.spaceXs
                        anchors.rightMargin: Theme.spaceXs
                        active: tile.renaming
                        sourceComponent: RenameField {
                            originalName: tile.name
                            isFolder: tile.isDir
                            horizontalAlignment: TextInput.AlignHCenter
                            onDone: (newName) => pane.finishRename(tile.index, newName)
                            onCancelled: { pane.renamingRow = -1; pane.focusView() }
                        }
                    }
                }
            }

            // ---- details view ----
            Item {
                id: listHeader
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: Theme.controlHeight
                visible: list.visible

                Rectangle {
                    anchors.fill: parent
                    color: Theme.bgSurface
                }
                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: Theme.borderWidth
                    color: Theme.borderSubtle
                }
                Row {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spaceMd
                    anchors.rightMargin: Theme.spaceMd
                    Repeater {
                        model: [
                            { role: "name", title: qsTr("Name"), width: list.nameWidth, right: false, shown: true },
                            { role: "size", title: qsTr("Size"), width: list.sizeWidth, right: true, shown: true },
                            { role: "type", title: qsTr("Type"), width: list.typeWidth, right: false, shown: list.showType },
                            { role: "modified", title: qsTr("Modified"), width: list.dateWidth, right: false, shown: list.showDate },
                        ]
                        Item {
                            id: headCell
                            required property var modelData
                            readonly property bool sorted: pane.shell.prefs.sortRole === modelData.role
                            visible: modelData.shown
                            width: modelData.width
                            height: listHeader.height
                            Accessible.role: Accessible.ColumnHeader
                            Accessible.name: modelData.title

                            Row {
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.left: headCell.modelData.right ? undefined : parent.left
                                anchors.right: headCell.modelData.right ? parent.right : undefined
                                anchors.leftMargin: headCell.modelData.role === "name" ? Theme.icon + Theme.spaceMd : Theme.spaceSm
                                anchors.rightMargin: Theme.spaceMd
                                spacing: Theme.spaceXs
                                Text {
                                    text: headCell.modelData.title
                                    color: headCell.sorted ? Theme.textPrimary : Theme.textMuted
                                    font.family: Theme.fontUi
                                    font.pixelSize: Theme.fontSizeSmall
                                    font.weight: Theme.weightSemibold
                                }
                                NxIcon {
                                    visible: headCell.sorted
                                    anchors.verticalCenter: parent.verticalCenter
                                    name: pane.shell.prefs.sortAscending ? "chevron-up" : "chevron-down"
                                    size: Theme.iconSmall - 4
                                    color: Theme.accent
                                }
                            }
                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                            TapHandler {
                                onTapped: {
                                    if (headCell.sorted) {
                                        pane.shell.prefs.sortAscending = !pane.shell.prefs.sortAscending
                                    } else {
                                        pane.shell.prefs.sortRole = headCell.modelData.role
                                        // Newest and largest first by default.
                                        pane.shell.prefs.sortAscending = !(headCell.modelData.role === "size" || headCell.modelData.role === "modified")
                                    }
                                }
                            }
                        }
                    }
                }
            }

            ListView {
                id: list
                anchors.top: listHeader.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                visible: pane.viewMode !== "grid" && !pane.searching
                model: visible ? folderModel : null

                readonly property int rowHeight: Theme.controlHeight
                readonly property bool showType: width >= 620
                readonly property bool showDate: width >= 460
                readonly property real sizeWidth: 96
                readonly property real typeWidth: showType ? Math.min(220, width * 0.22) : 0
                readonly property real dateWidth: showDate ? 176 : 0
                readonly property real nameWidth: Math.max(120, width - Theme.spaceMd * 2 - sizeWidth - typeWidth - dateWidth)

                interactive: false
                keyNavigationEnabled: false
                focus: visible
                activeFocusOnTab: true
                boundsBehavior: Flickable.StopAtBounds
                cacheBuffer: rowHeight * 8
                ScrollBar.vertical: ScrollBar { }
                Accessible.role: Accessible.List
                Accessible.name: qsTr("Files in %1").arg(folderModel.title)

                Keys.onPressed: (event) => pane.handleKey(event)
                onActiveFocusChanged: if (activeFocus) pane.activate()

                WheelScroll {
                    flickable: list
                    onZoomRequested: (direction) => pane.shell.zoom(direction)
                }

                delegate: Item {
                    id: row
                    required property int index
                    required property string name
                    required property url url
                    required property bool isDir
                    required property bool isLink
                    required property bool isHidden
                    required property real size
                    required property string typeName
                    required property string modifiedText
                    required property string iconName
                    required property bool selected
                    required property bool readable

                    // A delegate that is being released has index -1.
                    readonly property bool current: index >= 0 && pane.currentIndex === index
                    readonly property bool renaming: index >= 0 && pane.renamingRow === index
                    readonly property bool cut: FileOps.cutRevision >= 0 && FileOps.isCut(url)

                    width: list.width
                    height: list.rowHeight
                    Accessible.role: Accessible.ListItem
                    Accessible.name: name
                    Accessible.selected: selected

                    Rectangle {
                        id: rowBackground
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spaceXs
                        anchors.rightMargin: Theme.spaceXs
                        radius: Theme.radiusSm
                        color: row.selected ? Theme.accentFill
                            : rowDrop.containsDrag ? Theme.accentFill
                            : rowMouse.containsMouse ? Theme.hoverOverlay : "transparent"
                        border.width: rowDrop.containsDrag || (row.current && list.activeFocus) ? Theme.borderWidth : 0
                        border.color: rowDrop.containsDrag ? Theme.accent : Theme.borderStrong
                    }

                    ItemMouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        onPressed: (mouse) => pane.itemPressed(row.index, mouse.button, mouse.modifiers)
                        onClicked: (mouse) => pane.itemClicked(row.index, mouse.button, mouse.modifiers)
                        onDoubleClicked: (mouse) => { if (mouse.button === Qt.LeftButton) pane.openRow(row.index) }
                        onDragStarted: if (!row.renaming) pane.requestDrag(row.iconName)
                    }
                    DropArea {
                        id: rowDrop
                        anchors.fill: parent
                        enabled: row.isDir
                        keys: ["text/uri-list"]
                        onEntered: (drag) => { drag.accepted = FileOps.dropAction(drag.urls, row.url) !== "" }
                        onDropped: (drop) => pane.handleDrop(drop, row.url)
                    }

                    Row {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spaceMd
                        anchors.rightMargin: Theme.spaceMd
                        opacity: row.cut ? Theme.opacityDisabled : 1

                        Item {
                            width: list.nameWidth
                            height: parent.height
                            FileIcon {
                                id: rowIcon
                                anchors.verticalCenter: parent.verticalCenter
                                name: row.iconName
                                size: Theme.icon + 2
                                opacity: row.isHidden ? 0.6 : 1
                            }
                            NxIcon {
                                visible: row.isLink || !row.readable
                                anchors.left: rowIcon.left
                                anchors.bottom: rowIcon.bottom
                                anchors.leftMargin: -2
                                anchors.bottomMargin: -2
                                name: row.readable ? "link" : "lock"
                                size: Theme.iconSmall - 5
                                color: row.readable ? Theme.accent : Theme.warning
                            }
                            Text {
                                anchors.left: rowIcon.right
                                anchors.leftMargin: Theme.spaceSm + 2
                                anchors.right: parent.right
                                anchors.rightMargin: Theme.spaceSm
                                anchors.verticalCenter: parent.verticalCenter
                                visible: !row.renaming
                                text: row.name
                                color: row.isHidden ? Theme.textSecondary : Theme.textPrimary
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fontSizeBody
                                elide: Text.ElideMiddle
                            }
                            Loader {
                                anchors.left: rowIcon.right
                                anchors.leftMargin: Theme.spaceXs
                                anchors.right: parent.right
                                anchors.rightMargin: Theme.spaceSm
                                anchors.verticalCenter: parent.verticalCenter
                                height: list.rowHeight - Theme.spaceXs
                                active: row.renaming
                                sourceComponent: RenameField {
                                    originalName: row.name
                                    isFolder: row.isDir
                                    font.pixelSize: Theme.fontSizeBody
                                    onDone: (newName) => pane.finishRename(row.index, newName)
                                    onCancelled: { pane.renamingRow = -1; pane.focusView() }
                                }
                            }
                        }
                        Text {
                            width: list.sizeWidth
                            height: parent.height
                            rightPadding: Theme.spaceMd
                            verticalAlignment: Text.AlignVCenter
                            horizontalAlignment: Text.AlignRight
                            text: row.isDir ? "—" : Theme.formatBytes(row.size)
                            color: Theme.textSecondary
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeSmall
                        }
                        Text {
                            visible: list.showType
                            width: list.typeWidth
                            height: parent.height
                            leftPadding: Theme.spaceSm
                            verticalAlignment: Text.AlignVCenter
                            text: row.typeName
                            color: Theme.textSecondary
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeSmall
                            elide: Text.ElideRight
                        }
                        Text {
                            visible: list.showDate
                            width: list.dateWidth
                            height: parent.height
                            leftPadding: Theme.spaceSm
                            verticalAlignment: Text.AlignVCenter
                            text: row.modifiedText
                            color: Theme.textSecondary
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeSmall
                            elide: Text.ElideRight
                        }
                    }
                }
            }

            // Rubber band.
            Rectangle {
                id: band
                property real contentLeft: 0
                property real contentTop: 0
                visible: background.banding
                x: contentLeft + pane.view.x
                y: contentTop - (pane.view.contentY - pane.view.originY) + pane.view.y
                color: Theme.alpha(Theme.accent, 0.12)
                border.width: Theme.borderWidth
                border.color: Theme.alpha(Theme.accent, 0.6)
                radius: Theme.radiusXs
            }

            // ---- search results ----
            ListView {
                id: resultsView
                anchors.fill: parent
                visible: pane.searching
                model: visible ? searchModel : null
                interactive: false
                keyNavigationEnabled: false
                focus: visible
                activeFocusOnTab: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { }
                Accessible.role: Accessible.List
                Accessible.name: qsTr("Search results")

                WheelScroll { flickable: resultsView }

                Keys.onPressed: (event) => {
                    const n = searchModel.count
                    event.accepted = true
                    if (event.key === Qt.Key_Down) currentIndex = Math.min(n - 1, currentIndex + 1)
                    else if (event.key === Qt.Key_Up) currentIndex = Math.max(0, currentIndex - 1)
                    else if (event.key === Qt.Key_Home) currentIndex = n > 0 ? 0 : -1
                    else if (event.key === Qt.Key_End) currentIndex = n - 1
                    else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && currentIndex >= 0) pane.openResult(currentIndex)
                    else if (event.key === Qt.Key_Escape) { pane.leaveSearch(); pane.focusView(); return }
                    else { event.accepted = false; return }
                    if (currentIndex >= 0) positionViewAtIndex(currentIndex, ListView.Contain)
                }
                onActiveFocusChanged: if (activeFocus) pane.activate()

                delegate: Item {
                    id: hit
                    required property int index
                    required property string name
                    required property url url
                    required property bool isDir
                    required property string folder
                    required property string iconName
                    required property real size
                    required property string modifiedText

                    width: resultsView.width
                    height: Theme.rowHeight
                    Accessible.role: Accessible.ListItem
                    Accessible.name: name

                    Rectangle {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spaceXs
                        anchors.rightMargin: Theme.spaceXs
                        radius: Theme.radiusSm
                        color: resultsView.currentIndex === hit.index ? Theme.accentFill
                            : hitMouse.containsMouse ? Theme.hoverOverlay : "transparent"
                    }
                    MouseArea {
                        id: hitMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onPressed: (mouse) => {
                            pane.activate()
                            resultsView.forceActiveFocus()
                            resultsView.currentIndex = hit.index
                        }
                        onClicked: (mouse) => {
                            if (mouse.button === Qt.RightButton) pane.shell.showResultMenu(pane, hit.index)
                        }
                        onDoubleClicked: (mouse) => { if (mouse.button === Qt.LeftButton) pane.openResult(hit.index) }
                    }
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spaceMd
                        anchors.rightMargin: Theme.spaceMd
                        spacing: Theme.spaceMd
                        FileIcon {
                            name: hit.iconName
                            size: Theme.iconLarge
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Text {
                                Layout.fillWidth: true
                                text: hit.name
                                color: Theme.textPrimary
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fontSizeBody
                                elide: Text.ElideMiddle
                            }
                            Text {
                                Layout.fillWidth: true
                                text: hit.folder === "" ? qsTr("in this folder") : hit.folder
                                color: Theme.textMuted
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fontSizeCaption
                                elide: Text.ElideLeft
                            }
                        }
                        Text {
                            visible: !hit.isDir && hit.size >= 0
                            text: Theme.formatBytes(hit.size)
                            color: Theme.textSecondary
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeSmall
                        }
                        Text {
                            visible: resultsView.width >= 520
                            Layout.preferredWidth: 150
                            text: hit.modifiedText
                            color: Theme.textSecondary
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeSmall
                            elide: Text.ElideRight
                        }
                        NxIconButton {
                            iconName: "folder-open"
                            iconSize: Theme.iconSmall
                            tooltip: qsTr("Show in its folder")
                            focusPolicy: Qt.NoFocus
                            onClicked: pane.revealResult(hit.index)
                        }
                    }
                }
            }

            // ---- states: loading, error, empty ----
            BusyIndicator {
                anchors.centerIn: parent
                running: visible
                visible: !pane.searching && folderModel.loading && folderModel.count === 0
            }

            NxEmptyState {
                anchors.fill: parent
                visible: !pane.searching && folderModel.error !== ""
                tone: "danger"
                icon: "x-circle"
                title: folderModel.error
                message: folderModel.errorDetail
                actionText: folderModel.canGoBack ? qsTr("Go back") : qsTr("Go to Home")
                onActionTriggered: {
                    if (folderModel.canGoBack) folderModel.goBack()
                    else folderModel.open(Places.homeUrl)
                }
            }

            NxEmptyState {
                anchors.fill: parent
                visible: !pane.searching && folderModel.error === "" && !folderModel.loading && folderModel.count === 0
                icon: folderModel.trash ? "trash" : pane.filterText !== "" ? "filter" : "folder-open"
                title: folderModel.trash && pane.filterText === "" ? qsTr("The Trash is empty")
                    : pane.filterText !== "" ? qsTr("Nothing in this folder matches “%1”").arg(pane.filterText)
                    : qsTr("This folder is empty")
                message: pane.filterText !== "" ? qsTr("Search the subfolders of “%1” as well?").arg(folderModel.title)
                    : folderModel.hiddenCount > 0 ? pane.shell.plural(folderModel.hiddenCount, qsTr("1 hidden item is not shown."), qsTr("%1 hidden items are not shown.")) : ""
                actionText: pane.filterText !== "" ? qsTr("Search subfolders")
                    : folderModel.hiddenCount > 0 ? qsTr("Show hidden files") : ""
                onActionTriggered: {
                    if (pane.filterText !== "") pane.startSearch(pane.filterText)
                    else pane.shell.prefs.showHidden = true
                }
            }

            NxEmptyState {
                anchors.fill: parent
                visible: pane.searching && !searchModel.running && searchModel.count === 0
                tone: searchModel.error !== "" ? "danger" : "neutral"
                icon: "file-search"
                title: searchModel.error !== "" ? qsTr("The search could not be completed")
                                                : qsTr("No file names contain “%1”").arg(searchModel.query)
                message: searchModel.error !== "" ? searchModel.error
                    : qsTr("%1 items below “%2” were checked.").arg(searchModel.scanned).arg(folderModel.title)
                actionText: qsTr("Back to the folder")
                onActionTriggered: { pane.leaveSearch(); pane.focusView() }
            }

            // Drop highlight for the whole pane.
            Rectangle {
                anchors.fill: parent
                visible: paneDrop.containsDrag
                color: "transparent"
                border.width: Theme.focusRing
                border.color: Theme.accent
                radius: Theme.radiusSm
            }
        }

        // Status bar.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: Theme.controlHeight - Theme.spaceXs
            color: Theme.bgSurface

            Rectangle {
                width: parent.width
                height: Theme.borderWidth
                color: Theme.borderSubtle
            }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.spaceMd
                anchors.rightMargin: Theme.spaceMd
                spacing: Theme.spaceMd

                Text {
                    Layout.fillWidth: true
                    color: Theme.textSecondary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                    elide: Text.ElideRight
                    text: {
                        const f = folderModel
                        if (pane.searching)
                            return searchModel.running ? qsTr("Searching…") : pane.shell.plural(searchModel.count, qsTr("1 result"), qsTr("%1 results"))
                        if (f.error !== "") return ""
                        if (f.selectedCount > 0) {
                            let s = qsTr("%1 of %2 selected").arg(f.selectedCount).arg(f.count)
                            if (f.selectedCount > f.selectedFolders) s += " · " + Theme.formatBytes(f.selectedSize)
                            return s
                        }
                        if (f.loading && f.count === 0) return qsTr("Loading…")
                        let s = pane.shell.plural(f.count, qsTr("1 item"), qsTr("%1 items"))
                        if (f.count > 0 && f.folderCount > 0 && f.fileCount > 0)
                            s += " (" + pane.shell.plural(f.folderCount, qsTr("1 folder"), qsTr("%1 folders")) + ", "
                                + pane.shell.plural(f.fileCount, qsTr("1 file"), qsTr("%1 files")) + ")"
                        if (f.filter !== "") s = qsTr("%1 of %2 items shown").arg(f.count).arg(f.totalCount - f.hiddenCount)
                        if (f.hiddenCount > 0) s += " · " + qsTr("%1 hidden").arg(f.hiddenCount)
                        return s
                    }
                }
                // Why "New folder", "Paste" and the like are not offered here.
                RowLayout {
                    visible: !folderModel.writable && !folderModel.loading && folderModel.error === ""
                        && !folderModel.trash && !pane.searching
                    spacing: Theme.spaceXs
                    NxIcon {
                        name: "lock"
                        size: Theme.iconSmall - 2
                        color: Theme.warning
                    }
                    Text {
                        color: Theme.warning
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeSmall
                        text: qsTr("Read-only")
                    }
                }
                Text {
                    visible: folderModel.spaceKnown && !pane.searching
                    color: Theme.textMuted
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                    text: qsTr("%1 free of %2").arg(Theme.formatBytes(folderModel.freeSpace)).arg(Theme.formatBytes(folderModel.totalSpace))
                }
                Rectangle {
                    visible: folderModel.spaceKnown && !pane.searching
                    implicitWidth: Theme.spaceXxxl + Theme.spaceLg
                    implicitHeight: Theme.spaceXs
                    radius: height / 2
                    color: Theme.bgPressed
                    Rectangle {
                        readonly property real used: folderModel.totalSpace > 0 ? 1 - folderModel.freeSpace / folderModel.totalSpace : 0
                        width: parent.width * Math.max(0, Math.min(1, used))
                        height: parent.height
                        radius: parent.radius
                        color: used >= 0.92 ? Theme.danger : used >= 0.8 ? Theme.warning : Theme.accent
                    }
                }
            }
        }
    }
}
