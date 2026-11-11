// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui

// One of several mutually exclusive choices, shown as a card with a radio
// mark, a title, an explanation and optional extra content underneath.
// Space/Return choose; a disabled card says why in 'problem'.
Rectangle {
    id: root

    default property alias content: extra.data
    property string icon: ""
    property string title: ""
    property string description: ""
    property string problem: ""             // non-empty: cannot be chosen, and why
    property bool selected: false
    property bool danger: false
    property alias badges: badgeRow.data

    signal chosen()

    readonly property bool available: problem === ""

    Layout.fillWidth: true
    implicitHeight: body.implicitHeight + Theme.spaceMd * 2
    radius: Theme.radiusMd
    color: selected ? Theme.alpha(danger ? Theme.danger : Theme.accent, 0.08) : hover.hovered && available ? Theme.bgHover : Theme.bgRaised
    border.width: activeFocus ? Theme.focusRing : Theme.borderWidth
    border.color: activeFocus ? Theme.accent : selected ? Theme.alpha(danger ? Theme.danger : Theme.accent, 0.6) : Theme.borderSubtle
    activeFocusOnTab: available
    Accessible.role: Accessible.RadioButton
    Accessible.name: title
    Accessible.description: available ? description : problem
    Accessible.checked: selected
    Keys.onSpacePressed: if (available) chosen()
    Keys.onReturnPressed: if (available) chosen()

    HoverHandler { id: hover; cursorShape: root.available ? Qt.PointingHandCursor : Qt.ArrowCursor }
    TapHandler { enabled: root.available; onTapped: { root.forceActiveFocus(); root.chosen() } }

    RowLayout {
        id: body
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Theme.spaceMd
        spacing: Theme.spaceMd

        Rectangle {
            Layout.alignment: Qt.AlignTop
            Layout.topMargin: 2
            implicitWidth: 18
            implicitHeight: 18
            radius: 9
            color: "transparent"
            border.width: 2
            border.color: root.selected ? (root.danger ? Theme.danger : Theme.accent) : Theme.borderStrong
            opacity: root.available ? 1 : Theme.opacityDisabled
            Rectangle {
                anchors.centerIn: parent
                width: 8; height: 8; radius: 4
                visible: root.selected
                color: root.danger ? Theme.danger : Theme.accent
            }
        }
        NxIcon {
            Layout.alignment: Qt.AlignTop
            visible: root.icon !== ""
            name: root.icon
            size: Theme.icon
            color: root.selected ? (root.danger ? Theme.danger : Theme.accent) : Theme.textSecondary
            opacity: root.available ? 1 : Theme.opacityDisabled
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceXs
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceSm
                Text {
                    Layout.fillWidth: true
                    text: root.title
                    color: root.available ? Theme.textPrimary : Theme.textMuted
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBody
                    font.weight: Theme.weightSemibold
                    elide: Text.ElideRight
                }
                Row { id: badgeRow; spacing: Theme.spaceXs }
            }
            Text {
                Layout.fillWidth: true
                visible: text !== ""
                text: root.available ? root.description : root.problem
                color: Theme.textSecondary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WordWrap
            }
            ColumnLayout {
                id: extra
                Layout.fillWidth: true
                spacing: Theme.spaceXs
                visible: children.length > 0
            }
        }
    }
}
