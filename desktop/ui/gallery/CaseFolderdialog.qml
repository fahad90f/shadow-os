// SPDX-License-Identifier: GPL-3.0-or-later
import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import org.nexus.ui

// Test page: Qt Quick Dialogs' FolderDialog as NEXUS applications get it — the
// Qt Quick implementation inside the window (there is no native folder dialog
// in a NEXUS session), drawn by the files org.nexus.style provides for it.
NxWindow {
    id: window
    title: "NexusUI Gallery — folder dialog"
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
