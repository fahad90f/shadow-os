// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.nexus.ui

// The system tray in the panel: one icon per status notifier item that is
// not passive. Takes no space while there is none.
Row {
    id: root

    required property var shell
    required property var panel

    height: Theme.panelHeight
    spacing: 0
    visible: tray.shownCount > 0
    Accessible.role: Accessible.ToolBar
    Accessible.name: qsTr("System tray")

    Repeater {
        model: tray
        TrayIcon {
            required property var trayItem
            item: trayItem
            shell: root.shell
            panel: root.panel
        }
    }
}
