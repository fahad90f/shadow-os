// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui

// Test page: toasts of an NxWindow with a sidebar. A long message wraps
// inside a width-limited toast instead of spanning the window.
NxWindow {
    id: window
    title: "NexusUI Gallery — toast"
    width: 1100
    height: 640

    readonly property string longText: "The connection “Office VPN (WireGuard, split tunnel)” could not be activated: "
        + "the peer did not answer within 30 seconds, so the previous routes and DNS servers were restored and nothing was changed on this computer."

    function widestToast() {
        let widest = 0
        const column = toaster.children[0]
        for (let i = 0; i < column.children.length; i++)
            if (column.children[i].text !== undefined) widest = Math.max(widest, column.children[i].width)
        return widest
    }
    function tallestToast() {
        let tallest = 0
        const column = toaster.children[0]
        for (let i = 0; i < column.children.length; i++)
            if (column.children[i].text !== undefined) tallest = Math.max(tallest, column.children[i].height)
        return tallest
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0
        NxSidebar {
            Layout.fillHeight: true
            title: "Toasts"
            currentKey: "a"
            model: [
                { key: "a", title: "Connections", icon: "network", section: "Network" },
                { key: "b", title: "VPN", icon: "lock", section: "Network" },
                { key: "c", title: "Firewall", icon: "firewall", section: "Network" },
            ]
        }
        NxPage {
            Layout.fillWidth: true
            Layout.fillHeight: true
            title: "Toasts"
            subtitle: "Short messages stay as wide as their text; long ones wrap at " + toaster.maxToastWidth + " px."
            actions: [
                NxButton { text: "Short"; onClicked: toaster.show("Copied", "success") },
                NxButton { text: "Long"; onClicked: toaster.show(window.longText, "danger") }
            ]
        }
    }

    Component.onCompleted: {
        toaster.duration = 60000
        toaster.show("Copied", "success")
        toaster.show(longText, "danger")
    }

    CaseChecks {
        name: "toast"
        interval: 300
        steps: [
            () => {
                verify(widestToast() <= toaster.maxToastWidth, "no toast is wider than maxToastWidth (" + widestToast() + ")")
                verify(widestToast() < window.width / 2, "the long toast does not span the window")
                verify(tallestToast() > 60, "the long text wrapped onto several lines (height " + tallestToast() + ")")
            }
        ]
    }
}
