// SPDX-License-Identifier: GPL-3.0-or-later
import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import org.shadow.ui

// Test page: Qt Quick Dialogs' FolderDialog as SHADOW applications get it — the
// Qt Quick implementation inside the window (there is no native folder dialog
// in a SHADOW session), drawn by the files org.shadow.style provides for it.
NxWindow {
    id: window
    title: "ShadowUI Gallery — folder dialog"
    width: 1000
    height: 700

    ColumnLayout {
        anchors.centerIn: parent
        spacing: Theme.spaceMd
        Label { text: "Selected: " + (folderDialog.selectedFolder.toString() || "nothing yet") }
        NxButton { text: "Choose folder…"; onClicked: folderDialog.open() }
    }

    FolderDialog {
        id: folderDialog
        title: "Choose a folder"
        currentFolder: "file:///usr/share"
    }
    Component.onCompleted: folderDialog.open()
}
