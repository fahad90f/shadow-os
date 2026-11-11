// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.files

// Places sidebar: standard folders, bookmarks, storage devices (Solid) and
// network locations. Every entry is a drop target and has a context menu.
Rectangle {
    id: sidebar

    required property var shell
    // Folder of the active pane; the entry showing it is highlighted.
    property url currentUrl
    // Device the user clicked while it was unmounted: opened once mounted.
    property string pendingDevice: ""

    implicitWidth: Theme.sidebarWidth
    color: Theme.bgSurface

    Connections {
        target: Devices
        function onMounted(udi, url) {
            if (udi === sidebar.pendingDevice) {
                sidebar.pendingDevice = ""
                sidebar.shell.navigate(url)
            }
        }
    }

    component SectionLabel: Text {
        width: parent ? parent.width : 0
        topPadding: Theme.spaceMd
        bottomPadding: Theme.spaceXs
        leftPadding: Theme.spaceSm
        color: Theme.textMuted
        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeCaption
        font.weight: Theme.weightSemibold
        font.capitalization: Font.AllUppercase
        font.letterSpacing: 0.8
    }

    // One sidebar entry. `trailing` holds extra controls (eject button).
    component Entry: Rectangle {
        id: entry
        default property alias trailing: trailingRow.data
        property string title: ""
        property string icon: ""
        property url url
        property bool selected: false
        property bool busy: false
        // 0..1, shown as a thin capacity bar when >= 0.
        property real fill: -1
        property string hint: ""

        signal clicked()
        signal middleClicked()
        signal menuRequested()
        signal dropped(var drop)

        width: parent ? parent.width : 0
        height: fill >= 0 ? Theme.rowHeight : Theme.controlHeight + 2
        radius: Theme.radiusSm
        color: selected ? Theme.accentFill
            : entryDrop.containsDrag ? Theme.accentFill
            : entryMouse.pressed ? Theme.pressedOverlay
            : entryMouse.containsMouse || activeFocus ? Theme.hoverOverlay : "transparent"
        border.width: entryDrop.containsDrag ? Theme.borderWidth : 0
        border.color: Theme.accent
        Behavior on color { ColorAnimation { duration: Theme.motionFast } }
        activeFocusOnTab: true
        Accessible.role: Accessible.ListItem
        Accessible.name: title
        Accessible.description: hint
        Keys.onReturnPressed: clicked()
        Keys.onSpacePressed: clicked()
        Keys.onMenuPressed: menuRequested()

        Rectangle {
            visible: entry.selected
            anchors.verticalCenter: parent.verticalCenter
            width: 3
            height: Theme.iconSmall
            radius: 1.5
            color: Theme.accent
        }

        MouseArea {
            id: entryMouse
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
            cursorShape: Qt.PointingHandCursor
            onClicked: (mouse) => {
                if (mouse.button === Qt.RightButton) entry.menuRequested()
                else if (mouse.button === Qt.MiddleButton) entry.middleClicked()
                else entry.clicked()
            }
        }
        DropArea {
            id: entryDrop
            anchors.fill: parent
            enabled: entry.url.toString() !== ""
            keys: ["text/uri-list"]
            onEntered: (drag) => { drag.accepted = FileOps.dropAction(drag.urls, entry.url) !== "" }
            onDropped: (drop) => entry.dropped(drop)
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.spaceMd
            anchors.rightMargin: Theme.spaceXs
            spacing: Theme.spaceMd

            NxIcon {
                visible: !entry.busy
                name: entry.icon
                size: Theme.icon - 2
                color: entry.selected ? Theme.accent : Theme.textSecondary
            }
            BusyIndicator {
                visible: entry.busy
                running: entry.busy
                implicitWidth: Theme.icon - 2
                implicitHeight: Theme.icon - 2
                padding: 0
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceXs
                Text {
                    Layout.fillWidth: true
                    text: entry.title
                    color: entry.selected ? Theme.textPrimary : Theme.textSecondary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBody
                    font.weight: entry.selected ? Theme.weightSemibold : Theme.weightMedium
                    elide: Text.ElideRight
                }
                Rectangle {
                    Layout.fillWidth: true
                    visible: entry.fill >= 0
                    implicitHeight: Theme.spaceXs
                    radius: height / 2
                    color: Theme.bgPressed
                    Rectangle {
                        width: parent.width * Math.max(0, Math.min(1, entry.fill))
                        height: parent.height
                        radius: parent.radius
                        color: entry.fill >= 0.92 ? Theme.danger : entry.fill >= 0.8 ? Theme.warning : Theme.accent
                    }
                }
            }
            Row {
                id: trailingRow
                Layout.alignment: Qt.AlignVCenter
                spacing: 0
            }
        }

        ToolTip.visible: hint !== "" && entryMouse.containsMouse && !entryMouse.pressed
        ToolTip.text: hint
    }

    Rectangle {
        anchors.right: parent.right
        width: Theme.borderWidth
        height: parent.height
        color: Theme.borderSubtle
    }

    Flickable {
        id: flick
        anchors.fill: parent
        anchors.margins: Theme.spaceSm
        anchors.rightMargin: Theme.spaceSm + Theme.borderWidth
        contentHeight: column.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        ScrollBar.vertical: ScrollBar { }

        Column {
            id: column
            width: flick.width
            spacing: 2

            SectionLabel {
                text: qsTr("Places")
                topPadding: Theme.spaceXs
            }
            Repeater {
                model: Places.places
                Entry {
                    required property var modelData
                    title: modelData.title
                    icon: modelData.icon
                    url: modelData.url
                    selected: FileOps.sameLocation(sidebar.currentUrl, modelData.url)
                    onClicked: sidebar.shell.navigate(modelData.url)
                    onMiddleClicked: sidebar.shell.openInNewTab(modelData.url, false)
                    onMenuRequested: sidebar.shell.showPlaceMenu(modelData)
                    onDropped: (drop) => sidebar.shell.handleDrop(drop, modelData.url)
                }
            }

            SectionLabel {
                visible: Places.bookmarks.length > 0
                text: qsTr("Bookmarks")
            }
            Repeater {
                model: Places.bookmarks
                Entry {
                    required property var modelData
                    title: modelData.title
                    icon: modelData.icon
                    url: modelData.url
                    hint: FileOps.displayPath(modelData.url)
                    selected: FileOps.sameLocation(sidebar.currentUrl, modelData.url)
                    onClicked: sidebar.shell.navigate(modelData.url)
                    onMiddleClicked: sidebar.shell.openInNewTab(modelData.url, false)
                    onMenuRequested: sidebar.shell.showPlaceMenu(modelData)
                    onDropped: (drop) => sidebar.shell.handleDrop(drop, modelData.url)
                }
            }

            SectionLabel { text: qsTr("Devices") }
            Repeater {
                model: Devices.devices
                Entry {
                    id: device
                    required property var modelData
                    readonly property bool mounted: modelData.mounted
                    title: modelData.label
                    icon: modelData.icon
                    url: modelData.url
                    busy: modelData.busy
                    fill: mounted && modelData.total > 0 && modelData.free >= 0 ? 1 - modelData.free / modelData.total : -1
                    hint: mounted && modelData.free >= 0
                        ? qsTr("%1 free of %2 · %3").arg(Theme.formatBytes(modelData.free)).arg(Theme.formatBytes(modelData.total)).arg(modelData.path)
                        : modelData.total > 0 ? qsTr("%1 · not mounted").arg(Theme.formatBytes(modelData.total)) : qsTr("Not mounted")
                    selected: mounted && FileOps.sameLocation(sidebar.currentUrl, modelData.url)
                    onClicked: {
                        if (mounted) {
                            sidebar.shell.navigate(modelData.url)
                        } else {
                            sidebar.pendingDevice = modelData.udi
                            Devices.mount(modelData.udi)
                        }
                    }
                    onMiddleClicked: if (mounted) sidebar.shell.openInNewTab(modelData.url, false)
                    onMenuRequested: sidebar.shell.showDeviceMenu(modelData)
                    onDropped: (drop) => sidebar.shell.handleDrop(drop, modelData.url)

                    NxIconButton {
                        visible: device.mounted && device.modelData.path !== "/" && !device.modelData.busy
                        anchors.verticalCenter: parent.verticalCenter
                        iconName: "eject"
                        iconSize: Theme.iconSmall
                        tooltip: device.modelData.removable ? qsTr("Unmount and safely remove") : qsTr("Unmount")
                        focusPolicy: Qt.TabFocus
                        onClicked: Devices.unmount(device.modelData.udi)
                    }
                    NxIconButton {
                        visible: !device.mounted && device.modelData.optical && !device.modelData.busy
                        anchors.verticalCenter: parent.verticalCenter
                        iconName: "eject"
                        iconSize: Theme.iconSmall
                        tooltip: qsTr("Eject the disc")
                        focusPolicy: Qt.TabFocus
                        onClicked: Devices.eject(device.modelData.udi)
                    }
                }
            }
            // Honest empty state: say why there is nothing to show.
            Item {
                visible: Devices.devices.length === 0
                width: column.width
                height: emptyDevices.implicitHeight + Theme.spaceSm
                RowLayout {
                    id: emptyDevices
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: Theme.spaceMd
                    anchors.rightMargin: Theme.spaceSm
                    spacing: Theme.spaceMd
                    NxIcon {
                        Layout.alignment: Qt.AlignTop
                        name: "info"
                        size: Theme.iconSmall
                        color: Theme.textMuted
                    }
                    Text {
                        Layout.fillWidth: true
                        text: Devices.emptyReason
                        color: Theme.textMuted
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeSmall
                        wrapMode: Text.WordWrap
                    }
                }
            }

            SectionLabel { text: qsTr("Network") }
            Repeater {
                model: Places.servers
                Entry {
                    required property var modelData
                    title: modelData.title
                    icon: modelData.icon
                    url: modelData.url
                    hint: FileOps.displayPath(modelData.url)
                    selected: FileOps.sameLocation(sidebar.currentUrl, modelData.url)
                    onClicked: sidebar.shell.navigate(modelData.url)
                    onMiddleClicked: sidebar.shell.openInNewTab(modelData.url, false)
                    onMenuRequested: sidebar.shell.showPlaceMenu(modelData)
                    onDropped: (drop) => sidebar.shell.handleDrop(drop, modelData.url)
                }
            }
            Entry {
                visible: Launcher.networkAvailable
                title: qsTr("Connect to server…")
                icon: "globe"
                onClicked: sidebar.shell.connectToServer()
            }
            // Without any KIO network worker there is nothing to connect with.
            Item {
                visible: !Launcher.networkAvailable
                width: column.width
                height: noNetwork.implicitHeight + Theme.spaceSm
                RowLayout {
                    id: noNetwork
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: Theme.spaceMd
                    anchors.rightMargin: Theme.spaceSm
                    spacing: Theme.spaceMd
                    NxIcon {
                        Layout.alignment: Qt.AlignTop
                        name: "info"
                        size: Theme.iconSmall
                        color: Theme.textMuted
                    }
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("No network protocol support is installed (packages “kio6” and “kio-extras”).")
                        color: Theme.textMuted
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeSmall
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }
    }
}
