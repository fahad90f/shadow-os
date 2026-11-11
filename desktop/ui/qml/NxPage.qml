// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Scrollable content page with a title block and a centred, width-limited
// content column — the layout used by every settings-style screen.
//
// The header (title, subtitle and the 'actions') stays in place at the top;
// only the content column below it scrolls, so an action such as "Apply" is
// reachable however long the page is. Without a title and without actions
// there is no header and the whole page scrolls.
Item {
    id: root

    default property alias content: column.data
    property string title: ""
    property string subtitle: ""
    property alias actions: actionRow.data
    property real maxContentWidth: 880
    property real contentSpacing: Theme.spaceXl
    property alias flickable: flick

    readonly property bool hasHeader: title !== "" || actionRow.visibleChildren.length > 0
    // Left edge and width shared by the header and the content column.
    readonly property real bodyWidth: Math.max(0, Math.min(maxContentWidth, width - Theme.spaceXl * 2))
    readonly property real bodyX: Math.max(Theme.spaceXl, (width - bodyWidth) / 2)
    // Space kept free under the header, so scrolled content does not touch the
    // subtitle; the content column starts that much higher inside the
    // scrolling area, which leaves the unscrolled page exactly as it was.
    readonly property real headerGap: hasHeader ? Theme.spaceMd : 0

    // ---- fixed header ---------------------------------------------------------
    Item {
        id: header
        visible: root.hasHeader
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: visible ? headerRow.implicitHeight + Theme.spaceXl + root.headerGap : 0
        z: 1

        RowLayout {
            id: headerRow
            x: root.bodyX
            y: Theme.spaceXl
            width: root.bodyWidth
            spacing: Theme.spaceMd

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceXs
                Text {
                    Layout.fillWidth: true
                    text: root.title
                    color: Theme.textPrimary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeHeading
                    font.weight: Theme.weightSemibold
                    elide: Text.ElideRight
                }
                Text {
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

        // The wheel also scrolls the page while the pointer is over the header.
        WheelHandler {
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            onWheel: (event) => {
                const max = Math.max(0, flick.contentHeight - flick.height)
                const step = event.pixelDelta.y !== 0 ? event.pixelDelta.y : event.angleDelta.y / 120 * 60
                flick.contentY = Math.max(0, Math.min(max, flick.contentY - step))
            }
        }
    }

    Flickable {
        id: flick
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        contentWidth: width
        contentHeight: column.implicitHeight + Theme.spaceXl * 2 - root.headerGap
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        ScrollBar.vertical: ScrollBar { }

        ColumnLayout {
            id: column
            x: root.bodyX
            y: Theme.spaceXl - root.headerGap
            width: root.bodyWidth
            spacing: root.contentSpacing
        }
    }

    // Hairline under the header once content has scrolled beneath it.
    Rectangle {
        visible: root.hasHeader
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        height: 1
        z: 1
        color: Theme.borderSubtle
        opacity: flick.contentY > 0 ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: Theme.motionFast } }
    }
}
