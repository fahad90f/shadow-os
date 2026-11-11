// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.store

// One application in a list (category, search results, installed, recently
// released). The whole row opens the application's page; an optional action
// button sits at the end.
Rectangle {
    id: row

    required property string appId
    required property string name
    required property string summary
    required property string icon
    required property bool iconRemote
    required property string packageName
    required property bool installed
    required property string kind
    required property string releaseDate

    property bool showReleaseDate: false
    // Text of an extra button at the end of the row ("" = none), e.g. "Remove".
    property string actionText: ""
    property bool actionEnabled: true
    property string actionKind: "secondary"

    signal clicked()
    signal actionTriggered()

    implicitHeight: 72
    radius: Theme.radiusMd
    color: tap.pressed ? Theme.bgPressed : hover.hovered || activeFocus ? Theme.bgHover : Theme.bgRaised
    border.width: Theme.borderWidth
    border.color: activeFocus ? Theme.accent : Theme.borderSubtle
    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: name
    Accessible.description: summary
    Keys.onReturnPressed: clicked()
    Keys.onSpacePressed: clicked()
    Behavior on color { ColorAnimation { duration: Theme.motionFast } }

    HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
    TapHandler { id: tap; onTapped: row.clicked() }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.spaceLg
        anchors.rightMargin: Theme.spaceLg
        spacing: Theme.spaceLg

        AppIcon {
            source: row.icon
            remote: row.iconRemote
            size: 44
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            PkText {
                Layout.fillWidth: true
                text: row.name
                font.pixelSize: Theme.fontSizeBodyLarge
                font.weight: Theme.weightSemibold
                elide: Text.ElideRight
            }
            PkText {
                Layout.fillWidth: true
                text: row.summary
                color: Theme.textSecondary
                elide: Text.ElideRight
            }
        }
        PkText {
            visible: row.showReleaseDate && row.releaseDate !== ""
            text: row.releaseDate
            color: Theme.textMuted
            font.pixelSize: Theme.fontSizeSmall
        }
        NxBadge {
            visible: row.kind === "console-application"
            text: qsTr("Terminal")
            dot: false
        }
        NxBadge {
            visible: row.installed
            text: qsTr("Installed")
            kind: "success"
        }
        NxButton {
            visible: row.actionText !== ""
            text: row.actionText
            kind: row.actionKind
            enabled: row.actionEnabled
            onClicked: row.actionTriggered()
        }
        NxIcon {
            name: "chevron-right"
            size: Theme.iconSmall
            color: Theme.textMuted
        }
    }
}
