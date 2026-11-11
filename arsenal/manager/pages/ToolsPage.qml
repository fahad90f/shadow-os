// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.arsenal

// Search across the tools of all profiles.
NxPage {
    id: page

    readonly property var stateKeys: ["all", "installed", "missing"]
    readonly property var interfaceKeys: ["all", "gui", "cli"]
    readonly property var categoryModel: [{ id: "", name: qsTr("All categories") }].concat(arsenal.categories)
    readonly property var results: {
        arsenal.revision
        return arsenal.searchTools(search.text, stateKeys[stateFilter.currentIndex], interfaceKeys[interfaceFilter.currentIndex],
                                   categoryModel[Math.max(0, categoryBox.currentIndex)].id)
    }

    signal toolOpened(string packageName)
    signal actionRequested(string kind, string target)

    title: qsTr("All tools")
    subtitle: arsenal.loadError !== "" ? "" : qsTr("Every tool of every profile, each listed once.")
    maxContentWidth: 960

    function focusSearch() { search.forceActiveFocus() }

    NxEmptyState {
        Layout.fillWidth: true
        Layout.topMargin: Theme.spaceXxl
        visible: arsenal.loadError !== ""
        icon: "package"
        tone: "warning"
        title: qsTr("The profile manifests are not available")
        message: arsenal.loadError
    }

    ColumnLayout {
        Layout.fillWidth: true
        visible: arsenal.loadError === ""
        spacing: Theme.spaceMd

        NxSearchField {
            id: search
            Layout.fillWidth: true
            placeholderText: qsTr("Search by name, package, command or purpose")
            Accessible.name: qsTr("Search tools")
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceMd
            ComboBox {
                id: categoryBox
                Layout.preferredWidth: 260
                model: page.categoryModel
                textRole: "name"
                Accessible.name: qsTr("Category")
            }
            NxSegmented {
                id: stateFilter
                model: [qsTr("All"), qsTr("Installed"), qsTr("Not installed")]
                onActivated: (index) => currentIndex = index
            }
            NxSegmented {
                id: interfaceFilter
                model: [qsTr("Any"), qsTr("GUI"), qsTr("CLI")]
                onActivated: (index) => currentIndex = index
            }
            Item { Layout.fillWidth: true }
            Text {
                text: (page.results.length === 1 ? qsTr("1 tool") : qsTr("%1 tools").arg(page.results.length))
                color: Theme.textMuted
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
            }
        }
    }

    ToolList {
        visible: arsenal.loadError === "" && page.results.length > 0
        tools: page.results
        showCategory: true
        onOpened: (name) => page.toolOpened(name)
        onInstallRequested: (name) => page.actionRequested("install-tool", name)
    }

    NxEmptyState {
        Layout.fillWidth: true
        visible: arsenal.loadError === "" && page.results.length === 0
        icon: "search"
        title: qsTr("No tool matches")
        message: search.text !== "" ? qsTr("Nothing in the profiles matches “%1” with these filters. Tools that are not packaged are listed under External tools.").arg(search.text)
                                    : qsTr("No tool matches these filters.")
        actionText: qsTr("Clear filters")
        onActionTriggered: { search.text = ""; stateFilter.currentIndex = 0; interfaceFilter.currentIndex = 0; categoryBox.currentIndex = 0 }
    }
}
