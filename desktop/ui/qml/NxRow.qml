// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts

// One line inside an NxSection: icon, title, optional subtitle, and trailing
// controls (any children). Set 'chevron' for rows that navigate somewhere.
Item {
    id: root

    default property alias trailing: trailingRow.data
    property string icon: ""
    property color iconColor: Theme.textSecondary
    property string title: ""
    property string subtitle: ""
    property string value: ""
    property bool chevron: false
    property bool clickable: chevron
    property bool monoValue: false

    signal clicked()

    readonly property bool isFirst: parent && parent.children.length > 0 && parent.children[0] === root

    Layout.fillWidth: true
    implicitHeight: Math.max(subtitle !== "" ? Theme.rowHeightLarge : Theme.rowHeight,
                             labels.implicitHeight + Theme.spaceMd * 2)
    Accessible.role: clickable ? Accessible.Button : Accessible.StaticText
    Accessible.name: title
    Accessible.description: subtitle
    activeFocusOnTab: clickable

    Rectangle {
        anchors.fill: parent
        visible: root.clickable
        color: tap.pressed ? Theme.pressedOverlay : hover.hovered || root.activeFocus ? Theme.hoverOverlay : "transparent"
        Behavior on color { ColorAnimation { duration: Theme.motionFast } }
    }

    Rectangle {
        visible: !root.isFirst
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: Theme.spaceLg
        height: 1
        color: Theme.borderSubtle
    }

    HoverHandler { id: hover; enabled: root.clickable; cursorShape: root.clickable ? Qt.PointingHandCursor : Qt.ArrowCursor }
    TapHandler { id: tap; enabled: root.clickable; onTapped: root.clicked() }
    Keys.onReturnPressed: if (clickable) clicked()
    Keys.onSpacePressed: if (clickable) clicked()

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.spaceLg
        anchors.rightMargin: Theme.spaceLg
        spacing: Theme.spaceMd

        NxIcon {
            visible: root.icon !== ""
            name: root.icon
            size: Theme.icon
            color: root.iconColor
        }

        ColumnLayout {
            id: labels
            Layout.fillWidth: true
            spacing: 2
            Text {
                Layout.fillWidth: true
                text: root.title
                color: root.enabled ? Theme.textPrimary : Theme.textDisabled
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
                font.weight: Theme.weightMedium
                elide: Text.ElideRight
            }
            Text {
                Layout.fillWidth: true
                visible: root.subtitle !== ""
                text: root.subtitle
                color: Theme.textMuted
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WordWrap
                maximumLineCount: 3
                elide: Text.ElideRight
            }
        }

        Text {
            visible: root.value !== ""
            text: root.value
            color: Theme.textSecondary
            font.family: root.monoValue ? Theme.fontMono : Theme.fontUi
            font.pixelSize: Theme.fontSizeBody
            elide: Text.ElideMiddle
            horizontalAlignment: Text.AlignRight
            Layout.maximumWidth: root.width * 0.55
        }

        Row {
            id: trailingRow
            spacing: Theme.spaceSm
            Layout.alignment: Qt.AlignVCenter
        }

        NxIcon {
            visible: root.chevron
            name: "chevron-right"
            size: Theme.iconSmall
            color: Theme.textMuted
        }
    }
}
