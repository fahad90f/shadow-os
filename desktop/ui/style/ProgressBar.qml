// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Templates as T
import org.nexus.ui

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
        Rectangle {
            id: runner
            visible: control.indeterminate
            width: parent.width * 0.35
            height: parent.height
            radius: height / 2
            color: Theme.accent
            SequentialAnimation on x {
                running: control.indeterminate && control.visible
                loops: Animation.Infinite
                NumberAnimation { from: -runner.width; to: control.width; duration: 1100; easing.type: Easing.InOutCubic }
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
