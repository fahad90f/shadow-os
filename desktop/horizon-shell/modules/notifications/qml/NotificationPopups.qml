// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.kde.layershell as LayerShell
import org.shadow.ui

// Notification popups: a short stack in the top right corner, under the
// panel. The surface is exactly as large as the stack and never takes the
// keyboard, so it neither steals focus nor swallows input elsewhere.
//
// A new card fades in while what it shows comes in from the right and a bar
// grows down its left edge (Theme.motionNormal). When a card goes, it is
// gone at once and the ones below move up (Theme.motionFast).
ShellSurface {
    id: root

    required property var shell

    readonly property int margin: Theme.spaceSm
    readonly property int cardWidth: 380

    screen: shell.primaryScreen
    layer: "overlay"
    scope: "shadow-notifications"
    anchorTop: true
    anchorRight: true
    keyboard: "none"
    // Under the panel also when the panel reserves no space for itself (it
    // does only when it never hides): the surface is placed lower, so it
    // never lies over the panel's buttons.
    LayerShell.Window.margins.top: shell.panel && shell.panel.exclusiveZone > 0 ? 0 : Theme.panelHeight
    width: Math.min(cardWidth, (screen ? screen.width : 1280) - margin * 2) + margin * 2
    // As high as the stack, and while cards move up after one above them
    // went, as high as the lowest card still is: the surface does not
    // shrink under a card before the card is there.
    height: Math.max(1, Math.max(stack.height, stack.childrenRect.y + stack.childrenRect.height) + margin * 2)
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

        move: Transition {
            NumberAnimation {
                properties: "y"
                duration: Theme.motionFast
                easing.type: Easing.BezierSpline
                easing.bezierCurve: Theme.easingDecelerate
            }
        }

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
                NumberAnimation on enter { from: 0; to: 1; duration: Theme.motionNormal }
            }
        }
    }
}
