// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui

// The notification centre: what has arrived, grouped by application, with
// the Do Not Disturb switch. Registered as the overlay "notifications", so
// the panel's bell and Meta+N open it.
PopupSurface {
    id: root

    required property var shell

    screen: shell.primaryScreen
    alignment: "right"
    popupWidth: 420
    scope: "shadow-notification-center"

    // For the "5 min" labels; only ticks while the centre is showing.
    property double now: Date.now()
    Timer {
        interval: 20000
        running: root.open
        repeat: true
        triggeredOnStart: true
        onTriggered: root.now = Date.now()
    }

    Component.onCompleted: shell.registerOverlay("notifications", root)
    // Tells the server: popups are withdrawn while the list is showing, and
    // everything counts as read once it is closed again.
    onOpenChanged: {
        notifications.centerOpen = open
        // The list gets its model with this change; no entry is selected
        // until the keyboard is used.
        if (open) Qt.callLater(() => { list.currentIndex = -1; list.positionViewAtBeginning() })
    }

    ColumnLayout {
        id: column
        width: parent.width
        spacing: Theme.spaceMd

        // ---- header -------------------------------------------------------------
        RowLayout {
            id: header
            Layout.fillWidth: true
            spacing: Theme.spaceSm
            Text {
                Layout.fillWidth: true
                text: qsTr("Notifications")
                color: Theme.textPrimary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeTitle
                font.weight: Theme.weightSemibold
                elide: Text.ElideRight
            }
            NxButton {
                kind: "ghost"
                text: qsTr("Clear all")
                visible: notifications.count > 0
                implicitHeight: 28
                font.pixelSize: Theme.fontSizeSmall
                onClicked: notifications.clearAll()
            }
        }

        // ---- Do Not Disturb ---------------------------------------------------------
        Rectangle {
            id: dndRow
            Layout.fillWidth: true
            implicitHeight: dndLayout.implicitHeight + Theme.spaceSm * 2
            radius: Theme.radiusMd
            color: Theme.bgRaised
            border.width: Theme.borderWidth
            border.color: Theme.borderSubtle
            RowLayout {
                id: dndLayout
                anchors.fill: parent
                anchors.leftMargin: Theme.spaceMd
                anchors.rightMargin: Theme.spaceSm
                anchors.topMargin: Theme.spaceSm
                anchors.bottomMargin: Theme.spaceSm
                spacing: Theme.spaceMd
                NxIcon {
                    name: notifications.doNotDisturb ? "bell-off" : "bell"
                    size: Theme.icon
                    color: notifications.doNotDisturb ? Theme.accent : Theme.textSecondary
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Do Not Disturb")
                        color: Theme.textPrimary
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeBody
                        font.weight: Theme.weightMedium
                        elide: Text.ElideRight
                    }
                    Text {
                        Layout.fillWidth: true
                        text: notifications.doNotDisturb
                            ? qsTr("Only critical notifications pop up; the rest waits here.")
                            : qsTr("Notifications pop up as they arrive.")
                        color: Theme.textMuted
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeCaption
                        wrapMode: Text.Wrap
                    }
                }
                Switch {
                    id: dndSwitch
                    Accessible.name: qsTr("Do Not Disturb")
                    checked: notifications.doNotDisturb
                    onToggled: notifications.doNotDisturb = checked
                    // The setting can change elsewhere (quick settings, the file).
                    Connections {
                        target: notifications
                        function onDoNotDisturbChanged() { dndSwitch.checked = notifications.doNotDisturb }
                    }
                }
            }
        }

        // ---- the service is not ours ----------------------------------------------------
        NxBanner {
            Layout.fillWidth: true
            visible: !notifications.available && notifications.unavailableReason !== "starting"
            kind: "warning"
            title: qsTr("Notifications are not available")
            message: notifications.unavailableReason === "other-service"
                ? qsTr("Another notification service is running in this session. Shadow Horizon shows notifications as soon as it has stopped.")
                : qsTr("There is no session message bus, so applications cannot send notifications.")
        }

        // ---- history ------------------------------------------------------------------
        ListView {
            id: list
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight,
                Math.max(160, root.maximumHeight - root.padding * 2 - header.height - dndRow.height
                              - column.spacing * 3))
            visible: notifications.count > 0
            clip: true
            spacing: Theme.spaceSm
            boundsBehavior: Flickable.StopAtBounds
            // No view work while the centre is closed, whatever arrives.
            model: root.open ? notifications.history : null
            focus: true
            activeFocusOnTab: true
            keyNavigationEnabled: true
            highlightFollowsCurrentItem: true
            highlightMoveDuration: 0
            currentIndex: -1
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Notifications")

            ScrollBar.vertical: ScrollBar { policy: list.contentHeight > list.height ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff }

            Keys.onDeletePressed: if (currentItem) notifications.dismiss(currentItem.notificationId)
            Keys.onReturnPressed: if (currentItem && currentItem.interactive) {
                notifications.activate(currentItem.notificationId, root); root.hide()
            }

            section.property: "appKey"
            section.delegate: Item {
                id: groupHeader
                required property string section
                // Re-read whenever the groups change.
                readonly property var info: { notifications.revision; return notifications.group(section) }
                width: ListView.view ? ListView.view.width - (list.ScrollBar.vertical.visible ? Theme.spaceMd : 0) : 0
                height: 32

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.spaceXs
                    spacing: Theme.spaceSm
                    Image {
                        visible: groupHeader.info.iconSource !== ""
                        source: groupHeader.info.iconSource
                        sourceSize: Qt.size(32, 32)
                        Layout.preferredWidth: Theme.iconSmall
                        Layout.preferredHeight: Theme.iconSmall
                        fillMode: Image.PreserveAspectFit
                        asynchronous: false
                        cache: false
                    }
                    NxIcon {
                        visible: groupHeader.info.iconSource === ""
                        name: "app-generic"
                        size: Theme.iconSmall
                        color: Theme.textMuted
                    }
                    Text {
                        Layout.fillWidth: true
                        text: groupHeader.info.name
                        textFormat: Text.PlainText
                        color: Theme.textSecondary
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeCaption
                        font.weight: Theme.weightSemibold
                        font.capitalization: Font.AllUppercase
                        font.letterSpacing: 0.8
                        elide: Text.ElideRight
                    }
                    Text {
                        visible: groupHeader.info.count > 1
                        text: groupHeader.info.count
                        color: Theme.textMuted
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeCaption
                    }
                    NxIconButton {
                        implicitWidth: 26
                        implicitHeight: 26
                        iconSize: Theme.iconSmall
                        iconName: "trash"
                        tooltip: qsTr("Dismiss all from %1").arg(groupHeader.info.name)
                        onClicked: notifications.dismissApp(groupHeader.section)
                    }
                }
            }

            delegate: NotificationCard {
                required property var model
                required property int index

                width: ListView.view ? ListView.view.width - (list.ScrollBar.vertical.visible ? Theme.spaceMd : 0) : 0
                popup: false
                bodyLines: 6
                now: root.now
                current: ListView.isCurrentItem && list.activeFocus
                notificationId: model.notificationId
                appName: model.appName
                summary: model.summary
                body: model.body
                bodyPlain: model.bodyPlain
                imageSource: model.imageSource
                iconSource: model.iconSource
                critical: model.critical
                unread: model.unread
                interactive: model.interactive
                actions: model.actions
                progress: model.progress
                timestamp: model.timestamp

                onActivated: { notifications.activate(notificationId, root); root.hide() }
                onActionInvoked: key => { notifications.invokeAction(notificationId, key, root); root.hide() }
                onLinkActivated: link => { notifications.openLink(notificationId, link); root.hide() }
                onDismissed: notifications.dismiss(notificationId)
            }
        }

        // ---- nothing there -----------------------------------------------------------------
        NxEmptyState {
            Layout.fillWidth: true
            visible: notifications.count === 0
            icon: notifications.doNotDisturb ? "bell-off" : "bell"
            title: qsTr("No notifications")
            message: !notifications.available ? ""
                : notifications.doNotDisturb ? qsTr("Do Not Disturb is on. What arrives is collected here.")
                : qsTr("What applications tell you appears here.")
        }
    }
}
