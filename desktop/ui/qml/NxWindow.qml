// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls

// Top-level window of a NEXUS application: token-driven default and minimum
// sizes, the base background, and a ready-made toaster.
//   NxWindow { title: "NEXUS Files"; ... ; Component.onCompleted: toaster.show("Hi") }
ApplicationWindow {
    id: root

    property alias toaster: toasterItem

    width: Theme.windowDefaultWidth
    height: Theme.windowDefaultHeight
    minimumWidth: Theme.windowMinWidth
    minimumHeight: Theme.windowMinHeight
    visible: true
    color: Theme.bgBase

    NxToaster {
        id: toasterItem
        parent: root.contentItem
        anchors.fill: parent
    }
}
