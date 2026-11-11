// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs.quickimpl as Impl
import org.nexus.ui

// One entry of the file list in the NEXUS file and folder dialogs. Selection,
// double-click and keyboard handling are Qt's FileDialogDelegate.
Impl.FileDialogDelegate {
    id: control

    // Roles of FolderListModel. 'index' is read by the C++ side.
    required property int index
    required property string fileName
    required property url fileUrl
    required property date fileModified
    property double fileSize: -1
    property bool fileIsDir: true
    property bool showSize: true

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)
    leftPadding: Theme.spaceMd
    rightPadding: Theme.spaceMd
    topPadding: 0
    bottomPadding: 0
    spacing: Theme.spaceSm

    file: fileUrl

    contentItem: Row {
        spacing: control.spacing
        NxIcon {
            anchors.verticalCenter: parent.verticalCenter
            name: control.fileIsDir ? "folder" : "file"
            size: Theme.iconSmall + 2
            color: control.fileIsDir ? Theme.accent : Theme.textSecondary
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            width: Math.max(60, control.availableWidth - parent.spacing * 3 - Theme.iconSmall - 2
                                - (control.showSize ? sizeText.width : -parent.spacing) - dateText.width)
            text: control.fileName
            color: Theme.textPrimary
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeBody
            elide: Text.ElideMiddle
        }
        Text {
            id: sizeText
            visible: control.showSize
            anchors.verticalCenter: parent.verticalCenter
            width: 84
            horizontalAlignment: Text.AlignRight
            text: control.fileIsDir || control.fileSize < 0 ? "" : Theme.formatBytes(control.fileSize)
            color: Theme.textMuted
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
        }
        Text {
            id: dateText
            anchors.verticalCenter: parent.verticalCenter
            width: 128
            horizontalAlignment: Text.AlignRight
            text: Qt.formatDateTime(control.fileModified, "yyyy-MM-dd HH:mm")
            color: Theme.textMuted
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
        }
    }

    background: Rectangle {
        implicitWidth: 100
        implicitHeight: Theme.rowHeight - 8
        radius: Theme.radiusXs
        color: control.highlighted ? Theme.accentFill
            : control.down ? Theme.pressedOverlay
            : control.hovered ? Theme.hoverOverlay : "transparent"
        border.width: control.visualFocus ? Theme.focusRing : 0
        border.color: Theme.accent
    }
}
