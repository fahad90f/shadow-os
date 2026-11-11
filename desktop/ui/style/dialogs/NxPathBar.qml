// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs.quickimpl as Impl
import org.shadow.ui

// Path bar of the SHADOW file and folder dialogs: "up" button, one button per
// path component, and (Ctrl+L) a text field to type a path. The behaviour is
// Qt's FolderBreadcrumbBar; this file only gives it the SHADOW look.
Impl.FolderBreadcrumbBar {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + (upButton ? upButton.implicitWidth + upButtonSpacing : 0)
                            + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)
    upButtonSpacing: Theme.spaceSm
    padding: 3

    background: Rectangle {
        implicitHeight: Theme.controlHeight + 6
        radius: Theme.radiusSm
        color: Theme.bgInput
        border.width: Theme.borderWidth
        border.color: Theme.border
    }

    contentItem: ListView {
        currentIndex: control.currentIndex
        model: control.contentModel
        orientation: ListView.Horizontal
        snapMode: ListView.SnapToItem
        highlightMoveDuration: 0
        interactive: false
        clip: true
    }

    buttonDelegate: ToolButton {
        id: crumb
        required property int index
        required property string folderName
        text: folderName
        height: control.availableHeight
        leftPadding: Theme.spaceSm
        rightPadding: Theme.spaceSm
        font.weight: index === control.currentIndex ? Theme.weightSemibold : Theme.weightRegular
    }

    separatorDelegate: Item {
        width: 14
        height: control.availableHeight
        NxIcon {
            anchors.centerIn: parent
            name: "chevron-right"
            size: 12
            color: Theme.textMuted
        }
    }

    upButton: ToolButton {
        x: control.leftPadding
        y: control.topPadding
        width: height
        height: control.availableHeight
        icon.source: "image://nxicon/arrow-up"
        icon.width: Theme.iconSmall
        icon.height: Theme.iconSmall
        focusPolicy: Qt.TabFocus
        Accessible.name: qsTr("Parent folder")
        ToolTip.visible: hovered
        ToolTip.text: qsTr("Parent folder")
    }

    textField: TextField {
        text: (control.dialog as Impl.FileDialogImpl)?.selectedFile
            ?? (control.dialog as Impl.FolderDialogImpl).currentFolder
    }
}
