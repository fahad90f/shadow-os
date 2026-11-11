// SPDX-License-Identifier: GPL-3.0-or-later
import Qt.labs.folderlistmodel
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Dialogs.quickimpl
import QtQuick.Layouts
import QtQuick.Templates as T
import org.nexus.ui

import "." as Parts

// NEXUS look of Qt Quick Dialogs' FolderDialog — see FileDialog.qml.
FolderDialogImpl {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding,
                            implicitHeaderWidth, implicitFooterWidth)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding
                             + (implicitHeaderHeight > 0 ? implicitHeaderHeight + spacing : 0)
                             + (implicitFooterHeight > 0 ? implicitFooterHeight + spacing : 0))

    leftPadding: Theme.spaceMd
    rightPadding: Theme.spaceMd
    topPadding: Theme.spaceXs
    bottomPadding: Theme.spaceXs

    standardButtons: T.Dialog.Open | T.Dialog.Cancel

    // Qt shows this dialog either as a window of its own (then the window
    // frame carries the title and gives the shape) or inside the
    // application window's overlay like any other Dialog.
    readonly property bool embedded: header.parent !== null && header.parent.parent === T.Overlay.overlay

    FolderDialogImpl.folderDialogListView: folderDialogListView
    FolderDialogImpl.breadcrumbBar: breadcrumbBar

    background: Rectangle {
        implicitWidth: 680
        implicitHeight: 480
        color: Theme.bgOverlay
        radius: control.embedded ? Theme.radiusLg : 0
        border.width: control.embedded ? Theme.borderWidth : 0
        border.color: Theme.border
        // See Popup.qml: presses on the dialog never reach the content beneath it.
        TapHandler {
            acceptedButtons: Qt.AllButtons
            gesturePolicy: TapHandler.ReleaseWithinBounds
        }
    }

    header: ColumnLayout {
        spacing: Theme.spaceMd
        Label {
            objectName: "dialogTitleBarLabel"
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spaceXl
            Layout.rightMargin: Theme.spaceXl
            Layout.topMargin: Theme.spaceLg
            text: control.title
            visible: control.embedded && control.title.length > 0
            elide: Label.ElideRight
            font.pixelSize: Theme.fontSizeTitle
            font.weight: Theme.weightSemibold
        }
        Parts.NxPathBar {
            id: breadcrumbBar
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spaceLg
            Layout.rightMargin: Theme.spaceLg
            Layout.topMargin: control.embedded && control.title.length > 0 ? 0 : Theme.spaceLg
            dialog: control
            KeyNavigation.tab: folderDialogListView
        }
    }

    contentItem: ListView {
        id: folderDialogListView
        objectName: "folderDialogListView"
        clip: true
        focus: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { }

        model: FolderListModel {
            folder: control.currentFolder
            showFiles: false
            sortCaseSensitive: false
        }
        delegate: Parts.NxFileRow {
            objectName: "folderDialogDelegate" + index
            showSize: false
            width: ListView.view.width - (ListView.view.ScrollBar.vertical.visible ? ListView.view.ScrollBar.vertical.width : 0)
            highlighted: ListView.isCurrentItem
            dialog: control
            KeyNavigation.backtab: breadcrumbBar
            KeyNavigation.tab: control.footer
        }

        Label {
            anchors.centerIn: parent
            visible: folderDialogListView.count === 0
            text: qsTr("This folder contains no folders")
            color: Theme.textMuted
        }
    }

    footer: DialogButtonBox {
        id: buttonBox
        standardButtons: control.standardButtons
    }

    T.Overlay.modal: Rectangle {
        color: Theme.scrim
        TapHandler {
            acceptedButtons: Qt.AllButtons
            gesturePolicy: TapHandler.ReleaseWithinBounds
        }
    }
    T.Overlay.modeless: Rectangle { color: "transparent" }
}
