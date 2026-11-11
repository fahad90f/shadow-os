// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.editor

// One tab: the text area with its line-number gutter and the messages that
// belong to this file (changed on disk, encoding notices).
FocusScope {
    id: view

    required property var prefs
    readonly property alias document: doc
    readonly property alias editor: edit
    property int cursorLine: 1
    property int cursorColumn: 1
    property int pendingPosition: -1
    property int pendingLine: 0
    property var afterSave: null            // called once after the next successful save
    property bool discardOnClose: false

    signal fileLoaded()
    signal failed(string message)

    function open(path, line, position) {
        pendingLine = line || 0
        pendingPosition = position === undefined ? -1 : position
        doc.load(path)
    }
    function reopenWithEncoding(encoding) {
        pendingPosition = edit.cursorPosition
        pendingLine = 0
        doc.load(doc.filePath, encoding)
    }
    function goToLine(line, column) {
        edit.cursorPosition = doc.positionForLine(line, column || 1)
        edit.centerCursor()
        edit.forceActiveFocus()
    }
    function selectRange(start, end) {
        edit.select(start, end)
        edit.centerCursor()
    }
    function updateCursorInfo() {
        const info = doc.lineColumn(edit.cursorPosition)
        cursorLine = info.line
        cursorColumn = info.column
    }

    EditorDocument {
        id: doc
        editor: edit
        dark: Appearance.dark
        onLoaded: {
            if (view.pendingLine > 0)
                edit.cursorPosition = doc.positionForLine(view.pendingLine, 1)
            else
                edit.cursorPosition = Math.max(0, Math.min(view.pendingPosition, edit.length))
            view.pendingLine = 0
            view.pendingPosition = -1
            edit.centerCursor()
            view.updateCursorInfo()
            view.fileLoaded()
        }
        onLoadFailed: (message) => view.failed(message)
        onSaveFinished: (ok, message) => {
            const then = view.afterSave
            view.afterSave = null
            if (!ok) view.failed(message)
            else if (then) then()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---- messages about this file ---------------------------------------
        ColumnLayout {
            Layout.fillWidth: true
            Layout.margins: Theme.spaceSm
            spacing: Theme.spaceSm
            visible: doc.changedOnDisk || doc.deletedOnDisk || doc.notice !== ""

            FileNotice {
                visible: doc.changedOnDisk
                kind: "warning"
                title: qsTr("This file was changed by another program")
                message: doc.modified
                    ? qsTr("Reloading discards your unsaved changes; saving overwrites the other program's changes.")
                    : qsTr("Reload to see the new contents.")
                actionText: qsTr("Reload")
                dismissible: true
                onActionTriggered: { view.pendingPosition = edit.cursorPosition; doc.reload() }
                onDismissed: doc.acknowledgeDiskChange()
            }
            FileNotice {
                visible: doc.deletedOnDisk
                kind: "warning"
                title: qsTr("This file was deleted or moved by another program")
                message: qsTr("The text below now exists only in this window. Save to write it back.")
                dismissible: true
                onDismissed: doc.acknowledgeDiskChange()
            }
            FileNotice {
                visible: doc.notice !== ""
                kind: "info"
                message: doc.notice
                dismissible: true
                onDismissed: doc.dismissNotice()
            }
        }

        // ---- text (with its line-number gutter) --------------------------------
        CodeEditor {
            id: edit
            Layout.fillWidth: true
            Layout.fillHeight: true
            focus: true
            fontFamily: view.prefs.fontFamily
            fontPointSize: view.prefs.fontSize
            wordWrap: view.prefs.wordWrap
            tabWidth: view.prefs.indentWidth
            lineNumbers: view.prefs.lineNumbers
            highlightCurrentLine: view.prefs.highlightLine
            readOnly: doc.loading
            textColor: Theme.textPrimary
            backgroundColor: Theme.bgBase
            gutterColor: Theme.bgSurface
            gutterTextColor: Theme.textMuted
            gutterCurrentColor: Theme.textPrimary
            currentLineColor: Theme.mix(Theme.bgBase, Theme.textPrimary, 0.06)
            selectionColor: Theme.mix(Theme.bgBase, Theme.accent, 0.32)
            matchColor: Theme.mix(Theme.bgBase, Theme.warning, 0.30)
            dividerColor: Theme.borderSubtle
            Accessible.role: Accessible.EditableText
            Accessible.name: qsTr("Text of %1").arg(doc.fileName)

            onCursorChanged: view.updateCursorInfo()
            onContextMenuRequested: contextMenu.popup()

            Keys.onPressed: (event) => {
                const plain = !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))
                if (!plain || readOnly)
                    return
                const p = view.prefs
                if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) {
                    const unindent = event.key === Qt.Key_Backtab
                    if (unindent || selectedText.indexOf("\n") >= 0) {
                        const hadSelection = selectionStart !== selectionEnd
                        const range = doc.indentLines(selectionStart, selectionEnd, unindent, p.useSpaces, p.indentWidth)
                        if (hadSelection) select(range.start, range.end)
                    } else {
                        const start = selectionStart
                        if (selectionStart !== selectionEnd) remove(selectionStart, selectionEnd)
                        insert(start, doc.tabText(start, p.useSpaces, p.indentWidth))
                    }
                    event.accepted = true
                } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && !(event.modifiers & Qt.ShiftModifier)) {
                    const start = selectionStart
                    const text = doc.newlineText(start, p.autoIndent, p.useSpaces, p.indentWidth)
                    if (selectionStart !== selectionEnd) remove(selectionStart, selectionEnd)
                    insert(start, text)
                    event.accepted = true
                } else if (event.key === Qt.Key_Backspace && selectionStart === selectionEnd) {
                    const count = doc.backspaceIndent(cursorPosition, p.useSpaces, p.indentWidth)
                    if (count > 0) {
                        remove(cursorPosition - count, cursorPosition)
                        event.accepted = true
                    } else {
                        event.accepted = false
                    }
                } else {
                    event.accepted = false
                }
            }

            ScrollBar {
                id: verticalBar
                orientation: Qt.Vertical
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                size: edit.verticalSize
                policy: edit.verticalSize < 1 ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
                onPositionChanged: if (pressed) edit.scrollVertically(position)
                Binding on position { value: edit.verticalPosition; when: !verticalBar.pressed }
            }
            ScrollBar {
                id: horizontalBar
                orientation: Qt.Horizontal
                anchors.left: parent.left
                anchors.right: verticalBar.left
                anchors.bottom: parent.bottom
                size: edit.horizontalSize
                policy: edit.horizontalSize < 1 ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
                onPositionChanged: if (pressed) edit.scrollHorizontally(position)
                Binding on position { value: edit.horizontalPosition; when: !horizontalBar.pressed }
            }
        }
    }

    Menu {
        id: contextMenu
        MenuItem { text: qsTr("Undo"); enabled: edit.canUndo; onTriggered: edit.undo() }
        MenuItem { text: qsTr("Redo"); enabled: edit.canRedo; onTriggered: edit.redo() }
        MenuSeparator { }
        MenuItem { text: qsTr("Cut"); enabled: edit.selectedText !== ""; onTriggered: edit.cut() }
        MenuItem { text: qsTr("Copy"); enabled: edit.selectedText !== ""; onTriggered: edit.copy() }
        MenuItem { text: qsTr("Paste"); enabled: edit.canPaste; onTriggered: edit.paste() }
        MenuSeparator { }
        MenuItem { text: qsTr("Select all"); onTriggered: edit.selectAll() }
    }

    Rectangle {
        anchors.fill: parent
        visible: doc.loading
        color: Theme.alpha(Theme.bgBase, 0.6)
        ColumnLayout {
            anchors.centerIn: parent
            spacing: Theme.spaceMd
            BusyIndicator { Layout.alignment: Qt.AlignHCenter; running: doc.loading }
            Label { text: qsTr("Opening…"); color: Theme.textSecondary }
        }
    }
}
