// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.store

// One package (a row of NexusPk::PackageModel) in "All packages" lists.
Rectangle {
    id: row

    required property string packageId
    required property string name
    required property string version
    required property string arch
    required property string repo
    required property string summary
    required property bool installed
    required property bool automatic
    required property string installedVersion

    property string actionText: ""
    property bool actionEnabled: true
    property string actionKind: "secondary"

    signal clicked()
    signal actionTriggered()

    implicitHeight: 60
    radius: Theme.radiusMd
    color: tap.pressed ? Theme.bgPressed : hover.hovered || activeFocus ? Theme.bgHover : Theme.bgRaised
    border.width: Theme.borderWidth
    border.color: activeFocus ? Theme.accent : Theme.borderSubtle
    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: name + " " + version
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
        spacing: Theme.spaceMd

        NxIcon {
            name: "package"
            size: Theme.icon
            color: row.installed ? Theme.success : Theme.textSecondary
        }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 2
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceSm
                PkText {
                    text: row.name
                    font.weight: Theme.weightSemibold
                    elide: Text.ElideRight
                    Layout.maximumWidth: row.width * 0.45
                }
                PkText {
                    Layout.fillWidth: true
                    text: row.version + "  ·  " + row.arch + (row.repo !== "" ? "  ·  " + row.repo : "")
                    color: Theme.textMuted
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fontSizeSmall
                    elide: Text.ElideRight
                }
            }
            PkText {
                Layout.fillWidth: true
                visible: row.summary !== ""
                text: row.summary
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSizeSmall
                elide: Text.ElideRight
            }
        }
        NxBadge {
            visible: row.installed && row.installedVersion !== "" && row.installedVersion !== row.version
            text: qsTr("Update")
            kind: "warning"
        }
        NxBadge {
            visible: row.installed && row.automatic
            text: qsTr("Dependency")
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
