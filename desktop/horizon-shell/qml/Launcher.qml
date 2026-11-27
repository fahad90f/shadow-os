// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import "Fx.js" as Fx

// The application launcher: type to search, arrow keys to move, Enter to
// start. Categories on the left narrow the grid.
//
// How it opens (windows of the overlay's 'reveal' clock, 300 ms in all;
// see OverlaySurface): the search field is there, and has the keyboard, in
// the first frame. The card's outline is traced from its top left corner
// both ways round, its fill fades in, and a scan line runs from under the
// search field to the bottom edge; categories and applications appear as
// the line reaches them. Four brackets then close onto the card's corners,
// and the outline settles from the accent colour towards the border colour
// (it keeps a trace of the accent while the launcher is open). Nothing is
// moved or resized on the way: every row and cell is in its place and takes
// clicks from the first frame. Closing, everything fades as one
// (OverlaySurface.fade), except the tile of an application that was just
// started: it stays lit until the surface is gone.
OverlaySurface {
    id: root

    scope: "shadow-launcher"
    scrimOpacity: 0.55
    // The content fades itself (see launchedIndex).
    fadeContent: false

    signal sessionMenuRequested()

    // The scan line's window of the clock; how long a row takes and when
    // the last one is complete.
    readonly property real beamStart: 0.13
    readonly property real beamEnd: 0.8
    readonly property real rowSpan: 0.25
    readonly property real rowLatest: 0.95
    // Rows appear behind the scan line only when the launcher opens as it
    // is: once something is typed, what the grid shows is simply there.
    property bool staggered: true
    // The application that was just started: its tile stays lit while the
    // rest of the launcher goes.
    property int launchedIndex: -1

    onOpenChanged: {
        if (open) {
            search.text = ""
            applications.filter = ""
            applications.group = ""
            grid.currentIndex = 0
            launchedIndex = -1
            staggered = true
            search.forceActiveFocus()
        }
    }

    function launchCurrent() {
        if (grid.currentIndex >= 0 && applications.launchAt(grid.currentIndex, root)) {
            launchedIndex = grid.currentIndex
            root.hide()
        }
    }

    // 0..1 for a row or cell whose LOWER edge is at 'y' in the card: it
    // starts once the scan line has passed it, so nothing shows ahead of the
    // line; cells further right follow Theme.motionStagger apart (not in the
    // software renderer). 'counted' is false for rows beyond the twelfth:
    // those come all at once with the last stagger step, never before the
    // rows above them.
    function appearAt(y, column, counted) {
        const clock = root.stage        // read first: bindings follow the clock
        if (clock >= 1 || !root.staggered) return 1
        if (!counted) return root.ph(rowLatest - rowSpan, rowLatest)
        const fraction = (y - beam.from) / Math.max(1, card.height - beam.from)
        const lag = root.lite || root.revealDuration <= 0 ? 0 : column * Theme.motionStagger / root.revealDuration
        const start = Math.min(Fx.itemStart(beamStart, beamEnd, fraction) + lag, rowLatest - rowSpan)
        return root.ph(start, start + rowSpan)
    }

    // The card never moves and is never scaled or clipped: its blurred
    // background would not follow. Only the opacity of its fill changes.
    Item {
        id: card
        anchors.centerIn: parent
        width: Math.min(980, root.width - 96)
        height: Math.min(660, root.height - 140)
        readonly property real radius: Theme.radiusXl

        Rectangle {
            anchors.fill: parent
            radius: card.radius
            color: cardBlur.glass ? Theme.glassPopup : Theme.bgOverlay
            border.width: Theme.borderWidth
            border.color: Theme.border
            opacity: root.ph(0, 0.33) * root.fade
        }
        // The blur switches on when the fill is mostly there.
        BlurBehind { id: cardBlur; anchors.fill: parent; radius: card.radius; surface: root; visible: root.reveal >= 0.3 }

        // Swallow clicks so they do not reach the scrim and dismiss the launcher.
        TapHandler { }

        ColumnLayout {
            id: column
            anchors.fill: parent
            anchors.margins: Theme.spaceXl
            spacing: Theme.spaceLg

            NxSearchField {
                id: search
                Layout.fillWidth: true
                Layout.preferredHeight: Theme.controlHeightLarge + 4
                placeholderText: qsTr("Search applications")
                font.pixelSize: Theme.fontSizeBodyLarge
                // Readable in the very first frame; typing works while the reveal runs.
                opacity: (0.5 + 0.5 * root.ph(0, 0.2)) * root.fade
                onTextChanged: {
                    if (text !== "") root.staggered = false
                    applications.filter = text
                    grid.currentIndex = applications.count > 0 ? 0 : -1
                }
                Keys.onDownPressed: { grid.forceActiveFocus(); if (grid.currentIndex < 0) grid.currentIndex = 0 }
                Keys.onReturnPressed: root.launchCurrent()
                Keys.onEnterPressed: root.launchCurrent()
                // Escape: the field clears its text first (NxSearchField); on an
                // empty field it passes the key on and the overlay closes.
            }

            RowLayout {
                id: lists
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: Theme.spaceLg

                // ---- categories -------------------------------------------------
                ListView {
                    id: groupList
                    Layout.preferredWidth: 168
                    Layout.fillHeight: true
                    clip: true
                    spacing: 2
                    opacity: root.fade
                    visible: search.text === ""
                    model: [qsTr("All applications")].concat(applications.groups)
                    delegate: Item {
                        id: groupItem
                        required property string modelData
                        required property int index
                        readonly property string key: index === 0 ? "" : modelData
                        readonly property bool selected: applications.group === key
                        readonly property real appear: root.appearAt(column.y + lists.y + y + height - groupList.contentY, 0, index < 12)
                        width: groupList.width
                        height: 34
                        // What is drawn; the row itself keeps its place.
                        Rectangle {
                            anchors.fill: parent
                            radius: Theme.radiusSm
                            color: groupItem.selected ? Theme.accentFill : groupHover.hovered ? Theme.hoverOverlay : "transparent"
                            opacity: groupItem.appear
                            transform: Translate { y: 6 * (1 - groupItem.appear) }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                x: Theme.spaceMd
                                width: parent.width - Theme.spaceMd * 2
                                text: groupItem.modelData
                                color: groupItem.selected ? Theme.textPrimary : Theme.textSecondary
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fontSizeBody
                                font.weight: groupItem.selected ? Theme.weightSemibold : Theme.weightMedium
                                elide: Text.ElideRight
                            }
                        }
                        HoverHandler { id: groupHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler { onTapped: { applications.group = groupItem.key; grid.currentIndex = 0 } }
                    }
                }

                // ---- application grid ---------------------------------------------
                GridView {
                    id: grid
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    cellWidth: Math.floor(width / Math.max(1, Math.floor(width / 128)))
                    cellHeight: 118
                    model: applications
                    boundsBehavior: Flickable.StopAtBounds
                    keyNavigationEnabled: true
                    highlightMoveDuration: 0
                    ScrollBar.vertical: ScrollBar { opacity: root.fade }

                    Keys.onReturnPressed: root.launchCurrent()
                    Keys.onEnterPressed: root.launchCurrent()
                    Keys.onPressed: (event) => {
                        // Typing while the grid has focus continues the search.
                        if (event.text.length === 1 && event.text.charCodeAt(0) >= 32 && !(event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier))) {
                            search.forceActiveFocus()
                            search.text += event.text
                            event.accepted = true
                        } else if (event.key === Qt.Key_Backspace) {
                            search.forceActiveFocus()
                            search.text = search.text.slice(0, -1)
                            event.accepted = true
                        } else if (event.key === Qt.Key_Up && grid.currentIndex < Math.floor(grid.width / grid.cellWidth)) {
                            search.forceActiveFocus()
                            event.accepted = true
                        }
                    }

                    delegate: Item {
                        id: cell
                        required property int index
                        required property string name
                        required property string icon
                        required property string comment
                        readonly property bool current: GridView.isCurrentItem
                        readonly property bool launched: index === root.launchedIndex
                        readonly property int gridColumn: Math.round(x / grid.cellWidth)
                        // The software renderer staggers the rows that begin
                        // within the first twelve applications; the rest is there.
                        readonly property real appear: launched ? 1
                            : root.appearAt(column.y + lists.y + y + height - grid.contentY, gridColumn,
                                            root.lite ? index - gridColumn < 12 : y - grid.contentY < grid.cellHeight * 12)
                        width: grid.cellWidth
                        height: grid.cellHeight
                        Accessible.role: Accessible.Button
                        Accessible.name: name
                        Accessible.description: comment

                        // What is drawn: it fades in and rises 6 px. The cell
                        // itself, which takes the clicks, keeps its place.
                        Item {
                            anchors.fill: parent
                            opacity: cell.launched ? 1 : cell.appear * root.fade
                            transform: Translate { y: 6 * (1 - cell.appear) }

                            Rectangle {
                                id: tile
                                anchors.fill: parent
                                anchors.margins: 4
                                radius: Theme.radiusMd
                                color: cell.launched ? Theme.accentFillStrong
                                    : cell.current ? Theme.accentFill : cellHover.hovered ? Theme.hoverOverlay : "transparent"
                                border.width: cell.launched || (cell.current && grid.activeFocus) ? 1 : 0
                                border.color: Theme.alpha(Theme.accent, cell.launched ? 1 : 0.6)
                            }
                            // The selection does not glide: its corner marks
                            // fade in on the tile that is current now.
                            Loader {
                                anchors.fill: tile
                                anchors.margins: 3
                                active: cell.current
                                sourceComponent: FxBrackets {
                                    arm: 8
                                    NumberAnimation on strength { from: 0; to: 1; duration: Theme.motionInstant }
                                }
                            }
                            NxAppIcon {
                                id: cellIcon
                                anchors.horizontalCenter: parent.horizontalCenter
                                y: 14
                                name: cell.icon
                                size: 56
                            }
                            Text {
                                anchors.top: cellIcon.bottom
                                anchors.topMargin: 8
                                anchors.horizontalCenter: parent.horizontalCenter
                                width: parent.width - 16
                                horizontalAlignment: Text.AlignHCenter
                                text: cell.name
                                color: Theme.textPrimary
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fontSizeSmall
                                font.weight: Theme.weightMedium
                                elide: Text.ElideRight
                                maximumLineCount: 2
                                wrapMode: Text.Wrap
                            }
                        }
                        HoverHandler { id: cellHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler {
                            onTapped: { grid.currentIndex = cell.index; root.launchCurrent() }
                        }
                    }

                    NxEmptyState {
                        anchors.fill: parent
                        opacity: root.fade
                        visible: grid.count === 0
                        icon: "search"
                        title: search.text !== "" ? qsTr("No application matches “%1”").arg(search.text)
                                                   : qsTr("No applications in this category")
                        message: applications.totalCount === 0
                            ? qsTr("The application database is empty. It is rebuilt automatically when applications are installed.") : ""
                    }
                }
            }

            // ---- footer ---------------------------------------------------------
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceMd
                opacity: root.ph(0.75, 1) * root.fade
                Rectangle {
                    implicitWidth: 34; implicitHeight: 34; radius: 17
                    color: Theme.accentFill
                    NxIcon { anchors.centerIn: parent; name: "user"; size: Theme.icon; color: Theme.accent }
                }
                ColumnLayout {
                    spacing: 0
                    Text {
                        text: session.fullName
                        color: Theme.textPrimary
                        font.family: Theme.fontUi; font.pixelSize: Theme.fontSizeBody; font.weight: Theme.weightSemibold
                    }
                    Text {
                        text: session.userName + "@" + session.hostName + (session.liveSession ? "  ·  " + qsTr("live session") : "")
                        color: Theme.textMuted
                        font.family: Theme.fontUi; font.pixelSize: Theme.fontSizeCaption
                    }
                }
                Item { Layout.fillWidth: true }
                Text {
                    text: qsTr("%n application(s)", "", applications.count)
                    color: Theme.textMuted
                    font.family: Theme.fontUi; font.pixelSize: Theme.fontSizeSmall
                }
                NxIconButton {
                    iconName: "settings"
                    tooltip: qsTr("Control Center")
                    visible: session.programExists("shadow-control-center")
                    onClicked: { if (session.run("shadow-control-center")) root.hide() }
                }
                NxIconButton {
                    iconName: "power"
                    tooltip: qsTr("Lock, log out or power off")
                    onClicked: { root.hide(); root.sessionMenuRequested() }
                }
            }
        }

        // ---- the reveal: scan line, traced outline, brackets ---------------------
        FxBeam {
            id: beam
            anchors.fill: parent
            inset: card.radius
            direction: "down"
            from: column.y + search.y + search.height + Theme.spaceSm
            progress: root.open ? root.lin(root.beamStart, root.beamEnd) : 0
        }
        FxFrame {
            anchors.fill: parent
            radius: card.radius
            origin: "topLeft"
            progress: root.ph(0, 0.6)
            opacity: root.fade
            // Settles most of the way to the border colour, not all of it:
            // the traced outline stays faintly lit while the launcher is open.
            color: Theme.mix(Theme.accent, Theme.border, 0.6 * root.settle)
            glow: 1 - root.settle
        }
        FxBrackets {
            anchors.fill: parent
            anchors.margins: -Theme.spaceXs
            spread: 16 * (1 - root.ph(0.55, 0.9))
            // At rest they keep 60 % of their strength (85 % on a light
            // scheme, where the accent stands out less against the scrim).
            strength: root.ph(0.55, 0.9) * (1 - (Theme.dark ? 0.4 : 0.15) * root.settle) * root.fade
        }
    }
}
