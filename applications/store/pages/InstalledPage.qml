// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.store

// What is installed: applications (installed packages that have a catalogue
// entry) and all packages. Removing goes through PackageKit, after a
// simulation that lists everything that would be removed with it.
PkListPage {
    id: page

    required property QtObject store
    // 0 = applications, 1 = all packages
    property int mode: 0
    readonly property bool canRemove: store.service.available && !store.busy && store.installedSource === "packagekit"

    signal openApp(string id)
    signal openPackage(string name)

    title: qsTr("Installed")
    subtitle: store.installedState === "loading" ? qsTr("Reading the installed packages…")
        : store.installedState === "error" ? qsTr("The installed packages could not be read.")
        : mode === 0 ? qsTr("%1 installed applications with a catalogue entry, out of %2 installed packages.")
                           .arg(store.installedAppCount).arg(store.installedPackageCount)
                     : qsTr("%1 installed packages.").arg(store.installedPackageCount)
    actions: [
        NxIconButton {
            iconName: "refresh"
            tooltip: qsTr("Read the installed packages again")
            onClicked: page.store.refreshInstalled()
        }
    ]

    model: mode === 0 ? store.installedApps : store.installedPackages
    delegate: mode === 0 ? appDelegate : packageDelegate

    Component {
        id: appDelegate
        AppRow {
            width: page.contentWidth
            actionText: qsTr("Remove")
            actionEnabled: page.canRemove
            onClicked: page.openApp(appId)
            onActionTriggered: page.store.removeInstalled(packageName, name)
        }
    }
    Component {
        id: packageDelegate
        PackageRow {
            width: page.contentWidth
            actionText: qsTr("Remove")
            actionEnabled: page.canRemove && packageId !== ""
            onClicked: page.openPackage(name)
            onActionTriggered: page.store.removePackage(packageId, name)
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
        NxSearchField {
            Layout.fillWidth: true
            Layout.maximumWidth: 360
            placeholderText: page.mode === 0 ? qsTr("Filter installed applications") : qsTr("Filter installed packages")
            onTextChanged: { page.store.installedApps.filter = text; page.store.installedPackages.filter = text }
        }
        Item { Layout.fillWidth: true }
        Text {
            text: qsTr("%1 shown").arg(page.count)
            color: Theme.textMuted
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
        }
    }

    NxBanner {
        visible: page.store.installedState === "ready" && page.store.installedSource === "dpkg"
        kind: "warning"
        title: qsTr("Read-only list from the dpkg database")
        message: qsTr("PackageKit is not reachable, so this list was read with dpkg-query and nothing can be removed from here.")
                 + (page.store.installedError !== "" ? "\n" + page.store.installedError : "")
    }
    NxBanner {
        visible: page.store.installedState === "error"
        kind: "danger"
        title: qsTr("The installed packages could not be read")
        message: qsTr("Reason:") + "\n" + page.store.installedError
    }

    footer: Item {
        width: page.contentWidth
        height: empty.visible ? empty.implicitHeight : 0
        NxEmptyState {
            id: empty
            anchors.fill: parent
            visible: page.count === 0 && page.store.installedState === "ready"
            icon: "package"
            title: page.mode === 0 ? qsTr("No installed application has a catalogue entry") : qsTr("No package matches")
            message: page.mode === 0
                ? (page.store.catalog.applicationCount === 0
                    ? qsTr("The application catalogue is empty on this system, so installed applications cannot be recognised. “All packages” lists everything that is installed.")
                    : qsTr("None of the %1 installed packages belongs to an application in the catalogue, or the filter excludes them.").arg(page.store.installedPackageCount))
                : qsTr("The filter excludes every installed package.")
        }
    }
}
