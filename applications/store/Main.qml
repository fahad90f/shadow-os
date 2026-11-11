// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.store

NxWindow {
    id: window
    title: qsTr("NEXUS Store")
    width: 1200
    height: 820

    Store { id: backend }

    // ---- navigation ---------------------------------------------------------
    // A stack of places. The first entry is what the sidebar selects
    // (overview, a category, search, installed, updates, repositories); the
    // application page is pushed on top of it and "Back" pops it.
    //   { view: "overview" } | { view: "category", key } | { view: "search" } |
    //   { view: "installed" } | { view: "updates" } | { view: "repos" } |
    //   { view: "app", id, name }   (id: AppStream ID, or name: package without AppStream data)
    property var places: [{ view: "overview" }]
    readonly property var place: places[places.length - 1]
    readonly property string rootKey: places[0].view === "category" ? "cat:" + places[0].key : places[0].view
    // Command-line target that waits for the catalogue to finish loading.
    property string pendingTarget: ""

    readonly property var viewOrder: ["overview", "category", "search", "installed", "updates", "repos", "app"]

    function show(root) { places = [root] }
    function push(entry) { places = places.concat([entry]) }
    function back() { if (places.length > 1) places = places.slice(0, places.length - 1) }
    function openApp(id) { push({ view: "app", id: id, name: "" }) }
    function openPackage(name) {
        const id = backend.appIdForPackage(name)
        push(id !== "" ? { view: "app", id: id, name: "" } : { view: "app", id: "", name: name })
    }
    function search(text) {
        searchField.text = text
        searchPage.query = text
        show({ view: "search" })
    }
    function activateSidebar(key) {
        if (key.startsWith("cat:")) {
            backend.showCategory(key.substring(4))
            show({ view: "category", key: key.substring(4) })
        } else {
            show({ view: key })
        }
    }

    function openTarget(argument) {
        if (argument === "") return
        if (backend.catalog.loading || backend.catalog.state === "idle") {
            pendingTarget = argument     // decided once the catalogue is known
            return
        }
        const target = backend.resolveTarget(argument)
        if (target.view === "app") { show({ view: "overview" }); openApp(target.id) }
        else if (target.view === "package") { show({ view: "overview" }); push({ view: "app", id: "", name: target.name }) }
        else if (target.view === "search") search(target.text)
        else if (target.view === "page") activateSidebar(target.key)
    }

    function copy(text) {
        backend.copyToClipboard(text)
        toaster.show(qsTr("Copied to the clipboard"), "success")
    }

    readonly property var sidebarModel: {
        let m = [{ key: "overview", title: qsTr("Overview"), icon: "home", section: qsTr("Discover"), badge: "" }]
        const cats = backend.catalog.categories
        for (let i = 0; i < cats.length; i++)
            m.push({ key: "cat:" + cats[i].key, title: cats[i].title, icon: cats[i].icon, section: qsTr("Categories"),
                     badge: cats[i].count > 0 ? String(cats[i].count) : "" })
        m.push({ key: "installed", title: qsTr("Installed"), icon: "package", section: qsTr("Manage"), badge: "" })
        m.push({ key: "updates", title: qsTr("Updates"), icon: "arrow-up-circle", section: qsTr("Manage"),
                 badge: backend.updates.count > 0 ? String(backend.updates.count) : "" })
        m.push({ key: "repos", title: qsTr("Repositories"), icon: "server", section: qsTr("Manage"), badge: "" })
        return m
    }

    Component.onCompleted: {
        backend.initialize()
        const args = nexusApp.arguments
        if (args.length > 0) openTarget(args[0])
    }

    Connections {
        target: nexusApp
        function onActivationRequested(args) {
            if (args.length > 0) window.openTarget(args[0])
            window.raise()
            window.requestActivate()
        }
    }

    Connections {
        target: backend.catalog
        function onLoaded() {
            if (window.pendingTarget !== "") {
                const t = window.pendingTarget
                window.pendingTarget = ""
                window.openTarget(t)
            }
            if (window.places[0].view === "category") backend.showCategory(window.places[0].key)
        }
    }

    Connections {
        target: backend
        function onJobFinished(success, kind, title) {
            if (success) window.toaster.show(qsTr("%1 — finished").arg(title), "success")
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        NxSidebar {
            Layout.fillHeight: true
            title: qsTr("Store")
            model: window.sidebarModel
            currentKey: window.rootKey
            onActivated: (key) => window.activateSidebar(key)
            header: NxSearchField {
                id: searchField
                Layout.fillWidth: true
                placeholderText: qsTr("Search software")
                onAccepted: window.search(text)
                onTextEdited: searchDelay.restart()
                Timer {
                    id: searchDelay
                    interval: 350
                    onTriggered: if (searchField.text.trim().length >= 2) window.search(searchField.text)
                }
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: window.viewOrder.indexOf(window.place.view)

                OverviewPage {
                    store: backend
                    onOpenCategory: (key) => window.activateSidebar("cat:" + key)
                    onOpenApp: (id) => window.openApp(id)
                    onOpenView: (view) => window.show({ view: view })
                }
                BrowsePage {
                    store: backend
                    categoryKey: window.places[0].view === "category" ? window.places[0].key : ""
                    onOpenApp: (id) => window.openApp(id)
                }
                SearchPage {
                    id: searchPage
                    store: backend
                    onOpenApp: (id) => window.openApp(id)
                    onOpenPackage: (name) => window.openPackage(name)
                }
                InstalledPage {
                    store: backend
                    onOpenApp: (id) => window.openApp(id)
                    onOpenPackage: (name) => window.openPackage(name)
                }
                UpdatesPage {
                    store: backend
                    onOpenPackage: (name) => window.openPackage(name)
                }
                ReposPage {
                    store: backend
                }
                AppPage {
                    store: backend
                    appId: window.place.view === "app" ? window.place.id : ""
                    fallbackPackage: window.place.view === "app" ? window.place.name : ""
                    active: window.place.view === "app"
                    onBack: window.back()
                    onSearchRequested: (text) => window.search(text)
                    onCopyRequested: (text) => window.copy(text)
                }
            }

            // The operation in progress (or the last one) stays in view on every page.
            Rectangle {
                Layout.fillWidth: true
                visible: backend.job !== null
                implicitHeight: jobPanel.implicitHeight + Theme.spaceLg * 2
                color: Theme.bgSurface
                Rectangle { width: parent.width; height: 1; color: Theme.borderSubtle }
                PkJobPanel {
                    id: jobPanel
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: Theme.spaceXl
                    anchors.rightMargin: Theme.spaceXl
                    job: backend.job
                    onCloseRequested: backend.dismissJob()
                    onCopyRequested: (text) => window.copy(text)
                    onRepairRequested: {
                        if (!backend.openUpdater("recovery"))
                            window.toaster.show(qsTr("NEXUS Updater is not installed; it provides the package-system repair."), "warning")
                    }
                }
            }
        }
    }

    PkJobDialogs {
        anchors.fill: parent
        job: backend.job
    }
}
