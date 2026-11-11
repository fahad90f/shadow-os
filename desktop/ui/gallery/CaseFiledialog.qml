// SPDX-License-Identifier: GPL-3.0-or-later
import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import org.nexus.ui

// Test page: Qt Quick Dialogs' FileDialog as NEXUS applications get it — the
// Qt Quick implementation inside the window (there is no native file dialog
// in a NEXUS session), drawn by the files org.nexus.style provides for it.
NxWindow {
    id: window
    title: "NexusUI Gallery — file dialog"
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
        fileMode: nexusApp.arguments.indexOf("save") >= 0 ? FileDialog.SaveFile : FileDialog.OpenFile
    }
    Component.onCompleted: fileDialog.open()
}
