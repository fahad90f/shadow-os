// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.arsenal

NxWindow {
    id: window

    title: qsTr("SHADOW Arsenal")
    width: 1240
    height: 820

    // "overview" | "tools" | "external" | "responsible-use" | <profile id>
    property string currentPage: "overview"
    readonly property var profiles: arsenal.profiles
    readonly property var fixedPages: ["overview", "tools", "external", "responsible-use"]

    function isProfile(key) {
        return window.profiles.some(profile => profile.id === key)
    }

    function showPage(key) {
        if (fixedPages.indexOf(key) >= 0 || isProfile(key))
            currentPage = key
    }

    // Start an install or remove, or — without PackageKit — show the apt command.
    function requestAction(kind, target) {
        const headings = {
            "install-profile": qsTr("Install profile"), "install-profile-tools": qsTr("Install the tools of the profile"),
            "remove-profile": qsTr("Remove profile"), "install-tool": qsTr("Install tool"), "remove-tool": qsTr("Remove tool")
        }
        if (!arsenal.packageKit.available) {
            aptDialog.show(kind, target, headings[kind] || "")
            return
        }
        if (arsenal.job.active) {
            toaster.show(qsTr("Another operation is still running"), "warning")
            return
        }
        const started = kind === "install-profile" ? arsenal.installProfile(target)
            : kind === "install-profile-tools" ? arsenal.installProfileTools(target)
            : kind === "remove-profile" ? arsenal.removeProfile(target)
            : kind === "install-tool" ? arsenal.installTool(target)
            : kind === "remove-tool" ? arsenal.removeTool(target) : false
        if (!started)
            toaster.show(qsTr("There is nothing to do for this selection"), "info")
        else
            toolDialog.close()
    }

    Component.onCompleted: {
        if (arsenal.initialPage !== "")
            showPage(arsenal.initialPage)
        if (!arsenal.acknowledged)
            ackDialog.open()
    }

    Connections {
        target: shadowApp
        function onActivationRequested(arguments) {
            const page = arsenal.pageFromArguments(arguments)
            if (page !== "")
                window.showPage(page)
            window.raise()
            window.requestActivate()
        }
    }

    Shortcut {
        sequence: StandardKey.Find
        onActivated: {
            window.currentPage = "tools"
            Qt.callLater(() => { if (pageLoader.item && pageLoader.item.focusSearch) pageLoader.item.focusSearch() })
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        NxSidebar {
            Layout.fillHeight: true
            title: qsTr("Arsenal")
            currentKey: window.currentPage
            model: {
                let items = [{ key: "overview", title: qsTr("Overview"), icon: "grid" }]
                for (const profile of window.profiles) {
                    items.push({ key: profile.id, title: profile.name, icon: profile.glyph, section: qsTr("Profiles"),
                                 badge: arsenal.stateReady && profile.installedCount > 0 ? profile.installedCount + "/" + profile.toolCount : "" })
                }
                items.push({ key: "tools", title: qsTr("All tools"), icon: "search", section: qsTr("Browse") })
                items.push({ key: "external", title: qsTr("External tools"), icon: "external-link", section: qsTr("Browse") })
                items.push({ key: "responsible-use", title: qsTr("Responsible use"), icon: "shield-check", section: qsTr("About") })
                return items
            }
            onActivated: (key) => window.currentPage = key
        }

        Loader {
            id: pageLoader
            Layout.fillWidth: true
            Layout.fillHeight: true
            sourceComponent: window.currentPage === "overview" ? overviewPage
                           : window.currentPage === "tools" ? toolsPage
                           : window.currentPage === "external" ? externalPage
                           : window.currentPage === "responsible-use" ? responsiblePage
                           : profilePage
        }
    }

    Component {
        id: overviewPage
        OverviewPage { onProfileOpened: (id) => window.currentPage = id }
    }
    Component {
        id: profilePage
        ProfilePage {
            profileId: window.currentPage
            onToolOpened: (name) => toolDialog.show(name)
            onActionRequested: (kind, target) => window.requestAction(kind, target)
            onMessage: (text, kind) => window.toaster.show(text, kind)
        }
    }
    Component {
        id: toolsPage
        ToolsPage {
            onToolOpened: (name) => toolDialog.show(name)
            onActionRequested: (kind, target) => window.requestAction(kind, target)
        }
    }
    Component {
        id: externalPage
        ExternalPage { onMessage: (text, kind) => window.toaster.show(text, kind) }
    }
    Component {
        id: responsiblePage
        ResponsiblePage { onMessage: (text, kind) => window.toaster.show(text, kind) }
    }

    ToolDialog {
        id: toolDialog
        onActionRequested: (kind, target) => window.requestAction(kind, target)
        onMessage: (text, kind) => window.toaster.show(text, kind)
        onProfileRequested: (id) => window.currentPage = id
    }
    AptDialog {
        id: aptDialog
        onMessage: (text, kind) => window.toaster.show(text, kind)
    }
    JobDialog {
        onFollowUp: (kind, target) => window.requestAction(kind, target)
    }
    AckDialog {
        id: ackDialog
        onDeclined: Qt.quit()
    }
}
