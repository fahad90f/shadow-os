// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui

// A page whose body is one long virtualised list (updates, history, search
// results, installed packages). It looks like NxPage — title block, actions,
// centred width-limited column — but the title and any header content scroll
// away together with the rows, and only the visible rows exist as items.
//
//   PkListPage {
//       title: qsTr("History")
//       actions: [ NxButton { … } ]
//       NxBanner { … }                      // header content (default property)
//       model: backend.entries
//       delegate: MyRow { width: ListView.view.rowWidth }
//       sectionProperty: "group"
//       sectionDelegate: …
//       footer: NxEmptyState { visible: … }
//   }
Item {
    id: root

    default property alias headerContent: headerColumn.data
    property string title: ""
    property string subtitle: ""
    property alias actions: actionRow.data
    property alias model: list.model
    property alias delegate: list.delegate
    property alias footer: list.footer
    property alias count: list.count
    property alias view: list
    property string sectionProperty: ""
    property Component sectionDelegate: null
    property real maxContentWidth: 880
    property real rowSpacing: Theme.spaceSm

    readonly property real contentWidth: Math.max(200, Math.min(maxContentWidth, width - Theme.spaceXl * 2))
    readonly property real sideMargin: Math.max(Theme.spaceXl, (width - contentWidth) / 2)

    ListView {
        id: list

        // Width available to a row; delegates bind their width to it.
        readonly property real rowWidth: root.contentWidth

        anchors.fill: parent
        leftMargin: root.sideMargin
        rightMargin: root.sideMargin
        topMargin: Theme.spaceXl
        bottomMargin: Theme.spaceXl
        spacing: root.rowSpacing
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.VerticalFlick
        cacheBuffer: 600
        activeFocusOnTab: false
        ScrollBar.vertical: ScrollBar { }

        section.property: root.sectionProperty
        section.delegate: root.sectionDelegate

        // ListView keeps its rows where they are when the header above them
        // grows or the model is replaced, which would push the page title out
        // of view. Until the reader scrolls away, stay at the very top.
        property bool pinned: true
        function goToTop() { contentY = originY - topMargin }
        function stayAtTop() { if (pinned) Qt.callLater(list.goToTop) }
        onMovementEnded: pinned = atYBeginning
        onOriginYChanged: stayAtTop()
        onContentHeightChanged: stayAtTop()
        onCountChanged: stayAtTop()
        Connections {
            target: list.ScrollBar.vertical
            function onPressedChanged() { if (!list.ScrollBar.vertical.pressed) list.pinned = list.atYBeginning }
        }

        header: Item {
            id: headerHost
            width: root.contentWidth
            height: headerBox.implicitHeight + Theme.spaceLg
            onHeightChanged: list.stayAtTop()
            // The header block is declared once below (so 'actions' and the
            // header content can be plain aliases) and lives inside the list.
            Component.onCompleted: headerBox.parent = headerHost
        }
    }

    ColumnLayout {
        id: headerBox
        width: root.contentWidth
        spacing: Theme.spaceXl

        RowLayout {
            Layout.fillWidth: true
            visible: root.title !== "" || actionRow.children.length > 0
            spacing: Theme.spaceMd

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceXs
                Text {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: root.title
                    color: Theme.textPrimary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeHeading
                    font.weight: Theme.weightSemibold
                    elide: Text.ElideRight
                }
                Text {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    visible: root.subtitle !== ""
                    text: root.subtitle
                    color: Theme.textSecondary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBody
                    wrapMode: Text.WordWrap
                }
            }
            Row {
                id: actionRow
                spacing: Theme.spaceSm
                Layout.alignment: Qt.AlignTop
            }
        }

        ColumnLayout {
            id: headerColumn
            Layout.fillWidth: true
            spacing: Theme.spaceLg
            visible: children.length > 0
        }
    }
}
