// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.shadow.ui

// The bell in the panel: opens the notification centre and counts what has
// not been looked at yet.
PanelButton {
    id: root

    required property var shell
    required property var panel

    readonly property bool centerRegistered: shell.overlayNames.indexOf("notifications") >= 0

    icon: notifications.doNotDisturb ? "bell-off" : "bell"
    iconColor: notifications.available ? Theme.textPrimary : Theme.textMuted
    checked: shell.openOverlayName === "notifications"
    badge: checked ? 0 : notifications.unreadCount
    interactive: centerRegistered
    tooltip: !notifications.available ? qsTr("Notifications (service not available)")
        : notifications.doNotDisturb ? qsTr("Notifications (Do Not Disturb is on)")
        : notifications.unreadCount > 0 ? qsTr("Notifications (%n new)", "", notifications.unreadCount)
        : qsTr("Notifications")
    onClicked: shell.trigger("notifications")
}
