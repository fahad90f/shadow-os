// SPDX-License-Identifier: GPL-3.0-or-later
import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import org.shadow.ui

// Test page: Qt Quick Dialogs' FileDialog as SHADOW applications get it — the
// Qt Quick implementation inside the window (there is no native file dialog
// in a SHADOW session), drawn by the files org.shadow.style provides for it.
NxWindow {
    id: window
    title: "ShadowUI Gallery — file dialog"
    width: 1000
    height: 700

    ColumnLayout {
        anchors.centerIn: parent
        spacing: Theme.spaceMd
        Label { text: "Selected: " + (fileDialog.selectedFile.toString() || "nothing yet") }
        RowLayout {
            NxButton { text: "Open file…"; onClicked: { fileDialog.fileMode = FileDialog.OpenFile; fileDialog.open() } }
            NxButton { text: "Save file…"; onClicked: { fileDialog.fileMode = FileDialog.SaveFile; fileDialog.open() } }
        }
    }

    FileDialog {
        id: fileDialog
        title: "Choose a wallpaper"
        currentFolder: "file:///usr/share"
        nameFilters: ["All files (*)", "Images (*.png *.jpg *.svg)", "Text (*.txt *.md)"]
        fileMode: shadowApp.arguments.indexOf("save") >= 0 ? FileDialog.SaveFile : FileDialog.OpenFile
    }
    Component.onCompleted: fileDialog.open()
}
