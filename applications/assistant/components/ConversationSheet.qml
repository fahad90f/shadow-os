// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.assistant

// Conversations, dropped from the title like a table of contents. Rename and
// delete happen in place (no modal dialogs): F2 / the pencil turns a row into
// a field, Delete / the bin asks for confirmation inside the row.
//
// The sheet is modal: the scrim takes every click and hover meant for what
// lies under it, and Tab / Shift+Tab cycle inside the panel. With a filter,
// the keyboard only ever acts on the rows that are shown.
Item {
    id: sheet
    required property StageModel stage
    property bool open: false
    property real anchorX: 24
    property real anchorY: 60
    property string renamingId: ""
    property string confirmingId: ""
    // opened just to rename (F2 on the stage): closes again when the rename ends
    property bool closeAfterRename: false
    signal closeRequested()

    visible: open || panel.opacity > 0.01

    function close() {
        renamingId = ""
        confirmingId = ""
        closeAfterRename = false
        closeRequested()
    }
    // F2 on the stage: the list opens on the open conversation, as a name field
    function renameOpen(id: string) {
        closeAfterRename = true
        renamingId = id
    }
    function focusFilter() {
        filter.forceActiveFocus()
    }
    function choose(id: string) {
        if (stage.selectConversation(id))
            close()
    }
    onOpenChanged: {
        if (open) {
            filter.text = ""
            list.currentIndex = Math.max(0, stage.conversations.indexOfId(stage.activeId))
            fixCurrent()
            list.forceActiveFocus()
        } else {
            renamingId = ""
            confirmingId = ""
            closeAfterRename = false
        }
    }

    // ---- the filter: which rows are shown ---------------------------------------------------
    readonly property string filterText: filter.text
    function matchesTitle(title: string): bool {
        return filterText === "" || title.toLowerCase().indexOf(filterText.toLowerCase()) >= 0
    }
    function idAt(i: int): string {
        if (i < 0 || i >= stage.conversations.count)
            return ""
        return stage.conversations.data(stage.conversations.index(i, 0), 257 /* id */)
    }
    function titleAt(i: int): string {
        if (i < 0 || i >= stage.conversations.count)
            return ""
        return stage.conversations.data(stage.conversations.index(i, 0), 258 /* title */) || ""
    }
    // The rows the filter shows, in order.
    readonly property var shownRows: {
        if (stage.convRevision < 0)  // a dependency (a bare read could be compiled away)
            return []
        const out = []
        for (let i = 0; i < stage.conversations.count; ++i)
            if (matchesTitle(titleAt(i)))
                out.push(i)
        return out
    }
    function shown(i: int): bool {
        return shownRows.indexOf(i) >= 0
    }
    // The keyboard row is always a shown one (the first match after the filter changes).
    function fixCurrent() {
        if (shown(list.currentIndex))
            return
        list.currentIndex = shownRows.length > 0 ? shownRows[0] : -1
    }
    onFilterTextChanged: {
        confirmingId = ""
        list.currentIndex = shownRows.length > 0 ? shownRows[0] : -1
        if (list.currentIndex >= 0)
            list.positionViewAtIndex(list.currentIndex, ListView.Contain)
    }
    onShownRowsChanged: Qt.callLater(fixCurrent)
    // ↑ / ↓ (Home / End) walk through the shown rows only
    function move(step: int) {
        const at = shownRows.indexOf(list.currentIndex)
        let next = -1
        if (step === -2)
            next = shownRows.length > 0 ? shownRows[0] : -1
        else if (step === 2)
            next = shownRows.length > 0 ? shownRows[shownRows.length - 1] : -1
        else if (at < 0)
            next = shownRows.length > 0 ? shownRows[0] : -1
        else
            next = shownRows[Math.max(0, Math.min(shownRows.length - 1, at + step))]
        if (next >= 0) {
            list.currentIndex = next
            list.positionViewAtIndex(next, ListView.Contain)
        }
    }

    // ---- modal: Tab stays inside the panel --------------------------------------------------
    function inPanel(item: Item): bool {
        for (let p = item; p; p = p.parent)
            if (p === panel)
                return true
        return false
    }
    function cycleFocus(forward: bool) {
        let it = sheet.Window.activeFocusItem
        if (!it || !inPanel(it)) {
            filter.forceActiveFocus(forward ? Qt.TabFocusReason : Qt.BacktabFocusReason)
            return
        }
        for (let n = 0; n < 400 && it; ++n) {
            it = it.nextItemInFocusChain(forward)
            if (it && inPanel(it) && it.visible && it.enabled) {
                it.forceActiveFocus(forward ? Qt.TabFocusReason : Qt.BacktabFocusReason)
                return
            }
        }
    }

    // the scrim: takes every click and hover meant for what lies under it
    Rectangle {
        anchors.fill: parent
        color: Pe.alpha(Pe.dark ? "#000000" : "#0D1222", Pe.dark ? 0.5 : 0.18)
        opacity: sheet.open ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: Pe.tSheet } }
        MouseArea {
            anchors.fill: parent
            enabled: sheet.open
            hoverEnabled: true
            acceptedButtons: Qt.AllButtons
            onClicked: sheet.close()
            onWheel: (wheel) => wheel.accepted = true
        }
    }

    Rectangle {
        id: panel
        x: sheet.width < 640 ? 10 : sheet.anchorX
        y: sheet.anchorY + (sheet.open ? 0 : -8)
        width: Math.min(420, sheet.width - 20)
        height: Math.min(col.implicitHeight + 24, sheet.height - y - 16)
        radius: Pe.rSheet
        color: Pe.sheet
        border.width: 1
        border.color: Pe.lineStrong
        opacity: sheet.open ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: Pe.tSheet } }
        Behavior on y { NumberAnimation { duration: Pe.tSheet; easing.type: Easing.OutCubic } }
        Accessible.role: Accessible.Dialog
        Accessible.name: qsTr("Conversations")
        Keys.onTabPressed: (event) => {
            sheet.cycleFocus(true)
            event.accepted = true
        }
        Keys.onBacktabPressed: (event) => {
            sheet.cycleFocus(false)
            event.accepted = true
        }
        // clicks on the panel's own background stay on the panel (not the scrim)
        MouseArea {
            anchors.fill: parent
            enabled: sheet.open
            hoverEnabled: true
            acceptedButtons: Qt.AllButtons
        }

        ColumnLayout {
            id: col
            x: 12; y: 12
            width: parent.width - 24
            height: parent.height - 24
            spacing: 10

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 8
                spacing: 8
                Text {
                    textFormat: Text.PlainText
                    text: qsTr("Conversations")
                    font.family: Pe.sans
                    font.pixelSize: 20
                    font.weight: Font.Light
                    color: Pe.ink
                    Accessible.role: Accessible.Heading
                    Accessible.name: text
                }
                Text {
                    textFormat: Text.PlainText
                    Layout.alignment: Qt.AlignBaseline
                    text: sheet.filterText === "" ? String(sheet.stage.conversations.count)
                        : qsTr("%1 of %2").arg(sheet.shownRows.length).arg(sheet.stage.conversations.count)
                    font.family: Pe.mono
                    font.pixelSize: Pe.small
                    color: Pe.inkMute
                }
                Item { Layout.fillWidth: true }
                PeButton {
                    compact: true
                    kind: "primary"
                    iconName: "plus"
                    text: qsTr("New")
                    hint: "Ctrl+N"
                    enabled: !sheet.stage.busy
                    onClicked: {
                        if (sheet.stage.newConversation())
                            sheet.close()
                    }
                }
            }

            // filter
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 36
                radius: 18
                color: Pe.sunken
                border.width: 1
                border.color: filter.activeFocus ? Pe.alpha(Pe.violet, 0.6) : Pe.line
                PeIcon { x: 12; anchors.verticalCenter: parent.verticalCenter; name: "search"; size: 15; color: Pe.inkMute }
                TextField {
                    id: filter
                    anchors.fill: parent
                    anchors.leftMargin: 32
                    anchors.rightMargin: 10
                    background: null
                    placeholderText: qsTr("Find a conversation")
                    placeholderTextColor: Pe.inkMute
                    color: Pe.ink
                    font.family: Pe.sans
                    font.pixelSize: Pe.ui
                    verticalAlignment: TextInput.AlignVCenter
                    Accessible.name: qsTr("Find a conversation")
                    Keys.onDownPressed: list.forceActiveFocus()
                    Keys.onReturnPressed: {
                        const id = sheet.idAt(list.currentIndex)
                        if (id !== "" && sheet.shown(list.currentIndex))
                            sheet.choose(id)
                    }
                    Keys.onEnterPressed: {
                        const id = sheet.idAt(list.currentIndex)
                        if (id !== "" && sheet.shown(list.currentIndex))
                            sheet.choose(id)
                    }
                }
            }

            Text {
                textFormat: Text.PlainText
                visible: sheet.stage.busy
                Layout.fillWidth: true
                Layout.leftMargin: 8
                wrapMode: Text.Wrap
                text: qsTr("Shadow is working — switching waits until she's done.")
                font.family: Pe.sans
                font.pixelSize: Pe.small
                color: Pe.caution
            }

            // nothing to list: say so (no conversations yet, or none matches the filter)
            Text {
                textFormat: Text.PlainText
                visible: sheet.shownRows.length === 0
                Layout.fillWidth: true
                Layout.topMargin: 10
                Layout.bottomMargin: 10
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: sheet.stage.conversations.count === 0
                      ? qsTr("No conversations yet — say “Shadow” or type to start one.")
                      : qsTr("No conversation matches “%1”.").arg(sheet.filterText)
                font.family: Pe.sans
                font.pixelSize: Pe.ui
                color: Pe.inkMute
            }

            ListView {
                id: list
                visible: sheet.shownRows.length > 0
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredHeight: contentHeight
                clip: true
                model: sheet.stage.conversations
                boundsBehavior: Flickable.StopAtBounds
                // ↑ ↓ are handled below: they skip the rows the filter hides
                keyNavigationEnabled: false
                activeFocusOnTab: true
                highlightFollowsCurrentItem: false
                ScrollBar.vertical: PeScrollBar {}
                Accessible.role: Accessible.List
                Accessible.name: qsTr("Conversations")
                // a delete waiting for confirmation: Enter (or Del again) deletes, Esc keeps it
                Keys.onShortcutOverride: (event) => event.accepted = event.key === Qt.Key_Escape && sheet.confirmingId !== ""
                Keys.onPressed: (event) => {
                    if (event.key === Qt.Key_Up || event.key === Qt.Key_Down) {
                        sheet.confirmingId = ""
                        sheet.move(event.key === Qt.Key_Up ? -1 : 1)
                        event.accepted = true
                        return
                    }
                    if (event.key === Qt.Key_Home || event.key === Qt.Key_End) {
                        sheet.move(event.key === Qt.Key_Home ? -2 : 2)
                        event.accepted = true
                        return
                    }
                    // only a row that is shown can be opened, renamed or deleted
                    const id = sheet.shown(list.currentIndex) ? sheet.idAt(list.currentIndex) : ""
                    if (id === "")
                        return
                    if (sheet.confirmingId !== "") {
                        if (event.key === Qt.Key_Escape) {
                            sheet.confirmingId = ""
                            event.accepted = true
                            return
                        }
                        if (sheet.confirmingId === id && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                                                          || event.key === Qt.Key_Delete)) {
                            sheet.stage.deleteConversation(id)
                            sheet.confirmingId = ""
                            event.accepted = true
                            return
                        }
                    }
                    if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                        sheet.choose(id)
                        event.accepted = true
                    } else if (event.key === Qt.Key_F2) {
                        sheet.confirmingId = ""
                        sheet.renamingId = id
                        event.accepted = true
                    } else if (event.key === Qt.Key_Delete) {
                        sheet.confirmingId = id
                        event.accepted = true
                    }
                }

                delegate: Item {
                    id: row
                    required property int index
                    required property string id
                    required property string title
                    required property string subtitle
                    readonly property string convId: id
                    readonly property bool current: sheet.stage.activeId === id
                    readonly property bool matches: sheet.matchesTitle(title)
                    readonly property bool renaming: sheet.renamingId === id
                    readonly property bool confirming: sheet.confirmingId === id
                    readonly property bool keyed: ListView.isCurrentItem && list.activeFocus
                    width: list.width
                    height: matches ? (confirming ? 92 : 58) : 0
                    visible: matches
                    Accessible.role: Accessible.ListItem
                    Accessible.name: title + ", " + subtitle + (current ? ", " + qsTr("open") : "")

                    function saveName() {
                        sheet.stage.renameConversation(convId, renameField.text)
                        endRename()
                    }
                    function endRename() {
                        sheet.renamingId = ""
                        if (sheet.closeAfterRename) {
                            sheet.close()
                            return
                        }
                        list.currentIndex = index
                        list.forceActiveFocus()
                    }

                    HoverHandler { id: rowHover }
                    Rectangle {
                        anchors.fill: parent
                        anchors.topMargin: 2
                        anchors.bottomMargin: 2
                        radius: 12
                        color: row.confirming ? Pe.alpha(Pe.alarm, 0.08)
                             : row.current ? Pe.alpha(Pe.violet, Pe.dark ? 0.14 : 0.09)
                             : rowHover.hovered || row.keyed ? Pe.alpha(Pe.ink, 0.05) : "transparent"
                        border.width: row.keyed ? 2 : 0
                        border.color: Pe.cyan
                    }
                    TapHandler {
                        enabled: !row.renaming && !row.confirming
                        onTapped: sheet.choose(row.convId)
                    }

                    // each conversation has its own little constellation
                    ConstellationMark {
                        x: 10; y: 14
                        seed: row.convId + row.title
                        lit: row.current
                    }

                    ColumnLayout {
                        visible: !row.renaming
                        x: 42
                        y: 10
                        width: parent.width - x - (actions.visible ? actions.width + 8 : 12)
                        spacing: 2
                        Text {
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            text: sheet.stage.oneLine(row.title)
                            elide: Text.ElideRight
                            maximumLineCount: 1
                            font.family: Pe.sans
                            font.pixelSize: 14
                            font.weight: row.current ? Font.DemiBold : Font.Medium
                            color: Pe.ink
                        }
                        Text {
                            textFormat: Text.PlainText
                            Layout.fillWidth: true
                            text: row.confirming ? qsTr("Delete this conversation? It can't be undone.") : row.subtitle
                            elide: Text.ElideRight
                            maximumLineCount: 1
                            font.family: Pe.sans
                            font.pixelSize: Pe.small
                            color: row.confirming ? Pe.alarm : Pe.inkMute
                        }
                    }
                    Row {
                        visible: row.confirming
                        x: 42
                        y: 52
                        spacing: 8
                        PeButton {
                            compact: true; kind: "danger"; text: qsTr("Delete"); iconName: "trash"
                            hint: row.keyed ? "Enter" : ""
                            onClicked: {
                                sheet.stage.deleteConversation(row.convId)
                                sheet.confirmingId = ""
                                list.forceActiveFocus()
                            }
                        }
                        PeButton {
                            compact: true; kind: "ghost"; text: qsTr("Keep it")
                            hint: row.keyed ? "Esc" : ""
                            onClicked: {
                                sheet.confirmingId = ""
                                list.forceActiveFocus()
                            }
                        }
                    }

                    // inline rename
                    RowLayout {
                        visible: row.renaming
                        x: 30
                        width: parent.width - 42
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 6
                        TextField {
                            id: renameField
                            Layout.fillWidth: true
                            text: row.title
                            font.family: Pe.sans
                            font.pixelSize: 14
                            color: Pe.ink
                            selectByMouse: true
                            Accessible.name: qsTr("Conversation name")
                            // A hidden field never keeps the keyboard (the list is a
                            // focus scope: it would hand the keys back to it).
                            onVisibleChanged: {
                                if (visible) {
                                    text = row.title
                                    selectAll()
                                    forceActiveFocus()
                                } else {
                                    focus = false
                                }
                            }
                            // Enter saves here and goes no further (the list would open the row)
                            Keys.onReturnPressed: (event) => {
                                row.saveName()
                                event.accepted = true
                            }
                            Keys.onEnterPressed: (event) => {
                                row.saveName()
                                event.accepted = true
                            }
                            // Esc ends the rename (not the window's Esc shortcut)
                            Keys.onShortcutOverride: (event) => event.accepted = event.key === Qt.Key_Escape
                            Keys.onEscapePressed: (event) => {
                                row.endRename()
                                event.accepted = true
                            }
                        }
                        PeIconButton {
                            size: 30; iconSize: 15; iconName: "check"; tip: qsTr("Save name")
                            onClicked: row.saveName()
                        }
                        PeIconButton {
                            size: 30; iconSize: 15; iconName: "close"; tip: qsTr("Cancel")
                            onClicked: row.endRename()
                        }
                    }

                    Row {
                        id: actions
                        visible: !row.renaming && !row.confirming && (rowHover.hovered || row.current || row.keyed)
                        anchors.right: parent.right
                        anchors.rightMargin: 8
                        y: 12
                        spacing: 0
                        PeIconButton {
                            size: 32; iconSize: 15; iconName: "edit"; tip: qsTr("Rename (F2)")
                            accessibleName: qsTr("Rename “%1”").arg(sheet.stage.oneLine(row.title))
                            onClicked: {
                                sheet.confirmingId = ""
                                sheet.renamingId = row.convId
                            }
                        }
                        PeIconButton {
                            size: 32; iconSize: 15; iconName: "trash"; tip: qsTr("Delete (Del)")
                            accessibleName: qsTr("Delete “%1”").arg(sheet.stage.oneLine(row.title))
                            onClicked: {
                                list.currentIndex = row.index
                                sheet.confirmingId = row.convId
                            }
                        }
                    }
                }
            }

            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                Layout.leftMargin: 8
                text: qsTr("↑↓ move · Enter open · F2 rename · Del delete · Esc close")
                elide: Text.ElideRight
                font.family: Pe.sans
                font.pixelSize: Pe.label
                color: Pe.inkMute
            }
        }
    }
}
