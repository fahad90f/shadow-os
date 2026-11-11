// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.nexus.ui

// Notification popups: a short stack in the top right corner, under the
// panel. The surface is exactly as large as the stack and never takes the
// keyboard, so it neither steals focus nor swallows input elsewhere.
ShellSurface {
    id: root

    required property var shell

    readonly property int margin: Theme.spaceSm
    readonly property int cardWidth: 380

    screen: shell.primaryScreen
    layer: "overlay"
    scope: "nexus-notifications"
    anchorTop: true
    anchorRight: true
    keyboard: "none"
    width: Math.min(cardWidth, (screen ? screen.width : 1280) - margin * 2) + margin * 2
    height: Math.max(1, stack.height + margin * 2)
    visible: notifications.popups.count > 0

    // While the pointer is over the stack, nothing times out.
    HoverHandler { id: stackHover }
    Binding {
        target: notifications
        property: "popupsHovered"
        value: stackHover.hovered && root.visible
    }

    Column {
        id: stack
        x: root.margin
        y: root.margin
        width: root.width - root.margin * 2
        spacing: Theme.spaceSm

        Repeater {
            model: notifications.popups

            NotificationCard {
                id: card
                required property var model

                width: stack.width
                popup: true
                notificationId: model.notificationId
                appName: model.appName
                summary: model.summary
                body: model.body
                bodyPlain: model.bodyPlain
                imageSource: model.imageSource
                iconSource: model.iconSource
                critical: model.critical
                interactive: model.interactive
                actions: model.actions
                progress: model.progress

                onActivated: notifications.activate(notificationId, root)
                onActionInvoked: key => notifications.invokeAction(notificationId, key, root)
                onLinkActivated: link => notifications.openLink(notificationId, link)
                onDismissed: notifications.dismiss(notificationId)

                NumberAnimation on opacity { from: 0; to: 1; duration: Theme.motionNormal }
            }
        }
    }
}
