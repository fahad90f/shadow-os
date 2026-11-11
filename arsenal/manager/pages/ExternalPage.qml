// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.arsenal

// Every well-known tool the profiles mention that is not packaged.
NxPage {
    id: page

    readonly property var entries: arsenal.externalTools("")
    signal message(string text, string kind)

    title: qsTr("External tools")
    subtitle: qsTr("Well-known tools that are not in Debian main and therefore not installable from SHADOW repositories. SHADOW Arsenal does not download, repackage or update them: follow the vendor's own instructions, and check what you download.")
    maxContentWidth: 860

    NxEmptyState {
        Layout.fillWidth: true
        Layout.topMargin: Theme.spaceXxl
        visible: page.entries.length === 0
        icon: arsenal.loadError !== "" ? "package" : "check-circle"
        tone: arsenal.loadError !== "" ? "warning" : "neutral"
        title: arsenal.loadError !== "" ? qsTr("The profile manifests are not available") : qsTr("No external tools are listed")
        message: arsenal.loadError
    }

    Repeater {
        model: page.entries
        ExternalCard {
            required property var modelData
            entry: modelData
            onMessage: (text, kind) => page.message(text, kind)
        }
    }
}
