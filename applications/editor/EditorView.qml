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
    property var lines: []
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
        centerCursor()
        edit.forceActiveFocus()
    }
    function centerCursor() {
        const r = edit.cursorRectangle
        flick.contentY = Math.max(0, Math.min(Math.max(0, flick.contentHeight - flick.height),
                                              r.y - (flick.height - r.height) / 2))
    }
    function selectRange(start, end) {
        edit.select(start, end)
        const r = edit.cursorRectangle
        if (r.y < flick.contentY + r.height || r.y + r.height * 2 > flick.contentY + flick.height)
            centerCursor()
    }
    function ensureVisible(r) {
        const pad = Theme.spaceLg
        if (r.x - pad < flick.contentX)
            flick.contentX = Math.max(0, r.x - pad)
        else if (r.x + r.width + pad > flick.contentX + flick.width)
            flick.contentX = r.x + r.width + pad - flick.width
        if (r.y < flick.contentY)
            flick.contentY = r.y
        else if (r.y + r.height > flick.contentY + flick.height)
            flick.contentY = r.y + r.height - flick.height
    }
    function updateLines() {
        lines = prefs.lineNumbers ? doc.visibleLines(flick.contentY - edit.topPadding, flick.contentY + flick.height) : []
    }
    function updateCursorInfo() {
        const info = doc.lineColumn(edit.cursorPosition)
        cursorLine = info.line
        cursorColumn = info.column
    }
    function scheduleLines() { Qt.callLater(view.updateLines) }

    EditorDocument {
        id: doc
        textDocument: edit.textDocument
        dark: Appearance.dark
        onLoaded: {
            if (view.pendingLine > 0) {
                edit.cursorPosition = doc.positionForLine(view.pendingLine, 1)
            } else {
                edit.cursorPosition = Math.max(0, Math.min(view.pendingPosition, edit.length))
            }
            view.pendingLine = 0
            view.pendingPosition = -1
            Qt.callLater(view.centerCursor)
            view.updateCursorInfo()
            view.scheduleLines()
            view.fileLoaded()
        }
        onLoadFailed: (message) => view.failed(message)
        onLayoutRevisionChanged: view.scheduleLines()
        onSaveFinished: (ok, message) => {
            const then = view.afterSave
            view.afterSave = null
            if (!ok) view.failed(message)
            else if (then) then()
        }
    }

    FontMetrics { id: metrics; font: edit.font }

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

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // ---- line numbers --------------------------------------------------
            Rectangle {
                id: gutter
                Layout.fillHeight: true
                Layout.preferredWidth: view.prefs.lineNumbers
                    ? metrics.advanceWidth("0") * Math.max(3, String(doc.lineCount).length) + Theme.spaceLg + Theme.spaceSm : 0
                visible: view.prefs.lineNumbers
                color: Theme.bgSurface
                clip: true

                Repeater {
                    model: view.lines.length
                    Text {
                        required property int index
                        readonly property var entry: view.lines[index] ?? null
                        visible: entry !== null
                        x: 0
                        y: entry ? entry.y + edit.topPadding - flick.contentY : 0
                        width: gutter.width - Theme.spaceSm
                        height: entry ? entry.lineHeight : 0
                        horizontalAlignment: Text.AlignRight
                        verticalAlignment: Text.AlignVCenter
                        text: entry ? entry.line : ""
                        color: entry && entry.line === view.cursorLine ? Theme.textPrimary : Theme.textMuted
                        font.family: edit.font.family
                        font.pointSize: edit.font.pointSize
                    }
                }
                NxDivider { vertical: true; anchors.right: parent.right; height: parent.height }
            }

            // ---- text ----------------------------------------------------------
            Flickable {
                id: flick
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentWidth: edit.width
                contentHeight: edit.height
                boundsBehavior: Flickable.StopAtBounds
                flickableDirection: Flickable.AutoFlickIfNeeded
                pixelAligned: true
                ScrollBar.vertical: ScrollBar { }
                ScrollBar.horizontal: ScrollBar { }
                onContentYChanged: view.updateLines()
                onHeightChanged: view.scheduleLines()
                onWidthChanged: view.scheduleLines()

                Rectangle {
                    visible: view.prefs.highlightLine
                    y: edit.cursorRectangle.y
                    width: Math.max(flick.width, edit.width)
                    height: edit.cursorRectangle.height
                    color: Theme.hoverOverlay
                }

                TextEdit {
                    id: edit
                    width: view.prefs.wordWrap ? flick.width : Math.max(flick.width, implicitWidth)
                    height: Math.max(flick.height, implicitHeight)
                    focus: true
                    textFormat: TextEdit.PlainText
                    wrapMode: view.prefs.wordWrap ? TextEdit.Wrap : TextEdit.NoWrap
                    font.family: view.prefs.fontFamily
                    font.pointSize: view.prefs.fontSize
                    tabStopDistance: metrics.advanceWidth(" ") * view.prefs.indentWidth
                    color: Theme.textPrimary
                    selectionColor: Theme.accentFillStrong
                    selectedTextColor: Theme.textPrimary
                    selectByMouse: true
                    persistentSelection: true
                    leftPadding: Theme.spaceSm
                    rightPadding: Theme.spaceLg
                    topPadding: Theme.spaceXs
                    bottomPadding: Theme.spaceXl
                    readOnly: doc.loading
                    Accessible.name: qsTr("Text of %1").arg(doc.fileName)

                    onCursorRectangleChanged: view.ensureVisible(cursorRectangle)
                    onCursorPositionChanged: view.updateCursorInfo()
                    onFontChanged: view.scheduleLines()
                    onWidthChanged: view.scheduleLines()

                    Keys.onPressed: (event) => {
                        const plain = !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))
                        if (!plain || readOnly)
                            return
                        const p = view.prefs
                        if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) {
                            const unindent = event.key === Qt.Key_Backtab
                            if (unindent || selectedText.indexOf(" ") >= 0 || selectedText.indexOf("\n") >= 0) {
                                const range = doc.indentLines(selectionStart, selectionEnd, unindent, p.useSpaces, p.indentWidth)
                                if (selectionStart !== selectionEnd) select(range.start, range.end)
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
                            }
                        }
                    }

                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: contextMenu.popup()
                    }
                }
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
