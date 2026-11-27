// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Templates as T
import org.shadow.ui

T.ProgressBar {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    contentItem: Item {
        implicitWidth: 200
        implicitHeight: 6
        clip: true

        Rectangle {
            visible: !control.indeterminate
            width: control.visualPosition * parent.width
            height: parent.height
            radius: height / 2
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: Theme.accent }
                GradientStop { position: 1; color: Theme.mix(Theme.accent, Theme.accentViolet, 0.55) }
            }
            Behavior on width { NumberAnimation { duration: Theme.motionNormal; easing.type: Easing.OutCubic } }
        }
        // Busy without a known amount: a piece that travels along the bar.
        // With "Reduce motion" it rests in the middle instead, also when the
        // switch is turned on while it travels: the animation moves 'travel',
        // never x itself, so x falls back to the middle when it stops.
        Rectangle {
            id: runner
            property real travel: 0
            visible: control.indeterminate
            x: travelAnimation.running ? travel : (parent.width - width) / 2
            width: parent.width * 0.35
            height: parent.height
            radius: height / 2
            color: Theme.accent
            NumberAnimation {
                id: travelAnimation
                target: runner
                property: "travel"
                running: control.indeterminate && control.visible && !Appearance.reduceMotion
                loops: Animation.Infinite
                from: -runner.width; to: control.width
                duration: Theme.motionPage * 2 + Theme.motionNormal
                easing.type: Easing.InOutCubic
            }
        }
    }

    background: Rectangle {
        implicitWidth: 200
        implicitHeight: 6
        y: (control.height - height) / 2
        height: 6
        radius: 3
        color: Theme.bgPressed
    }
}
