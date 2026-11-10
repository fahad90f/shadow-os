// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Templates as T
import org.nexus.ui

T.Dialog {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding,
                            implicitHeaderWidth, implicitFooterWidth)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding
                             + (implicitHeaderHeight > 0 ? implicitHeaderHeight + spacing : 0)
                             + (implicitFooterHeight > 0 ? implicitFooterHeight + spacing : 0))

    padding: Theme.spaceXl
    topPadding: Theme.spaceSm
    anchors.centerIn: T.Overlay.overlay
    modal: true

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.motionNormal; easing.type: Easing.OutCubic }
        NumberAnimation { property: "scale"; from: 0.94; to: 1; duration: Theme.motionNormal; easing.type: Easing.OutCubic }
    }
    exit: Transition {
        NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.motionFast }
    }

    background: Rectangle {
        color: Theme.bgOverlay
        radius: Theme.radiusLg
        border.width: Theme.borderWidth
        border.color: Theme.border
    }

    header: Label {
        text: control.title
        visible: control.title
        elide: Label.ElideRight
        font.pixelSize: Theme.fontSizeTitle
        font.weight: Theme.weightSemibold
        leftPadding: Theme.spaceXl
        rightPadding: Theme.spaceXl
        topPadding: Theme.spaceXl
        bottomPadding: Theme.spaceXs
    }

    footer: DialogButtonBox {
        visible: count > 0
    }

    T.Overlay.modal: Rectangle { color: Theme.scrim }
    T.Overlay.modeless: Rectangle { color: "transparent" }
}
