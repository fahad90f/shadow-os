// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui

// One tool in a list: name, interface, one-line summary and its real state on
// this system. Activating the row opens the tool's details.
Rectangle {
    id: root

    required property var tool
    property bool showCategory: false
    property bool first: false

    signal opened(string packageName)
    signal installRequested(string packageName)

    Layout.fillWidth: true
    implicitHeight: Math.max(Theme.rowHeightLarge, content.implicitHeight + Theme.spaceMd * 2)
    color: hover.hovered || activeFocus ? Theme.bgHover : "transparent"
    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: qsTr("%1, %2").arg(tool.name).arg(stateText)
    Accessible.onPressAction: root.opened(tool.package)
    Keys.onReturnPressed: root.opened(tool.package)
    Keys.onSpacePressed: root.opened(tool.package)

    readonly property string stateText: tool.state === "installed" ? qsTr("installed, version %1").arg(tool.installedVersion)
        : tool.state === "available" ? qsTr("not installed")
        : tool.state === "unsupported" ? qsTr("not built for %1").arg(arsenal.architecture)
        : tool.state === "unavailable" ? qsTr("not in the configured repositories")
        : qsTr("state unknown")

    HoverHandler { id: hover }
    TapHandler { onTapped: root.opened(root.tool.package) }

    Rectangle {
        visible: !root.first
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: Theme.spaceLg
        height: Theme.borderWidth
        color: Theme.borderSubtle
    }

    RowLayout {
        id: content
        anchors.fill: parent
        anchors.leftMargin: Theme.spaceLg
        anchors.rightMargin: Theme.spaceMd
        spacing: Theme.spaceMd

        NxIcon {
            Layout.alignment: Qt.AlignVCenter
            name: root.tool.state === "installed" ? "check-circle" : root.tool.categoryIcon
            size: Theme.icon
            color: root.tool.state === "installed" ? Theme.success : Theme.textMuted
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            spacing: Theme.spaceXxs

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceSm
                Text {
                    text: root.tool.name
                    color: Theme.textPrimary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBody
                    font.weight: Theme.weightMedium
                    elide: Text.ElideRight
                    Layout.maximumWidth: content.width * 0.4
                }
                Text {
                    text: root.tool.package
                    visible: root.tool.package.toLowerCase() !== root.tool.name.toLowerCase()
                    color: Theme.textMuted
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fontSizeCaption
                    elide: Text.ElideRight
                    Layout.maximumWidth: content.width * 0.25
                }
                NxBadge {
                    visible: root.tool.interface !== "cli"
                    text: root.tool.interface === "both" ? qsTr("GUI + CLI") : qsTr("GUI")
                    kind: "accent"
                    dot: false
                }
                Item { Layout.fillWidth: true }
            }
            Text {
                Layout.fillWidth: true
                text: root.showCategory ? qsTr("%1 · %2").arg(root.tool.categoryName).arg(root.tool.summary) : root.tool.summary
                color: Theme.textSecondary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                elide: Text.ElideRight
            }
        }

        Text {
            Layout.alignment: Qt.AlignVCenter
            visible: root.tool.state === "installed"
            text: root.tool.installedVersion
            color: Theme.textMuted
            font.family: Theme.fontMono
            font.pixelSize: Theme.fontSizeSmall
            elide: Text.ElideRight
            Layout.maximumWidth: 150
        }
        NxBadge {
            Layout.alignment: Qt.AlignVCenter
            visible: root.tool.state === "unsupported" || root.tool.state === "unavailable" || root.tool.state === "unknown"
            text: root.tool.state === "unsupported" ? qsTr("Not for %1").arg(arsenal.architecture)
                : root.tool.state === "unavailable" ? qsTr("Not in repositories") : qsTr("Unknown")
            kind: root.tool.state === "unknown" ? "neutral" : "warning"
        }
        NxButton {
            Layout.alignment: Qt.AlignVCenter
            visible: root.tool.state === "available"
            text: qsTr("Install")
            iconName: "download"
            Accessible.name: qsTr("Install %1").arg(root.tool.name)
            onClicked: root.installRequested(root.tool.package)
        }
        NxIcon {
            Layout.alignment: Qt.AlignVCenter
            name: "chevron-right"
            size: Theme.iconSmall
            color: Theme.textMuted
        }
    }
}
