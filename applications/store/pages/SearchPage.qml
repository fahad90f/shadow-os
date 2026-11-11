// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.store

// Search results. Two sources, chosen explicitly:
//   Applications   AppStream full-text search (name, summary, keywords) in the catalogue
//   All packages   PackageKit SearchNames / SearchDetails over every package of the enabled sources
PkListPage {
    id: page

    required property QtObject store
    property string query: ""
    // 0 = applications (AppStream), 1 = all packages (PackageKit)
    property int mode: 0
    property bool descriptions: false

    signal openApp(string id)
    signal openPackage(string name)

    function run() {
        if (mode === 0) store.searchApplications(query)
        else store.searchAllPackages(query, descriptions)
    }
    onQueryChanged: run()
    onModeChanged: run()
    onDescriptionsChanged: if (mode === 1) run()

    readonly property string searchState: mode === 0 ? store.appSearchState : store.packageSearchState

    title: query.trim() !== "" ? qsTr("Search: %1").arg(query.trim()) : qsTr("Search")
    subtitle: mode === 0
        ? (searchState === "searching" ? qsTr("Searching the application catalogue…")
           : searchState === "done" ? (count === 1 ? qsTr("1 application matches.") : qsTr("%1 applications match.").arg(count))
           : qsTr("Type what you are looking for in the search field."))
        : (searchState === "searching" ? qsTr("PackageKit is searching all packages…")
           : searchState === "done" ? (count === 1 ? qsTr("1 package matches.") : qsTr("%1 packages match.").arg(count))
           : searchState === "unavailable" ? qsTr("Packages cannot be searched right now.")
           : searchState === "error" ? qsTr("The package search failed.")
           : qsTr("Type at least two characters to search all packages."))

    model: mode === 0 ? store.searchApps : store.searchPackages
    delegate: mode === 0 ? appDelegate : packageDelegate

    Component {
        id: appDelegate
        AppRow {
            width: page.contentWidth
            onClicked: page.openApp(appId)
        }
    }
    Component {
        id: packageDelegate
        PackageRow {
            width: page.contentWidth
            onClicked: page.openPackage(name)
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spaceMd
        NxSegmented {
            model: [qsTr("Applications"), qsTr("All packages")]
            currentIndex: page.mode
            onActivated: (index) => page.mode = index
        }
        CheckBox {
            visible: page.mode === 1
            text: qsTr("Also search descriptions")
            checked: page.descriptions
            onToggled: page.descriptions = checked
        }
        Item { Layout.fillWidth: true }
        BusyIndicator {
            running: page.searchState === "searching"
            visible: running
            implicitWidth: 24
            implicitHeight: 24
            padding: 0
        }
    }

    PkServiceBanner {
        visible: page.mode === 1 && page.searchState === "unavailable"
        service: page.store.service
        consequence: qsTr("“All packages” asks PackageKit; applications can still be searched in the catalogue.")
    }
    NxBanner {
        visible: page.mode === 1 && page.searchState === "error"
        kind: "danger"
        title: qsTr("The package search failed")
        message: qsTr("Reason:") + "\n" + page.store.packageSearchError
    }
    NxBanner {
        visible: page.mode === 1 && page.searchState === "done" && page.store.packageSearchTruncated
        kind: "info"
        message: qsTr("More packages match than are shown. Type a longer or more specific text to narrow the search.")
    }

    footer: Item {
        width: page.contentWidth
        height: empty.visible ? empty.implicitHeight : 0
        NxEmptyState {
            id: empty
            anchors.fill: parent
            visible: page.count === 0 && (page.searchState === "done" || page.searchState === "idle")
            icon: "search"
            title: page.searchState === "idle" ? qsTr("Search for software")
                 : page.mode === 0 ? qsTr("No application matches “%1”").arg(page.query.trim())
                                   : qsTr("No package matches “%1”").arg(page.query.trim())
            message: page.searchState === "idle"
                ? (page.mode === 0 ? qsTr("Applications are searched by name, summary and keywords in the AppStream catalogue.")
                                   : qsTr("All packages of the enabled software sources are searched by name."))
                : page.mode === 0
                    ? (page.store.catalog.applicationCount === 0
                        ? qsTr("The application catalogue is empty on this system. Try “All packages”.")
                        : qsTr("Libraries, command-line tools and other packages without catalogue data are found under “All packages”."))
                    : qsTr("Check the spelling, or search the descriptions as well.")
            actionText: page.searchState === "done" && page.mode === 0 ? qsTr("Search all packages") : ""
            onActionTriggered: page.mode = 1
        }
    }
}
