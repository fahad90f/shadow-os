// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Window
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.files

// Preview and information about the selection of the active pane (or about
// the folder itself when nothing is selected): image preview, the first lines
// of text files, and the item's metadata.
Rectangle {
    id: panel

    required property var shell
    // The active FolderPane.
    property var pane: null

    readonly property var wantedUrls: {
        if (!pane || pane.folder.error !== "") return []
        if (pane.folder.selectedCount > 0) return pane.folder.selectedUrls
        return [pane.folder.url]
    }
    readonly property bool showsFolderItself: pane !== null && pane.folder.selectedCount === 0

    implicitWidth: Theme.sidebarWidth + Theme.spaceXxxl
    color: Theme.bgSurface

    // One labelled fact; hidden while it has no value.
    component Fact: ColumnLayout {
        id: fact
        property string label: ""
        property string value: ""
        property bool mono: false
        visible: value !== ""
        Layout.fillWidth: true
        spacing: 0
        Text {
            Layout.fillWidth: true
            text: fact.label
            color: Theme.textMuted
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeCaption
            font.weight: Theme.weightMedium
        }
        TextEdit {
            Layout.fillWidth: true
            text: fact.value
            readOnly: true
            selectByMouse: true
            wrapMode: TextEdit.WrapAnywhere
            color: Theme.textPrimary
            selectionColor: Theme.accentFillStrong
            selectedTextColor: Theme.textPrimary
            font.family: fact.mono ? Theme.fontMono : Theme.fontUi
            font.pixelSize: fact.mono ? Theme.fontSizeSmall : Theme.fontSizeBody
        }
    }

    // A rubber band changes the selection many times per second.
    onWantedUrlsChanged: settle.restart()
    Timer {
        id: settle
        interval: 120
        onTriggered: info.urls = panel.wantedUrls
    }

    FileProperties {
        id: info
        previews: true
        onUrlsChanged: computeSize = false
    }

    Rectangle {
        width: Theme.borderWidth
        height: parent.height
        color: Theme.borderSubtle
    }

    NxEmptyState {
        anchors.fill: parent
        visible: info.count === 0
        icon: "info"
        title: qsTr("Nothing to show")
        message: qsTr("Select a file or folder to see its details.")
    }

    Flickable {
        id: flick
        anchors.fill: parent
        anchors.leftMargin: Theme.borderWidth
        visible: info.count > 0
        contentHeight: content.implicitHeight + Theme.spaceLg * 2
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        ScrollBar.vertical: ScrollBar { }

        ColumnLayout {
            id: content
            x: Theme.spaceLg
            y: Theme.spaceLg
            width: flick.width - Theme.spaceLg * 2
            spacing: Theme.spaceMd

            // Preview
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: Math.min(width, Theme.sidebarWidth - Theme.spaceXl)
                radius: Theme.radiusMd
                color: Theme.bgRaised
                border.width: Theme.borderWidth
                border.color: Theme.borderSubtle

                NxAppIcon {
                    anchors.centerIn: parent
                    visible: info.imageSource === "" && info.iconName !== ""
                    name: info.iconName
                    size: Theme.iconXL * 3
                }
                Rectangle {
                    visible: preview.visible && preview.status === Image.Ready
                    anchors.centerIn: parent
                    width: preview.paintedWidth
                    height: preview.paintedHeight
                    color: Theme.alpha(Theme.textSecondary, 0.3)
                }
                Image {
                    id: preview
                    anchors.fill: parent
                    anchors.margins: Theme.spaceSm
                    visible: info.imageSource !== ""
                    source: info.imageSource
                    sourceSize: Qt.size(Math.round(width * Screen.devicePixelRatio), Math.round(height * Screen.devicePixelRatio))
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                    smooth: true
                }
            }

            Text {
                Layout.fillWidth: true
                text: info.name
                color: Theme.textPrimary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeTitle
                font.weight: Theme.weightSemibold
                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                maximumLineCount: 3
                elide: Text.ElideRight
            }
            Text {
                Layout.fillWidth: true
                Layout.topMargin: -Theme.spaceSm
                visible: text !== ""
                text: info.error !== "" ? info.error : info.typeName
                color: info.error !== "" ? Theme.danger : Theme.textSecondary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
                wrapMode: Text.WordWrap
            }

            // Text snippet
            Rectangle {
                Layout.fillWidth: true
                visible: info.textPreview !== ""
                implicitHeight: Math.min(snippet.implicitHeight + Theme.spaceSm * 2, Theme.sidebarWidth)
                radius: Theme.radiusSm
                color: Theme.bgInput
                border.width: Theme.borderWidth
                border.color: Theme.borderSubtle
                clip: true
                Accessible.role: Accessible.StaticText
                Accessible.name: qsTr("Beginning of the file")
                Text {
                    id: snippet
                    x: Theme.spaceSm
                    y: Theme.spaceSm
                    width: parent.width - Theme.spaceSm * 2
                    text: info.textPreview
                    textFormat: Text.PlainText
                    color: Theme.textSecondary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fontSizeCaption
                    wrapMode: Text.WrapAnywhere
                }
            }

            NxDivider { Layout.fillWidth: true }

            // Metadata
            Fact {
                label: qsTr("Size")
                value: info.count === 1 && !info.isDir && info.size >= 0 ? Theme.formatBytes(info.size) : ""
            }
            Fact { label: qsTr("Dimensions"); value: info.imageInfo }
            Fact {
                label: info.count > 1 ? qsTr("Total size") : qsTr("Contents")
                value: {
                    if (info.count === 1 && !info.isDir) return ""
                    if (info.sizeBusy) return qsTr("Calculating…")
                    if (info.sizeError !== "") return info.sizeError
                    return info.contentsText
                }
            }
            NxButton {
                visible: (info.isDir || info.count > 1) && !info.computeSize && !info.loading
                text: qsTr("Calculate size")
                iconName: "pie"
                focusPolicy: Qt.TabFocus
                onClicked: info.computeSize = true
            }
            Fact { label: qsTr("Location"); value: info.location }
            Fact { label: qsTr("Link target"); value: info.linkTarget; mono: true }
            Fact { label: qsTr("Modified"); value: info.modified }
            Fact { label: qsTr("Created"); value: info.created }
            Fact { label: qsTr("Accessed"); value: info.accessed }
            Fact {
                label: qsTr("Owner")
                value: info.owner === "" ? "" : (info.group === "" ? info.owner : info.owner + " : " + info.group)
            }
            Fact { label: qsTr("Permissions"); value: info.permissionsText; mono: true }

            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spaceXs
                spacing: Theme.spaceSm
                NxButton {
                    visible: !panel.showsFolderItself && info.count === 1
                    text: qsTr("Open")
                    kind: "primary"
                    focusPolicy: Qt.TabFocus
                    onClicked: if (panel.pane) panel.pane.openSelection()
                }
                NxButton {
                    text: qsTr("Properties")
                    focusPolicy: Qt.TabFocus
                    onClicked: if (panel.pane) panel.shell.showProperties(panel.pane)
                }
                Item { Layout.fillWidth: true }
            }
        }
    }
}
