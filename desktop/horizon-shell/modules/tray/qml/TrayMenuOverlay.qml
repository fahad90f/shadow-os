// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import org.nexus.ui

// The menu of a tray item (com.canonical.dbusmenu), drawn by the shell in
// the NEXUS style: a card under the item's icon, submenus as further cards
// beside it. An overlay: Escape or a click elsewhere closes it.
//
// Keyboard: Up/Down (Home/End) move, Right or Return opens a submenu, Left
// or Escape leaves it, Return or Space chooses an entry.
OverlaySurface {
    id: root

    required property var shell

    property var item: null                 // TrayItem
    property var menu: null                 // its TrayMenu
    property int anchorX: 0                 // centre of the icon on the screen
    // The cards showing: [{ parentId, left, right, top }] — the first is the
    // menu itself, each further one a submenu opened from a row of the card
    // before it (whose left and right edges and row position it remembers).
    property var levels: []
    // The card of this level selects its first entry when it appears
    // (a submenu opened with the keyboard); -1: none.
    property int selectFirstInLevel: -1
    readonly property int margin: Theme.spaceSm
    readonly property int rowHeight: 30

    screen: shell.primaryScreen
    scope: "nexus-tray-menu"

    function showFor(newItem, x) {
        if (open && menu) menu.close()
        item = newItem
        menu = newItem.menu
        anchorX = x
        levels = [{ parentId: 0, left: 0, right: 0, top: 0, side: 1 }]
        if (!menu) { hide(); return }
        if (open) { menu.open(); tray.menuItem = item }
        else shell.present(root)
    }
    function release() {
        if (menu) menu.close()
        tray.menuItem = null
        levels = []
    }
    // Closes the submenus above 'level' (0: only the menu itself stays).
    function truncate(level) {
        if (levels.length <= level + 1) return
        for (let i = levels.length - 1; i > level; --i) menu.closeSubmenu(levels[i].parentId)
        levels = levels.slice(0, level + 1)
    }
    function openSubmenu(level, id, card, rowY) {
        if (levels.length > level + 1 && levels[level + 1].parentId === id) {
            // Already showing; a key press still moves into it.
            if (selectFirstInLevel === level + 1) {
                selectFirstInLevel = -1
                const shown = cards.itemAt(level + 1)
                if (shown && shown.current < 0) shown.move(1)
            }
            return
        }
        truncate(level)
        menu.openSubmenu(id)
        levels = levels.concat([{ parentId: id, left: card.x, right: card.x + card.width, top: card.y + rowY,
                                  side: card.side }])
    }
    function choose(id) {
        menu.trigger(id, root)
        hide()
    }
    function topCard() { return cards.itemAt(levels.length - 1) }

    onOpenChanged: {
        if (open && menu) { menu.open(); tray.menuItem = item; keys.forceActiveFocus() }
        else if (!open) release()
    }

    Connections {
        target: tray
        function onMenuRequested(item, x) { root.showFor(item, x) }
        function onItemRemoved(item) { if (item === root.item) { root.hide(); root.item = null; root.menu = null } }
    }
    Connections {
        target: root.item
        // The item exports another menu object now.
        function onMenuChanged() { root.hide() }
    }

    FocusScope {
        id: keys
        anchors.fill: parent
        focus: true

        Keys.onDownPressed: { const c = root.topCard(); if (c) c.move(1) }
        Keys.onUpPressed: { const c = root.topCard(); if (c) c.move(-1) }
        Keys.onPressed: event => {
            const c = root.topCard()
            if (!c) return
            if (event.key === Qt.Key_Home) { c.current = -1; c.move(1); event.accepted = true }
            else if (event.key === Qt.Key_End) { c.current = c.rows.length; c.move(-1); event.accepted = true }
        }
        Keys.onRightPressed: { const c = root.topCard(); if (c) c.enter(false) }
        Keys.onLeftPressed: if (root.levels.length > 1) root.truncate(root.levels.length - 2)
        Keys.onReturnPressed: { const c = root.topCard(); if (c) c.enter(true) }
        Keys.onEnterPressed: { const c = root.topCard(); if (c) c.enter(true) }
        Keys.onSpacePressed: { const c = root.topCard(); if (c) c.enter(true) }
        Keys.onEscapePressed: event => {
            if (root.levels.length > 1) root.truncate(root.levels.length - 2)
            else event.accepted = false     // the overlay closes
        }

        Repeater {
            id: cards
            model: root.levels.length

            Rectangle {
                id: card

                required property int index
                readonly property var level: root.levels[index] || { parentId: 0, left: 0, right: 0, top: 0, side: 1 }
                // Re-read whenever the application changes the menu.
                readonly property var rows: root.menu && root.menu.loaded
                    ? (root.menu.revision, root.menu.entries(level.parentId)) : []
                readonly property bool waiting: root.menu !== null && !root.menu.loaded && root.menu.failure === ""
                readonly property bool failed: root.menu !== null && !root.menu.loaded && root.menu.failure !== ""
                property int current: -1
                readonly property real maxHeight: root.height - Theme.panelHeight - root.margin * 2
                readonly property real labelWidth: {
                    let widest = 0
                    for (const row of rows) widest = Math.max(widest, metrics.advanceWidth(row.label))
                    return widest
                }

                // Moves the selection to the next entry that can be chosen.
                function move(step) {
                    const n = rows.length
                    let i = current
                    for (let tries = 0; tries < n; ++tries) {
                        i += step
                        if (i < 0) i = n - 1
                        if (i >= n) i = 0
                        if (!rows[i].separator && rows[i].enabled) {
                            current = i
                            list.positionViewAtIndex(i, ListView.Contain)
                            return
                        }
                    }
                }
                // Return/Space ('choose') or Right on the selected entry.
                function enter(choose) {
                    if (current < 0 || current >= rows.length) { move(1); return }
                    const row = rows[current]
                    if (row.separator || !row.enabled) return
                    if (row.submenu) {
                        const delegate = list.itemAtIndex(current)
                        root.selectFirstInLevel = index + 1
                        root.openSubmenu(index, row.id, card, delegate ? delegate.mapToItem(card, 0, 0).y : 0)
                    } else if (choose) {
                        root.choose(row.id)
                    }
                }

                FontMetrics { id: metrics; font.family: Theme.fontUi; font.pixelSize: Theme.fontSizeBody }
                Component.onCompleted: if (index === root.selectFirstInLevel) { root.selectFirstInLevel = -1; move(1) }

                width: Math.round(Math.min(420, Math.max(200, labelWidth + 96)))
                height: Math.min(maxHeight, body.height + Theme.spaceXs * 2)
                // The menu hangs under its icon; a submenu opens to the right of
                // the row it belongs to, or to the left where there is no room.
                // submenus keep going the way the one before went, so that
                // they do not cover the cards they came from.
                readonly property bool fitsRight: level.right - 4 + width <= root.width - root.margin
                readonly property bool fitsLeft: level.left - width + 4 >= root.margin
                readonly property int side: index === 0 ? 1
                    : level.side > 0 ? (fitsRight || !fitsLeft ? 1 : -1) : (fitsLeft || !fitsRight ? -1 : 1)
                x: index === 0
                    ? Math.max(root.margin, Math.min(root.width - width - root.margin, root.anchorX - width / 2))
                    : side > 0 ? Math.min(level.right - 4, root.width - width - root.margin)
                               : Math.max(root.margin, level.left - width + 4)
                y: index === 0
                    ? Theme.panelHeight + Theme.spaceXs
                    : Math.max(Theme.panelHeight + Theme.spaceXs,
                               Math.min(root.height - height - root.margin, level.top - Theme.spaceXs))
                radius: Theme.radiusMd
                color: Theme.bgOverlay
                border.width: Theme.borderWidth
                border.color: index === root.levels.length - 1 ? Theme.borderStrong : Theme.border
                Accessible.role: Accessible.PopupMenu
                Accessible.name: root.item ? root.item.toolTipTitle : ""

                // Clicks on the card itself do not close the menu.
                TapHandler { gesturePolicy: TapHandler.ReleaseWithinBounds }
                NumberAnimation on opacity { from: 0; to: 1; duration: Theme.motionFast }

                Column {
                    id: body
                    x: Theme.spaceXs
                    y: Theme.spaceXs
                    width: parent.width - Theme.spaceXs * 2

                    // Whose menu this is.
                    Text {
                        visible: card.index === 0 && text !== ""
                        width: parent.width
                        height: 26
                        leftPadding: Theme.spaceSm
                        rightPadding: Theme.spaceSm
                        verticalAlignment: Text.AlignVCenter
                        text: root.item ? root.item.toolTipTitle : ""
                        textFormat: Text.PlainText
                        color: Theme.textMuted
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeCaption
                        font.weight: Theme.weightSemibold
                        font.capitalization: Font.AllUppercase
                        font.letterSpacing: 0.8
                        elide: Text.ElideRight
                    }

                    // The application has not answered yet / does not answer.
                    Row {
                        visible: card.waiting
                        height: root.rowHeight
                        leftPadding: Theme.spaceSm
                        spacing: Theme.spaceSm
                        BusyIndicator { width: 18; height: 18; padding: 0; running: card.waiting; anchors.verticalCenter: parent.verticalCenter }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("Asking the application…")
                            color: Theme.textMuted
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeBody
                        }
                    }
                    Text {
                        visible: card.failed || (!card.waiting && card.rows.length === 0)
                        width: parent.width
                        padding: Theme.spaceSm
                        text: !card.failed ? qsTr("This menu is empty.")
                            : root.menu && root.menu.failure === "timeout" ? qsTr("The application does not answer.")
                            : qsTr("The application did not provide its menu.")
                        color: Theme.textMuted
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeBody
                        wrapMode: Text.Wrap
                    }

                    ListView {
                        id: list
                        width: parent.width
                        height: Math.min(contentHeight, card.maxHeight - Theme.spaceXs * 2 - 26)
                        visible: card.rows.length > 0
                        clip: true
                        interactive: contentHeight > height
                        boundsBehavior: Flickable.StopAtBounds
                        model: card.rows
                        ScrollBar.vertical: ScrollBar { policy: list.interactive ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff }

                        delegate: Item {
                            id: entry
                            required property var modelData
                            required property int index
                            readonly property bool selectable: !modelData.separator && modelData.enabled
                            readonly property bool selected: card.current === index && selectable
                            readonly property bool expanded: root.levels.length > card.index + 1
                                && root.levels[card.index + 1].parentId === modelData.id

                            // Room for the scroll bar of a menu taller than the screen.
                            width: ListView.view.width - (list.interactive ? Theme.spaceMd : 0)
                            height: modelData.separator ? 9 : root.rowHeight
                            Accessible.role: modelData.separator ? Accessible.Separator
                                : modelData.toggle === "check" ? Accessible.CheckBox
                                : modelData.toggle === "radio" ? Accessible.RadioButton : Accessible.MenuItem
                            Accessible.name: modelData.label
                            Accessible.checked: modelData.state === 1

                            Rectangle {
                                visible: entry.modelData.separator
                                anchors.verticalCenter: parent.verticalCenter
                                x: Theme.spaceSm
                                width: parent.width - Theme.spaceSm * 2
                                height: 1
                                color: Theme.border
                            }
                            Rectangle {
                                visible: !entry.modelData.separator
                                anchors.fill: parent
                                radius: Theme.radiusSm
                                color: rowTap.pressed && entry.selectable ? Theme.pressedOverlay
                                    : entry.selected || entry.expanded ? Theme.hoverOverlay : "transparent"
                            }

                            // Check mark, radio dot or the entry's icon.
                            Item {
                                id: lead
                                visible: !entry.modelData.separator
                                x: Theme.spaceSm
                                anchors.verticalCenter: parent.verticalCenter
                                width: Theme.iconSmall
                                height: Theme.iconSmall
                                NxIcon {
                                    anchors.centerIn: parent
                                    visible: entry.modelData.toggle !== "" && entry.modelData.state !== 0
                                    name: entry.modelData.state === 2 ? "minus"
                                        : entry.modelData.toggle === "radio" ? "dot" : "check"
                                    size: Theme.iconSmall
                                    color: entry.modelData.enabled ? Theme.accent : Theme.textDisabled
                                }
                                Image {
                                    anchors.fill: parent
                                    visible: entry.modelData.toggle === "" && entry.modelData.icon !== ""
                                    source: visible
                                        ? entry.modelData.icon + "?fg=" + root.rgb(Theme.textPrimary) + "&bg=" + root.rgb(Theme.bgOverlay)
                                        : ""
                                    sourceSize: Qt.size(32, 32)
                                    opacity: entry.modelData.enabled ? 1 : 0.4
                                    asynchronous: false
                                    cache: false
                                    smooth: true
                                }
                            }
                            Text {
                                visible: !entry.modelData.separator
                                anchors.left: lead.right
                                anchors.leftMargin: Theme.spaceSm
                                anchors.right: chevron.left
                                anchors.rightMargin: Theme.spaceXs
                                anchors.verticalCenter: parent.verticalCenter
                                text: entry.modelData.label
                                textFormat: Text.PlainText
                                color: entry.modelData.enabled ? Theme.textPrimary : Theme.textDisabled
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fontSizeBody
                                elide: Text.ElideRight
                                maximumLineCount: 1
                            }
                            NxIcon {
                                id: chevron
                                anchors.right: parent.right
                                anchors.rightMargin: Theme.spaceXs
                                anchors.verticalCenter: parent.verticalCenter
                                visible: !entry.modelData.separator
                                opacity: entry.modelData.submenu ? 1 : 0
                                name: "chevron-right"
                                size: Theme.iconSmall
                                color: entry.modelData.enabled ? Theme.textSecondary : Theme.textDisabled
                            }

                            HoverHandler {
                                enabled: entry.selectable
                                cursorShape: Qt.PointingHandCursor
                                onHoveredChanged: if (hovered) {
                                    card.current = entry.index
                                    hoverDelay.restart()
                                }
                            }
                            // Resting on a row opens its submenu, or closes the one of another row.
                            Timer {
                                id: hoverDelay
                                interval: 220
                                onTriggered: {
                                    if (card.current !== entry.index) return
                                    if (entry.modelData.submenu)
                                        root.openSubmenu(card.index, entry.modelData.id, card, entry.mapToItem(card, 0, 0).y)
                                    else
                                        root.truncate(card.index)
                                }
                            }
                            TapHandler {
                                id: rowTap
                                enabled: entry.selectable
                                gesturePolicy: TapHandler.ReleaseWithinBounds
                                onTapped: {
                                    if (entry.modelData.submenu)
                                        root.openSubmenu(card.index, entry.modelData.id, card, entry.mapToItem(card, 0, 0).y)
                                    else
                                        root.choose(entry.modelData.id)
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    function rgb(c) { const s = c.toString(); return s.length === 9 ? s.substring(3) : s.substring(1) }
}
