// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.shadow.ui

// The scan line of the desktop's start sequence: a 2 px line in the accent
// colour with a fading tail above it, at progress (0 top, 1 bottom) of this
// item's height. What lies above the line has been resolved. With software
// rendering it is the line alone: a translucent band across the screen is
// the expensive part there.
Item {
    id: scan

    property real progress: 0

    visible: progress > 0 && progress < 1

    Item {
        y: Math.round(scan.progress * scan.height)
        width: scan.width

        Rectangle {
            visible: !Appearance.softwareRendering
            y: -height
            width: parent.width
            height: 36
            gradient: Gradient {
                GradientStop { position: 0; color: Theme.alpha(Theme.accent, 0) }
                GradientStop { position: 1; color: Theme.alpha(Theme.accent, 0.3) }
            }
        }
        Rectangle {
            width: parent.width
            height: 2
            color: Theme.accent
        }
    }
}
