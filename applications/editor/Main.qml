// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.editor

// NEXUS Text Editor — main window: toolbar, tabs, the editor views, find
// bar, status bar and the dialogs.
NxWindow {
    id: window

    readonly property var currentView: views[currentTab] ?? null
    readonly property var currentDocument: currentView ? currentView.document : null

    title: currentDocument
        ? qsTr("%1%2 — Text Editor").arg(currentDocument.modified ? "● " : "").arg(currentDocument.fileName)
        : qsTr("Text Editor")
    width: 1180
    height: 780

    // ---- preferences: ~/.config/nexus/editor.conf, [Editor] ---------------------
    QtObject {
        id: prefs
        property string fontFamily: Theme.fontMono
        property int fontSize: 11                   // points
        property bool wordWrap: false
        property bool lineNumbers: true
        property bool highlightLine: true
        property bool useSpaces: true
        property int indentWidth: 4
        property bool autoIndent: true
        property bool restoreSession: true
        property string lastFolder: ""
    }
    Settings {
        category: "Editor"
        property alias fontFamily: prefs.fontFamily
        property alias fontSize: prefs.fontSize
        property alias wordWrap: prefs.wordWrap
        property alias lineNumbers: prefs.lineNumbers
        property alias highlightLine: prefs.highlightLine
        property alias useSpaces: prefs.useSpaces
        property alias indentWidth: prefs.indentWidth
        property alias autoIndent: prefs.autoIndent
        property alias restoreSession: prefs.restoreSession
        property alias lastFolder: prefs.lastFolder
    }

    // ---- tabs ----------------------------------------------------------------------
    ListModel { id: tabs }
    property int currentTab: 0
    property var views: []
    property int nextTabId: 1
    property bool started: false

    function refreshViews() {
        const list = []
        for (let i = 0; i < tabRepeater.count; i++) {
            const item = tabRepeater.itemAt(i)
            if (item) list.push(item)
        }
        views = list
        if (currentTab >= list.length) currentTab = Math.max(0, list.length - 1)
    }
    property int tabBeforeNew: 0
    function newTab() {
        tabBeforeNew = currentTab
        tabs.append({ tabId: nextTabId++ })
        refreshViews()
        currentTab = tabs.count - 1
        focusEditor()
        return views[currentTab]
    }
    function focusEditor() {
        if (currentView) currentView.editor.forceActiveFocus()
    }
    function closeTab(index) {
        tabs.remove(index)
        if (index < currentTab) currentTab -= 1
        refreshViews()
        if (tabs.count === 0) newTab()
        focusEditor()
        storeSession()
    }
    function indexOfPath(path) {
        for (let i = 0; i < views.length; i++)
            if (views[i].document.filePath === path) return i
        return -1
    }
    function isPristine(view) {
        return view && view.document.filePath === "" && !view.document.modified
            && view.editor.length === 0 && !view.document.loading
    }

    // ---- opening -------------------------------------------------------------------
    function openFile(rawPath, position) {
        let path = EditorBackend.absolutePath(rawPath)
        let line = 0
        if (!EditorBackend.exists(path)) {
            const m = /^(.*):(\d+)$/.exec(path)
            if (m && EditorBackend.exists(m[1])) { path = m[1]; line = parseInt(m[2]) }
        }
        const existing = indexOfPath(path)
        if (existing >= 0) {
            currentTab = existing
            if (line > 0) currentView.goToLine(line, 1)
            focusEditor()
            return
        }
        const view = isPristine(currentView) ? currentView : newTab()
        view.open(path, line, position)
    }
    function openFiles(paths) {
        for (const path of paths) openFile(path)
    }
    function storeSession() {
        if (!started) return
        const files = []
        let current = 0
        for (let i = 0; i < views.length; i++) {
            const d = views[i].document
            if (d.filePath === "") continue
            if (i === currentTab) current = files.length
            files.push({ path: d.filePath, position: views[i].editor.cursorPosition })
        }
        EditorBackend.saveSession(files, current)
    }

    // ---- saving --------------------------------------------------------------------
    function saveView(view, then) {
        if (!view) return
        const d = view.document
        if (d.filePath === "") { saveViewAs(view, then); return }
        d.checkDisk()
        if (d.changedOnDisk) {
            overwriteDialog.view = view
            overwriteDialog.then = then ?? null
            overwriteDialog.open()
            return
        }
        view.afterSave = () => { window.afterSaved(view); if (then) then() }
        d.save()
    }
    function saveViewAs(view, then) {
        if (!view) return
        savePicker.view = view
        savePicker.then = then ?? null
        savePicker.folder = view.document.filePath !== "" ? EditorBackend.parentPath(view.document.filePath) : prefs.lastFolder
        savePicker.suggestedName = view.document.filePath !== "" ? view.document.fileName : ""
        savePicker.open()
    }
    function afterSaved(view) {
        EditorBackend.addRecentFile(view.document.filePath)
        toaster.show(qsTr("Saved %1").arg(view.document.fileName), "success")
        storeSession()
    }
    function saveAll() {
        for (const view of views)
            if (view.document.modified && view.document.filePath !== "") saveView(view)
    }

    // ---- closing -------------------------------------------------------------------
    function requestCloseTab(index) {
        const view = views[index]
        if (!view) return
        if (view.document.modified) {
            currentTab = index
            unsavedDialog.view = view
            unsavedDialog.closingWindow = false
            unsavedDialog.open()
        } else {
            closeTab(index)
        }
    }
    function firstUnsaved() {
        for (let i = 0; i < views.length; i++)
            if (views[i].document.modified && !views[i].discardOnClose) return i
        return -1
    }
    onClosing: (close) => {
        const index = firstUnsaved()
        if (index >= 0) {
            close.accepted = false
            currentTab = index
            unsavedDialog.view = views[index]
            unsavedDialog.closingWindow = true
            unsavedDialog.open()
        } else {
            storeSession()
        }
    }

    // ---- start-up ------------------------------------------------------------------
    Component.onCompleted: {
        if (prefs.restoreSession && !EditorBackend.benchmark) {
            const session = EditorBackend.session()
            for (const file of session.files) openFile(file.path, file.position)
            if (session.files.length > 0) currentTab = Math.min(session.current, views.length - 1)
        }
        openFiles(nexusApp.arguments)
        if (tabs.count === 0) newTab()
        started = true
        focusEditor()
    }
    Connections {
        target: nexusApp
        function onActivationRequested(arguments) {
            window.openFiles(arguments)
            window.raise()
            window.requestActivate()
        }
    }
    // Coming back to the window is when an external change matters.
    onActiveChanged: if (active) { for (const view of views) view.document.checkDisk() }

    // ---- measurement (NEXUS_EDITOR_BENCH=1) ---------------------------------------
    // Each step changes something and is timed until the next frame is on screen.
    QtObject {
        id: bench
        property int step: -1
        property double started: 0
        property double launched: Date.now()
        property var results: []
        readonly property var steps: [
            { name: "first frame after load", run: () => {} },
            { name: "type one character at the start", run: () => window.currentView.editor.insert(0, "x") },
            { name: "undo", run: () => window.currentView.editor.undo() },
            { name: "jump to the end", run: () => { window.currentView.editor.cursorPosition = window.currentView.editor.length } },
            { name: "type one character at the end", run: () => window.currentView.editor.insert(window.currentView.editor.length, "y") },
            { name: "jump to the middle", run: () => window.currentView.goToLine(Math.floor(window.currentDocument.lineCount / 2), 1) },
            { name: "toggle word wrap", run: () => { prefs.wordWrap = !prefs.wordWrap } },
            { name: "toggle word wrap back", run: () => { prefs.wordWrap = !prefs.wordWrap } },
        ]
        function begin() {
            console.log("BENCH file:", window.currentDocument.filePath, "lines:", window.currentDocument.lineCount,
                        "characters:", window.currentView.editor.length, "highlighting:", window.currentDocument.highlighting)
            console.log("BENCH launch to text in document:", Date.now() - launched, "ms; inserting the text:",
                        window.currentDocument.insertMilliseconds, "ms")
            next()
        }
        function next() {
            step += 1
            if (step >= steps.length) { Qt.quit(); return }
            started = Date.now()
            steps[step].run()
            waiting = true
            window.currentView.editor.update()
        }
        property bool waiting: false
        function frame() {
            if (!waiting || step < 0 || step >= steps.length) return
            waiting = false
            console.log("BENCH", steps[step].name + ":", Date.now() - started, "ms")
            Qt.callLater(next)
        }
    }
    onFrameSwapped: if (EditorBackend.benchmark && bench.step >= 0) bench.frame()

    // ---- shortcuts -----------------------------------------------------------------
    Shortcut { sequences: [StandardKey.New]; onActivated: window.newTab() }
    Shortcut { sequences: [StandardKey.Open]; onActivated: openPicker.show() }
    Shortcut { sequences: [StandardKey.Save]; onActivated: window.saveView(window.currentView) }
    Shortcut { sequence: "Ctrl+Shift+S"; onActivated: window.saveViewAs(window.currentView) }
    Shortcut { sequence: "Ctrl+W"; onActivated: window.requestCloseTab(window.currentTab) }
    Shortcut { sequence: "Ctrl+Q"; onActivated: window.close() }
    Shortcut { sequences: [StandardKey.Find]; onActivated: findBar.show(false) }
    Shortcut { sequence: "Ctrl+H"; onActivated: findBar.show(true) }
    Shortcut { sequence: "F3"; onActivated: findBar.visible ? findBar.step(false) : findBar.show(false) }
    Shortcut { sequence: "Shift+F3"; onActivated: findBar.visible ? findBar.step(true) : findBar.show(false) }
    Shortcut { sequence: "Ctrl+G"; onActivated: goToDialog.open() }
    Shortcut { sequence: "Ctrl+Tab"; onActivated: window.currentTab = (window.currentTab + 1) % tabs.count }
    Shortcut { sequence: "Ctrl+Shift+Tab"; onActivated: window.currentTab = (window.currentTab + tabs.count - 1) % tabs.count }
    Shortcut { sequences: ["Ctrl+=", "Ctrl++"]; onActivated: prefs.fontSize = Math.min(48, prefs.fontSize + 1) }
    Shortcut { sequence: "Ctrl+-"; onActivated: prefs.fontSize = Math.max(6, prefs.fontSize - 1) }
    Shortcut { sequence: "Ctrl+0"; onActivated: prefs.fontSize = 11 }
    Shortcut { sequence: "Alt+Z"; onActivated: prefs.wordWrap = !prefs.wordWrap }
    Shortcut { sequence: "Ctrl+,"; onActivated: preferencesDialog.open() }
    Shortcut {
        sequence: "Escape"
        enabled: findBar.visible
        onActivated: { findBar.hide(); window.focusEditor() }
    }

    onCurrentTabChanged: Qt.callLater(window.focusEditor)

    // ---- layout --------------------------------------------------------------------
    DropArea {
        anchors.fill: parent
        onDropped: (drop) => {
            if (!drop.hasUrls) return
            for (const url of drop.urls) {
                const path = EditorBackend.localPath(url)
                if (path === "") window.toaster.show(qsTr("Only local files can be opened"), "warning")
                else if (EditorBackend.isDirectory(path)) window.toaster.show(qsTr("“%1” is a folder").arg(path), "warning")
                else window.openFile(path)
            }
            drop.accept()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // toolbar
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.headerHeight
            color: Theme.bgSurface

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.spaceMd
                anchors.rightMargin: Theme.spaceMd
                spacing: Theme.spaceXs

                NxIconButton { iconName: "plus"; tooltip: qsTr("New (Ctrl+N)"); onClicked: window.newTab() }
                NxIconButton { iconName: "folder-open"; tooltip: qsTr("Open… (Ctrl+O)"); onClicked: openPicker.show() }
                NxIconButton {
                    id: recentButton
                    iconName: "clock"
                    tooltip: qsTr("Recent files")
                    onClicked: recentMenu.popup(recentButton, 0, recentButton.height)
                }
                NxIconButton {
                    iconName: "save"
                    tooltip: qsTr("Save (Ctrl+S)")
                    enabled: window.currentDocument !== null && (window.currentDocument.modified || window.currentDocument.filePath === "")
                        && !window.currentDocument.saving
                    onClicked: window.saveView(window.currentView)
                }
                NxDivider { vertical: true; Layout.preferredHeight: Theme.icon; Layout.margins: Theme.spaceXs }
                NxIconButton {
                    iconName: "undo"
                    tooltip: qsTr("Undo (Ctrl+Z)")
                    enabled: window.currentView !== null && window.currentView.editor.canUndo
                    onClicked: { window.currentView.editor.undo(); window.focusEditor() }
                }
                NxIconButton {
                    iconName: "redo"
                    tooltip: qsTr("Redo (Ctrl+Shift+Z)")
                    enabled: window.currentView !== null && window.currentView.editor.canRedo
                    onClicked: { window.currentView.editor.redo(); window.focusEditor() }
                }
                NxDivider { vertical: true; Layout.preferredHeight: Theme.icon; Layout.margins: Theme.spaceXs }
                NxIconButton {
                    iconName: "search"
                    tooltip: qsTr("Find and replace (Ctrl+F, Ctrl+H)")
                    checkable: true
                    checked: findBar.visible
                    onClicked: findBar.visible ? findBar.hide() : findBar.show(false)
                }
                NxIconButton {
                    iconName: "rows"
                    tooltip: qsTr("Word wrap (Alt+Z)")
                    checkable: true
                    checked: prefs.wordWrap
                    onClicked: prefs.wordWrap = !prefs.wordWrap
                }
                Item { Layout.fillWidth: true }
                NxIconButton {
                    id: menuButton
                    iconName: "more-vertical"
                    tooltip: qsTr("More")
                    onClicked: mainMenu.popup(menuButton, 0, menuButton.height)
                }
            }
        }

        // tab strip
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.controlHeight + Theme.spaceXs
            color: Theme.bgSurface

            ListView {
                id: tabStrip
                anchors.fill: parent
                anchors.leftMargin: Theme.spaceSm
                anchors.rightMargin: Theme.spaceSm
                orientation: ListView.Horizontal
                spacing: Theme.spaceXxs
                clip: true
                model: tabs
                currentIndex: window.currentTab
                boundsBehavior: Flickable.StopAtBounds
                highlightMoveDuration: 0

                delegate: Rectangle {
                    id: tabItem
                    required property int index
                    readonly property var view: window.views[index] ?? null
                    readonly property var document: view ? view.document : null
                    readonly property bool current: index === window.currentTab
                    width: Math.min(260, tabRow.implicitWidth + Theme.spaceMd * 2)
                    height: tabStrip.height
                    radius: Theme.radiusSm
                    color: current ? Theme.bgBase : tabHover.hovered ? Theme.hoverOverlay : "transparent"
                    Accessible.role: Accessible.PageTab
                    Accessible.name: document ? document.fileName : ""

                    HoverHandler { id: tabHover }
                    TapHandler { onTapped: window.currentTab = tabItem.index }
                    TapHandler { acceptedButtons: Qt.MiddleButton; onTapped: window.requestCloseTab(tabItem.index) }
                    ToolTip.visible: tabHover.hovered && tabItem.document !== null && tabItem.document.filePath !== ""
                    ToolTip.text: tabItem.document ? tabItem.document.filePath : ""
                    ToolTip.delay: 600

                    RowLayout {
                        id: tabRow
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spaceMd
                        anchors.rightMargin: Theme.spaceXs
                        spacing: Theme.spaceXs
                        Rectangle {
                            visible: tabItem.document !== null && tabItem.document.modified
                            implicitWidth: Theme.spaceSm
                            implicitHeight: Theme.spaceSm
                            radius: Theme.spaceXs
                            color: Theme.accent
                            Accessible.name: qsTr("Unsaved changes")
                        }
                        Label {
                            Layout.fillWidth: true
                            text: tabItem.document ? tabItem.document.fileName : ""
                            color: tabItem.current ? Theme.textPrimary : Theme.textSecondary
                            font.weight: tabItem.current ? Theme.weightSemibold : Theme.weightRegular
                            elide: Text.ElideMiddle
                        }
                        NxIconButton {
                            iconName: "close"
                            iconSize: Theme.iconSmall
                            implicitWidth: Theme.iconLarge
                            implicitHeight: Theme.iconLarge
                            padding: Theme.spaceXxs
                            tooltip: qsTr("Close tab (Ctrl+W)")
                            onClicked: window.requestCloseTab(tabItem.index)
                        }
                    }
                    Rectangle {
                        visible: tabItem.current
                        anchors.bottom: parent.bottom
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: parent.width - Theme.spaceLg
                        height: Theme.focusRing
                        radius: 1
                        color: Theme.accent
                    }
                }
            }
        }
        NxDivider { Layout.fillWidth: true }

        // editors
        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: window.currentTab

            Repeater {
                id: tabRepeater
                model: tabs
                EditorView {
                    id: editorView
                    required property int index
                    prefs: prefs
                    onFileLoaded: {
                        if (EditorBackend.benchmark) { bench.begin(); return }
                        EditorBackend.addRecentFile(document.filePath)
                        prefs.lastFolder = EditorBackend.parentPath(document.filePath)
                        window.storeSession()
                    }
                    onFailed: (message) => {
                        if (EditorBackend.benchmark) { console.log("BENCH refused:", message); Qt.quit(); return }
                        errorDialog.message = message
                        errorDialog.open()
                        // A tab created only for a file that could not be opened is of no use.
                        if (document.filePath === "" && editor.length === 0 && !document.modified && tabs.count > 1) {
                            const back = window.tabBeforeNew
                            window.closeTab(index)
                            window.currentTab = Math.min(back, tabs.count - 1)
                        }
                    }
                }
            }
        }

        FindBar {
            id: findBar
            Layout.fillWidth: true
            view: window.currentView
            onReplacedAll: (count) => window.toaster.show(qsTr("Replaced: %1").arg(count), "success")
            onClosed: window.focusEditor()
        }

        // status bar
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.controlHeight
            color: Theme.bgSurface
            NxDivider { anchors.top: parent.top; width: parent.width }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.spaceSm
                anchors.rightMargin: Theme.spaceSm
                spacing: Theme.spaceXs

                component StatusButton: ToolButton {
                    font.pixelSize: Theme.fontSizeSmall
                    implicitHeight: Theme.controlHeight - Theme.spaceSm
                    topPadding: 0
                    bottomPadding: 0
                    ToolTip.visible: hovered && ToolTip.text !== ""
                    ToolTip.delay: 500
                }

                StatusButton {
                    objectName: "positionButton"
                    text: window.currentView
                        ? qsTr("Ln %1, Col %2").arg(window.currentView.cursorLine).arg(window.currentView.cursorColumn) : ""
                    ToolTip.text: qsTr("Go to line (Ctrl+G)")
                    onClicked: goToDialog.open()
                }
                Label {
                    readonly property int selected: window.currentView
                        ? window.currentView.editor.selectionEnd - window.currentView.editor.selectionStart : 0
                    visible: selected > 0
                    text: qsTr("%1 selected").arg(selected)
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSizeSmall
                }
                Label {
                    visible: window.currentDocument !== null
                    text: window.currentDocument ? (window.currentDocument.lineCount === 1 ? qsTr("1 line") : qsTr("%1 lines").arg(window.currentDocument.lineCount)) : ""
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSizeSmall
                    leftPadding: Theme.spaceSm
                }
                Item { Layout.fillWidth: true }
                StatusButton {
                    id: indentButton
                    text: prefs.useSpaces ? qsTr("Spaces: %1").arg(prefs.indentWidth) : qsTr("Tabs: %1").arg(prefs.indentWidth)
                    ToolTip.text: qsTr("Indentation")
                    onClicked: indentMenu.popup(indentButton, 0, -indentMenu.implicitHeight)
                }
                StatusButton {
                    text: window.currentDocument
                        ? window.currentDocument.encoding + (window.currentDocument.byteOrderMark ? qsTr(" with BOM") : "") : ""
                    ToolTip.text: qsTr("Text encoding")
                    onClicked: encodingDialog.open()
                }
                StatusButton {
                    id: eolButton
                    text: window.currentDocument
                        ? window.currentDocument.lineEnding + (window.currentDocument.mixedLineEndings ? qsTr(" (mixed)") : "") : ""
                    ToolTip.text: qsTr("Line endings")
                    onClicked: eolMenu.popup(eolButton, 0, -eolMenu.implicitHeight)
                }
                StatusButton {
                    text: !window.currentDocument ? ""
                        : window.currentDocument.syntax === "None" ? qsTr("Plain text")
                        : window.currentDocument.highlighting ? window.currentDocument.syntax
                        : qsTr("%1 (highlighting off)").arg(window.currentDocument.syntax)
                    ToolTip.text: qsTr("Syntax highlighting")
                    onClicked: syntaxDialog.open()
                }
            }
        }
    }

    // ---- menus ---------------------------------------------------------------------
    Menu {
        id: mainMenu
        MenuItem { text: qsTr("Save as…"); onTriggered: window.saveViewAs(window.currentView) }
        MenuItem { text: qsTr("Save all"); onTriggered: window.saveAll() }
        MenuItem {
            text: qsTr("Reload from disk")
            enabled: window.currentDocument !== null && window.currentDocument.filePath !== ""
            onTriggered: window.currentDocument.modified ? reloadDialog.open() : window.currentDocument.reload()
        }
        MenuSeparator { }
        MenuItem { text: qsTr("Go to line…"); onTriggered: goToDialog.open() }
        MenuItem { text: qsTr("Line numbers"); checkable: true; checked: prefs.lineNumbers; onTriggered: prefs.lineNumbers = checked }
        MenuItem { text: qsTr("Highlight current line"); checkable: true; checked: prefs.highlightLine; onTriggered: prefs.highlightLine = checked }
        MenuSeparator { }
        MenuItem { text: qsTr("Preferences…"); onTriggered: preferencesDialog.open() }
    }
    Menu {
        id: recentMenu
        Instantiator {
            model: EditorBackend.recentFiles
            delegate: MenuItem {
                required property string modelData
                text: modelData
                onTriggered: window.openFile(modelData)
            }
            onObjectAdded: (index, object) => recentMenu.insertItem(index, object)
            onObjectRemoved: (index, object) => recentMenu.removeItem(object)
        }
        MenuItem {
            text: EditorBackend.recentFiles.length > 0 ? qsTr("Clear this list") : qsTr("No recent files")
            enabled: EditorBackend.recentFiles.length > 0
            onTriggered: EditorBackend.clearRecentFiles()
        }
    }
    Menu {
        id: indentMenu
        MenuItem { text: qsTr("Indent with spaces"); checkable: true; checked: prefs.useSpaces; onTriggered: prefs.useSpaces = true }
        MenuItem { text: qsTr("Indent with tabs"); checkable: true; checked: !prefs.useSpaces; onTriggered: prefs.useSpaces = false }
        MenuSeparator { }
        Repeater {
            model: [2, 4, 8]
            MenuItem {
                required property int modelData
                text: qsTr("Width %1").arg(modelData)
                checkable: true
                checked: prefs.indentWidth === modelData
                onTriggered: prefs.indentWidth = modelData
            }
        }
    }
    Menu {
        id: eolMenu
        Repeater {
            model: [
                { key: "LF", label: qsTr("LF (Unix, Linux)") },
                { key: "CRLF", label: qsTr("CRLF (Windows)") },
                { key: "CR", label: qsTr("CR (classic Mac)") },
            ]
            MenuItem {
                required property var modelData
                text: modelData.label
                checkable: true
                checked: window.currentDocument !== null && window.currentDocument.lineEnding === modelData.key
                    && !window.currentDocument.mixedLineEndings
                onTriggered: {
                    window.currentDocument.lineEnding = modelData.key
                    window.toaster.show(qsTr("Line endings will be written as %1 when you save").arg(modelData.key), "info")
                }
            }
        }
    }

    // ---- dialogs -------------------------------------------------------------------
    PathPicker {
        id: openPicker
        fs: EditorBackend
        mode: "open"
        multiple: true
        function show() {
            folder = window.currentDocument && window.currentDocument.filePath !== ""
                ? EditorBackend.parentPath(window.currentDocument.filePath) : prefs.lastFolder
            open()
        }
        onChosen: (paths) => window.openFiles(paths)
        onClosed: window.focusEditor()
    }
    PathPicker {
        id: savePicker
        property var view: null
        property var then: null
        fs: EditorBackend
        mode: "save"
        onChosen: (paths) => {
            const target = view, next = then
            prefs.lastFolder = EditorBackend.parentPath(paths[0])
            target.afterSave = () => { window.afterSaved(target); if (next) next() }
            target.document.saveAs(paths[0])
        }
        onClosed: window.focusEditor()
    }

    Dialog {
        id: unsavedDialog
        property var view: null
        property bool closingWindow: false
        title: qsTr("Save changes?")
        width: Math.min(480, window.width - Theme.spaceXxl)
        closePolicy: Popup.CloseOnEscape
        footer: null
        onRejected: { for (const v of window.views) v.discardOnClose = false }
        contentItem: ColumnLayout {
            spacing: Theme.spaceLg
            Label {
                Layout.fillWidth: true
                text: unsavedDialog.view
                    ? qsTr("“%1” has changes that have not been saved. They are lost if you do not save them.").arg(unsavedDialog.view.document.fileName)
                    : ""
                color: Theme.textSecondary
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceSm
                NxButton {
                    text: qsTr("Discard changes")
                    kind: "danger"
                    onClicked: {
                        const view = unsavedDialog.view, closing = unsavedDialog.closingWindow
                        unsavedDialog.accept()
                        if (closing) { view.discardOnClose = true; window.close() }
                        else window.closeTab(window.views.indexOf(view))
                    }
                }
                Item { Layout.fillWidth: true }
                NxButton { text: qsTr("Cancel"); onClicked: unsavedDialog.reject() }
                NxButton {
                    text: qsTr("Save")
                    kind: "primary"
                    onClicked: {
                        const view = unsavedDialog.view, closing = unsavedDialog.closingWindow
                        unsavedDialog.accept()
                        window.saveView(view, () => {
                            if (closing) window.close()
                            else window.closeTab(window.views.indexOf(view))
                        })
                    }
                }
            }
        }
    }

    NxConfirmDialog {
        id: overwriteDialog
        property var view: null
        property var then: null
        title: qsTr("Overwrite the newer file?")
        message: view ? qsTr("“%1” was changed by another program after you opened it. Saving now replaces those changes with your version.").arg(view.document.fileName) : ""
        confirmText: qsTr("Overwrite")
        destructive: true
        onConfirmed: {
            const target = view, next = then
            target.document.acknowledgeDiskChange()
            target.afterSave = () => { window.afterSaved(target); if (next) next() }
            target.document.save()
        }
    }
    NxConfirmDialog {
        id: reloadDialog
        title: qsTr("Discard changes and reload?")
        message: window.currentDocument ? qsTr("Your unsaved changes to “%1” will be lost.").arg(window.currentDocument.fileName) : ""
        confirmText: qsTr("Reload")
        destructive: true
        onConfirmed: window.currentDocument.reload()
    }

    Dialog {
        id: errorDialog
        property string message: ""
        title: qsTr("That did not work")
        width: Math.min(520, window.width - Theme.spaceXxl)
        footer: null
        contentItem: ColumnLayout {
            spacing: Theme.spaceLg
            Label {
                objectName: "errorMessage"
                Layout.fillWidth: true
                text: errorDialog.message
                color: Theme.textSecondary
                wrapMode: Text.WordWrap
            }
            NxButton {
                Layout.alignment: Qt.AlignRight
                text: qsTr("Close")
                kind: "primary"
                onClicked: errorDialog.close()
            }
        }
        onClosed: window.focusEditor()
    }

    Dialog {
        id: goToDialog
        title: qsTr("Go to line")
        width: 360
        footer: null
        function go() {
            const parts = lineField.text.trim().split(/[:,\s]+/)
            const line = parseInt(parts[0])
            if (isNaN(line) || !window.currentView) return
            const column = parts.length > 1 ? parseInt(parts[1]) : 1
            accept()
            window.currentView.goToLine(line, isNaN(column) ? 1 : column)
        }
        onOpened: {
            lineField.text = window.currentView ? String(window.currentView.cursorLine) : ""
            lineField.selectAll()
            lineField.forceActiveFocus()
        }
        onClosed: window.focusEditor()
        contentItem: ColumnLayout {
            spacing: Theme.spaceMd
            Label {
                Layout.fillWidth: true
                text: window.currentDocument
                    ? qsTr("Line number from 1 to %1, optionally followed by “:column”.").arg(window.currentDocument.lineCount) : ""
                color: Theme.textSecondary
                wrapMode: Text.WordWrap
            }
            TextField {
                id: lineField
                Layout.fillWidth: true
                font.family: Theme.fontMono
                Accessible.name: qsTr("Line number")
                validator: RegularExpressionValidator { regularExpression: /\s*\d+\s*([:,\s]\s*\d+)?\s*/ }
                onAccepted: goToDialog.go()
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceSm
                Item { Layout.fillWidth: true }
                NxButton { text: qsTr("Cancel"); onClicked: goToDialog.reject() }
                NxButton { text: qsTr("Go"); kind: "primary"; enabled: lineField.acceptableInput; onClicked: goToDialog.go() }
            }
        }
    }

    Dialog {
        id: encodingDialog
        title: qsTr("Text encoding")
        width: Math.min(520, window.width - Theme.spaceXxl)
        footer: null
        onOpened: {
            encodingBox.currentIndex = Math.max(0, EditorBackend.encodings.indexOf(window.currentDocument.encoding))
            bomBox.checked = window.currentDocument.byteOrderMark
        }
        onClosed: window.focusEditor()
        contentItem: ColumnLayout {
            spacing: Theme.spaceMd
            Label {
                Layout.fillWidth: true
                text: window.currentDocument
                    ? qsTr("“%1” is currently read and written as %2.").arg(window.currentDocument.fileName).arg(window.currentDocument.encoding) : ""
                color: Theme.textSecondary
                wrapMode: Text.WordWrap
            }
            ComboBox {
                id: encodingBox
                Layout.fillWidth: true
                model: EditorBackend.encodings
                Accessible.name: qsTr("Encoding")
            }
            CheckBox {
                id: bomBox
                text: qsTr("Write a byte order mark (BOM)")
                enabled: encodingBox.currentText.startsWith("UTF-")
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("“Reopen” reads the file again from disk assuming the chosen encoding — use it when the text looks garbled. "
                           + "“Convert” keeps the text and writes it in the chosen encoding the next time you save.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WordWrap
            }
            Label {
                Layout.fillWidth: true
                visible: window.currentDocument !== null && window.currentDocument.modified
                text: qsTr("Reopening discards your unsaved changes.")
                color: Theme.warning
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceSm
                NxButton {
                    text: qsTr("Reopen")
                    enabled: window.currentDocument !== null && window.currentDocument.filePath !== ""
                    onClicked: {
                        const encoding = encodingBox.currentText
                        encodingDialog.accept()
                        window.currentView.reopenWithEncoding(encoding)
                    }
                }
                Item { Layout.fillWidth: true }
                NxButton { text: qsTr("Cancel"); onClicked: encodingDialog.reject() }
                NxButton {
                    text: qsTr("Convert")
                    kind: "primary"
                    onClicked: {
                        window.currentDocument.encoding = encodingBox.currentText
                        window.currentDocument.byteOrderMark = bomBox.checked && bomBox.enabled
                        encodingDialog.accept()
                    }
                }
            }
        }
    }

    Dialog {
        id: syntaxDialog
        title: qsTr("Syntax highlighting")
        width: Math.min(460, window.width - Theme.spaceXxl)
        height: Math.min(560, window.height - Theme.spaceXxl)
        footer: null
        readonly property var filtered: {
            const needle = syntaxFilter.text.trim().toLowerCase()
            const all = EditorBackend.syntaxes
            if (needle === "") return all
            return all.filter((s) => s.name.toLowerCase().indexOf(needle) >= 0)
        }
        function choose(name) {
            window.currentDocument.syntax = name
            accept()
        }
        onOpened: { syntaxFilter.text = ""; syntaxFilter.forceActiveFocus() }
        onClosed: window.focusEditor()
        contentItem: ColumnLayout {
            spacing: Theme.spaceMd
            NxSearchField {
                id: syntaxFilter
                Layout.fillWidth: true
                placeholderText: qsTr("Filter languages")
                Keys.onDownPressed: syntaxList.forceActiveFocus()
                onAccepted: if (syntaxDialog.filtered.length > 0) syntaxDialog.choose(syntaxDialog.filtered[0].name)
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
                    id: syntaxList
                    anchors.fill: parent
                    anchors.margins: Theme.spaceXs
                    model: syntaxDialog.filtered
                    boundsBehavior: Flickable.StopAtBounds
                    keyNavigationEnabled: true
                    activeFocusOnTab: true
                    highlightMoveDuration: 0
                    ScrollBar.vertical: ScrollBar { }
                    Keys.onReturnPressed: if (currentIndex >= 0) syntaxDialog.choose(syntaxDialog.filtered[currentIndex].name)
                    delegate: Rectangle {
                        id: syntaxItem
                        required property var modelData
                        required property int index
                        readonly property bool active: window.currentDocument !== null && window.currentDocument.syntax === modelData.name
                        width: syntaxList.width
                        height: Theme.controlHeight
                        radius: Theme.radiusXs
                        color: ListView.isCurrentItem && syntaxList.activeFocus ? Theme.accentFill
                            : syntaxHover.hovered ? Theme.hoverOverlay : "transparent"
                        Accessible.role: Accessible.ListItem
                        Accessible.name: modelData.name
                        HoverHandler { id: syntaxHover }
                        TapHandler { onTapped: syntaxDialog.choose(syntaxItem.modelData.name) }
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.spaceMd
                            anchors.rightMargin: Theme.spaceMd
                            Label {
                                Layout.fillWidth: true
                                text: syntaxItem.modelData.name === "None" ? qsTr("Plain text (no highlighting)") : syntaxItem.modelData.name
                                color: syntaxItem.active ? Theme.accent : Theme.textPrimary
                                elide: Text.ElideRight
                            }
                            Label {
                                text: syntaxItem.modelData.section
                                color: Theme.textMuted
                                font.pixelSize: Theme.fontSizeSmall
                            }
                        }
                    }
                }
            }
            NxButton {
                Layout.alignment: Qt.AlignRight
                text: qsTr("Cancel")
                onClicked: syntaxDialog.reject()
            }
        }
    }

    Dialog {
        id: preferencesDialog
        title: qsTr("Preferences")
        width: Math.min(520, window.width - Theme.spaceXxl)
        footer: null
        onClosed: window.focusEditor()
        contentItem: ColumnLayout {
            spacing: Theme.spaceMd
            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: Theme.spaceLg
                rowSpacing: Theme.spaceSm

                Label { text: qsTr("Font") }
                ComboBox {
                    Layout.fillWidth: true
                    model: EditorBackend.monospaceFamilies
                    currentIndex: Math.max(0, EditorBackend.monospaceFamilies.indexOf(prefs.fontFamily))
                    Accessible.name: qsTr("Font")
                    onActivated: prefs.fontFamily = currentText
                }
                Label { text: qsTr("Font size (points)") }
                SpinBox {
                    from: 6; to: 48
                    value: prefs.fontSize
                    Accessible.name: qsTr("Font size")
                    onValueModified: prefs.fontSize = value
                }
                Label { text: qsTr("Indent with") }
                NxSegmented {
                    model: [qsTr("Spaces"), qsTr("Tabs")]
                    currentIndex: prefs.useSpaces ? 0 : 1
                    onActivated: (index) => prefs.useSpaces = index === 0
                }
                Label { text: qsTr("Indentation / tab width") }
                SpinBox {
                    from: 1; to: 16
                    value: prefs.indentWidth
                    Accessible.name: qsTr("Indentation width")
                    onValueModified: prefs.indentWidth = value
                }
                Label { text: qsTr("Keep indentation on new lines") }
                Switch { checked: prefs.autoIndent; onToggled: prefs.autoIndent = checked; Accessible.name: qsTr("Auto-indent") }
                Label { text: qsTr("Wrap long lines") }
                Switch { checked: prefs.wordWrap; onToggled: prefs.wordWrap = checked; Accessible.name: qsTr("Word wrap") }
                Label { text: qsTr("Line numbers") }
                Switch { checked: prefs.lineNumbers; onToggled: prefs.lineNumbers = checked; Accessible.name: qsTr("Line numbers") }
                Label { text: qsTr("Highlight the current line") }
                Switch { checked: prefs.highlightLine; onToggled: prefs.highlightLine = checked; Accessible.name: qsTr("Highlight current line") }
                Label { text: qsTr("Reopen the last files at start") }
                Switch { checked: prefs.restoreSession; onToggled: prefs.restoreSession = checked; Accessible.name: qsTr("Restore session") }
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Unsaved text is never restored: only the list of open files and the cursor positions are remembered.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WordWrap
            }
            NxButton {
                Layout.alignment: Qt.AlignRight
                text: qsTr("Close")
                kind: "primary"
                onClicked: preferencesDialog.close()
            }
        }
    }
}
