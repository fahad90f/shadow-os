// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Templates as T
import org.nexus.ui

T.GroupBox {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding,
                            implicitLabelWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)

    spacing: Theme.spaceSm
    padding: Theme.spaceLg
    topPadding: padding + (implicitLabelWidth > 0 ? implicitLabelHeight + spacing : 0)

    label: Text {
        x: control.leftPadding
        width: control.availableWidth
        text: control.title
        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeSmall
        font.weight: Theme.weightSemibold
        font.capitalization: Font.AllUppercase
        font.letterSpacing: 0.8
        color: Theme.textMuted
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
    }

    background: Rectangle {
        y: control.topPadding - control.bottomPadding
        width: parent.width
        height: parent.height - control.topPadding + control.bottomPadding
        color: Theme.bgRaised
        radius: Theme.radiusMd
        border.width: Theme.borderWidth
        border.color: Theme.borderSubtle
    }
}
