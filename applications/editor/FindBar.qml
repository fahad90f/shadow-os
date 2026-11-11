// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui

// Find and replace for the current tab. All matching is done by the
// document (EditorDocument.setSearch/findNext/replaceOne/replaceAll).
Rectangle {
    id: bar

    property var view: null                 // current EditorView
    property bool replaceMode: false
    property int currentIndex: 0            // 1-based index of the selected match, 0: none
    readonly property var document: view ? view.document : null
    readonly property int matchCount: document ? document.matchCount : 0

    signal closed()
    signal replacedAll(int count)

    function show(withReplace) {
        replaceMode = withReplace
        visible = true
        const selected = view ? view.editor.selectedText : ""
        if (selected !== "" && selected.indexOf(" ") < 0 && selected.indexOf("\n") < 0)
            searchField.text = selected
        searchField.forceActiveFocus()
        searchField.selectAll()
        apply(true)
    }
    function hide() {
        visible = false
        if (document) document.setSearch("", false, false, false)
        closed()
    }
    // Send the pattern to the document; optionally jump to the first match
    // at or after the selection.
    function apply(jump) {
        if (!document || !visible)
            return
        document.setSearch(searchField.text, regexButton.checked, caseButton.checked, wordButton.checked)
        currentIndex = 0
        if (jump && searchField.text !== "")
            step(false, view.editor.selectionStart)
    }
    function step(backward, from) {
        if (!document || searchField.text === "")
            return
        const editor = view.editor
        const origin = from !== undefined ? from : backward ? editor.selectionStart : editor.selectionEnd
        const result = document.findNext(origin, backward)
        if (result.found) {
            view.selectRange(result.start, result.end)
            currentIndex = result.index
        } else {
            currentIndex = 0
        }
    }
    function replaceCurrent() {
        if (!document || searchField.text === "")
            return
        const editor = view.editor
        const result = document.replaceOne(editor.selectionStart, editor.selectionEnd, replaceField.text)
        if (result.found) {
            view.selectRange(result.start, result.end)
            currentIndex = result.index
        } else {
            currentIndex = 0
        }
    }
    function replaceEverything() {
        if (!document || searchField.text === "")
            return
        const count = document.replaceAll(replaceField.text)
        currentIndex = 0
        replacedAll(count)
    }

    onViewChanged: apply(false)

    visible: false
    implicitHeight: column.implicitHeight + Theme.spaceSm * 2
    color: Theme.bgSurface

    NxDivider { anchors.top: parent.top; width: parent.width }

    ColumnLayout {
        id: column
        anchors.fill: parent
        anchors.margins: Theme.spaceSm
        anchors.leftMargin: Theme.spaceMd
        anchors.rightMargin: Theme.spaceMd
        spacing: Theme.spaceSm

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceSm

            TextField {
                id: searchField
                Layout.fillWidth: true
                Layout.maximumWidth: 420
                placeholderText: qsTr("Find")
                Accessible.name: qsTr("Find")
                font.family: Theme.fontMono
                onTextEdited: bar.apply(true)
                Keys.onReturnPressed: (event) => bar.step(event.modifiers & Qt.ShiftModifier)
                Keys.onEnterPressed: (event) => bar.step(event.modifiers & Qt.ShiftModifier)
                Keys.onEscapePressed: bar.hide()
            }
            ToolButton {
                id: caseButton
                text: "Aa"
                checkable: true
                Accessible.name: qsTr("Match case")
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Match case")
                onToggled: bar.apply(true)
            }
            ToolButton {
                id: wordButton
                text: "W"
                checkable: true
                Accessible.name: qsTr("Whole words")
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Whole words")
                onToggled: bar.apply(true)
            }
            ToolButton {
                id: regexButton
                text: ".*"
                checkable: true
                Accessible.name: qsTr("Regular expression")
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Regular expression (PCRE; ^ and $ match at line breaks)")
                onToggled: bar.apply(true)
            }
            Label {
                objectName: "matchLabel"
                Layout.minimumWidth: 110
                text: bar.document && bar.document.searchError !== "" ? bar.document.searchError
                    : searchField.text === "" ? ""
                    : bar.matchCount === 0 ? qsTr("No matches")
                    : bar.currentIndex > 0 ? qsTr("%1 of %2").arg(bar.currentIndex).arg(bar.matchCount)
                    : qsTr("%n matches", "", bar.matchCount)
                color: (bar.document && bar.document.searchError !== "") || (searchField.text !== "" && bar.matchCount === 0)
                    ? Theme.danger : Theme.textSecondary
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            NxIconButton {
                iconName: "chevron-up"
                tooltip: qsTr("Previous match (Shift+F3)")
                enabled: bar.matchCount > 0
                onClicked: bar.step(true)
            }
            NxIconButton {
                iconName: "chevron-down"
                tooltip: qsTr("Next match (F3)")
                enabled: bar.matchCount > 0
                onClicked: bar.step(false)
            }
            NxIconButton {
                iconName: "edit"
                tooltip: qsTr("Replace")
                checkable: true
                checked: bar.replaceMode
                onToggled: bar.replaceMode = checked
            }
            NxIconButton {
                iconName: "close"
                tooltip: qsTr("Close (Esc)")
                onClicked: bar.hide()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: bar.replaceMode
            spacing: Theme.spaceSm

            TextField {
                id: replaceField
                Layout.fillWidth: true
                Layout.maximumWidth: 420
                placeholderText: regexButton.checked ? qsTr("Replace with (\\1 or $1 inserts a group)") : qsTr("Replace with")
                Accessible.name: qsTr("Replace with")
                font.family: Theme.fontMono
                Keys.onReturnPressed: bar.replaceCurrent()
                Keys.onEscapePressed: bar.hide()
            }
            NxButton {
                text: qsTr("Replace")
                enabled: bar.matchCount > 0
                onClicked: bar.replaceCurrent()
            }
            NxButton {
                text: qsTr("Replace all")
                enabled: bar.matchCount > 0
                onClicked: bar.replaceEverything()
            }
            Item { Layout.fillWidth: true }
        }
    }
}
