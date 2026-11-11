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

// NEXUS look of Qt Quick Dialogs' FileDialog (the Qt Quick implementation,
// used when no native dialog is shown — which is always the case in NEXUS
// applications, see Nexus::App). Qt picks this file instead of its built-in
// one through the file selector "+org.nexus.style"; the logic (navigation,
// filters, overwrite confirmation, keyboard) stays in Qt's FileDialogImpl,
// which is told below which item plays which part.
FileDialogImpl {
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

    FileDialogImpl.buttonBox: buttonBox
    FileDialogImpl.nameFiltersComboBox: nameFiltersComboBox
    FileDialogImpl.fileDialogListView: fileDialogListView
    FileDialogImpl.breadcrumbBar: breadcrumbBar
    FileDialogImpl.fileNameLabel: fileNameLabel
    FileDialogImpl.fileNameTextField: fileNameTextField
    FileDialogImpl.overwriteConfirmationDialog: overwriteConfirmationDialog

    Dialog {
        id: overwriteConfirmationDialog
        objectName: "confirmationDialog"
        anchors.centerIn: parent
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
        dim: true
        modal: true
        title: qsTr("Replace the file?")
        width: Math.min(420, control.width - control.leftPadding - control.rightPadding)

        contentItem: Label {
            text: qsTr("“%1” already exists. Saving will replace its contents.").arg(control.fileName)
            wrapMode: Text.WordWrap
        }
        footer: DialogButtonBox {
            standardButtons: DialogButtonBox.Yes | DialogButtonBox.No
        }
    }

    background: Rectangle {
        implicitWidth: 760
        implicitHeight: 520
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
            KeyNavigation.tab: fileDialogListView
        }
    }

    contentItem: ListView {
        id: fileDialogListView
        objectName: "fileDialogListView"
        clip: true
        focus: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { }

        model: FolderListModel {
            folder: control.currentFolder
            nameFilters: control.selectedNameFilter.globs
            showDirsFirst: true
            sortCaseSensitive: false
        }
        delegate: Parts.NxFileRow {
            objectName: "fileDialogDelegate" + index
            required property double fileSize
            required property bool fileIsDir
            width: ListView.view.width - (ListView.view.ScrollBar.vertical.visible ? ListView.view.ScrollBar.vertical.width : 0)
            highlighted: ListView.isCurrentItem
            dialog: control
            KeyNavigation.backtab: breadcrumbBar
            KeyNavigation.tab: fileNameTextField.visible ? fileNameTextField : nameFiltersComboBox
        }

        Label {
            anchors.centerIn: parent
            visible: fileDialogListView.count === 0
            text: qsTr("This folder has no matching files")
            color: Theme.textMuted
        }
    }

    footer: GridLayout {
        columns: 3
        columnSpacing: Theme.spaceMd
        rowSpacing: Theme.spaceSm

        Label {
            id: fileNameLabel
            Layout.leftMargin: Theme.spaceXl
            Layout.topMargin: Theme.spaceSm
            text: qsTr("Name")
            color: Theme.textSecondary
            visible: false          // shown by FileDialogImpl when saving
        }
        TextField {
            id: fileNameTextField
            objectName: "fileNameTextField"
            Layout.fillWidth: true
            Layout.columnSpan: 2
            Layout.rightMargin: Theme.spaceXl
            Layout.topMargin: Theme.spaceSm
            visible: false
        }

        Label {
            Layout.row: 1
            Layout.column: 0
            Layout.leftMargin: Theme.spaceXl
            Layout.topMargin: Theme.spaceSm
            Layout.bottomMargin: Theme.spaceLg
            text: qsTr("Show")
            color: Theme.textSecondary
        }
        ComboBox {
            id: nameFiltersComboBox
            Layout.row: 1
            Layout.column: 1
            Layout.fillWidth: true
            Layout.maximumWidth: 320
            Layout.topMargin: Theme.spaceSm
            Layout.bottomMargin: Theme.spaceLg
            model: control.nameFilters
        }
        DialogButtonBox {
            id: buttonBox
            Layout.row: 1
            Layout.column: 2
            Layout.alignment: Qt.AlignRight
            Layout.rightMargin: Theme.spaceXl
            Layout.topMargin: Theme.spaceSm
            Layout.bottomMargin: Theme.spaceLg
            standardButtons: control.standardButtons
            padding: 0
        }
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
