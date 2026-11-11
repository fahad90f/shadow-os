// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.store

// Page of one application (an AppStream component) or of one plain package.
//
//   catalogue data   name, summary, description, screenshots, developer, licence, links,
//                    content rating, releases                       — AppStream (Catalog.details)
//   package data     installed and candidate version, origin repository, architecture, sizes,
//                    dependencies, verification                      — PackageKit (PackageInfo)
//   actions          install / update / remove                      — PackageKit jobs (Store)
NxPage {
    id: page

    required property QtObject store
    // AppStream component ID, or "" for a package without catalogue data.
    property string appId: ""
    property string fallbackPackage: ""
    // The page is the one being shown.
    property bool active: false

    signal back()
    signal searchRequested(string text)
    signal copyRequested(string text)

    readonly property var details: {
        const reloaded = store.catalog.state      // re-read after the catalogue was reloaded
        return appId !== "" && reloaded !== "" ? store.catalog.details(appId) : ({})
    }
    readonly property bool hasComponent: details.id !== undefined
    readonly property string packageName: hasComponent ? (details.packageName || "") : fallbackPackage
    readonly property string displayName: hasComponent ? details.name : fallbackPackage
    readonly property var urls: hasComponent ? details.urls : ({})
    readonly property var screenshots: hasComponent ? details.screenshots : []
    readonly property var changes: pkg.changes
    readonly property bool canAct: store.service.available && !store.busy && pkg.state === "ready"
    readonly property string systemArch: store.systemArchitecture

    function archText() {
        const rel = pkg.archRelation
        if (rel === "native") return qsTr("%1 — matches this system").arg(pkg.arch)
        if (rel === "all") return qsTr("all — independent of the processor architecture (this system: %1)").arg(systemArch)
        if (rel === "foreign") return qsTr("%1 — a foreign architecture (this system: %2)").arg(pkg.arch).arg(systemArch)
        return qsTr("Not reported")
    }
    // Direct dependencies as text; the ones not installed yet are marked.
    function dependencyNames(list) {
        var out = []
        for (var i = 0; list && i < list.length; i++)
            out.push(list[i].installed ? list[i].name : qsTr("%1 (not installed)").arg(list[i].name))
        return out.join(", ")
    }
    function installedCount(list) {
        var n = 0
        for (var i = 0; list && i < list.length; i++) if (list[i].installed) n++
        return n
    }
    function names(list) {
        var out = []
        for (var i = 0; list && i < list.length; i++) out.push(list[i].name)
        return out.join(", ")
    }

    onAppIdChanged: { flickable.contentY = 0; viewer.close() }
    onFallbackPackageChanged: flickable.contentY = 0

    PackageInfo {
        id: pkg
        packageName: page.active ? page.packageName : ""
    }
    Connections {
        target: page.store
        function onPackagesChanged() { if (page.active) pkg.refresh() }
    }

    maxContentWidth: 920
    contentSpacing: Theme.spaceLg

    // A "label: value" line inside a section; values are selectable plain text.
    component Fact: Item {
        id: fact
        property alias label: keyValue.label
        property alias value: keyValue.value
        property alias mono: keyValue.mono
        default property alias trailing: trailingRow.data
        // First row that is actually shown in its section (no separator above it).
        readonly property bool isFirst: {
            if (!parent) return true
            for (var i = 0; i < parent.children.length; i++) {
                var c = parent.children[i]
                if (c.visible && c.height > 0) return c === fact
            }
            return false
        }

        Layout.fillWidth: true
        implicitHeight: Math.max(Theme.rowHeight, keyValue.implicitHeight + Theme.spaceMd * 2)
        Rectangle {
            visible: !fact.isFirst
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: Theme.spaceLg
            height: 1
            color: Theme.borderSubtle
        }
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.spaceLg
            anchors.rightMargin: Theme.spaceLg
            spacing: Theme.spaceMd
            NxKeyValue {
                id: keyValue
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                labelWidth: 190
            }
            Row {
                id: trailingRow
                spacing: Theme.spaceSm
                Layout.alignment: Qt.AlignVCenter
            }
        }
    }

    // ---- back ---------------------------------------------------------------------
    RowLayout {
        Layout.fillWidth: true
        NxButton {
            text: qsTr("Back")
            kind: "ghost"
            iconName: "arrow-left"
            onClicked: page.back()
        }
        Item { Layout.fillWidth: true }
    }

    NxCard {
        Layout.fillWidth: true
        visible: page.appId !== "" && !page.hasComponent && !page.store.catalog.loading
        implicitHeight: 240
        NxEmptyState {
            anchors.fill: parent
            icon: "search"
            tone: "warning"
            title: qsTr("This application is not in the catalogue")
            message: qsTr("The AppStream catalogue on this system has no component with the requested ID.")
            actionText: qsTr("Search for it")
            onActionTriggered: page.searchRequested(page.appId)
        }
    }

    // ---- header -----------------------------------------------------------------------
    NxCard {
        Layout.fillWidth: true
        visible: page.hasComponent || page.fallbackPackage !== ""
        RowLayout {
            width: parent.width
            spacing: Theme.spaceXl

            AppIcon {
                Layout.alignment: Qt.AlignTop
                size: 84
                source: page.hasComponent ? (page.details.icon || "") : ""
                remote: page.hasComponent && page.details.iconRemote === true
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceXs
                PkText {
                    Layout.fillWidth: true
                    text: page.displayName
                    font.pixelSize: Theme.fontSizeDisplay
                    font.weight: Theme.weightSemibold
                    wrapMode: Text.WordWrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                }
                PkText {
                    Layout.fillWidth: true
                    text: page.hasComponent ? page.details.summary : pkg.summary
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSizeBodyLarge
                    wrapMode: Text.WordWrap
                }
                PkText {
                    Layout.fillWidth: true
                    visible: page.hasComponent && (page.details.developer || "") !== ""
                    text: page.hasComponent ? qsTr("by %1").arg(page.details.developer || "") : ""
                    color: Theme.textMuted
                    elide: Text.ElideRight
                }
                Flow {
                    Layout.fillWidth: true
                    Layout.topMargin: Theme.spaceXs
                    spacing: Theme.spaceSm
                    NxBadge { visible: pkg.installed; text: qsTr("Installed %1").arg(pkg.installedVersion); kind: "success" }
                    NxBadge { visible: pkg.updateAvailable; text: qsTr("Update to %1 available").arg(pkg.candidateVersion); kind: "warning" }
                    NxBadge { visible: pkg.state === "ready" && pkg.trust === "untrusted"; text: qsTr("Not verified"); kind: "danger" }
                    NxBadge { visible: page.hasComponent && page.details.kind === "console-application"; text: qsTr("Terminal application"); dot: false }
                    NxBadge { visible: !page.hasComponent && page.fallbackPackage !== ""; text: qsTr("Package without catalogue entry"); dot: false }
                    NxBadge { visible: page.hasComponent && page.details.licenseIsFree === true; text: qsTr("Free software"); kind: "accent"; dot: false }
                }
            }
            ColumnLayout {
                Layout.alignment: Qt.AlignTop
                spacing: Theme.spaceSm
                BusyIndicator {
                    Layout.alignment: Qt.AlignRight
                    visible: pkg.state === "loading"
                    running: visible
                }
                NxButton {
                    Layout.alignment: Qt.AlignRight
                    visible: pkg.state === "ready" && !pkg.installed && pkg.candidateAvailable
                    text: qsTr("Install")
                    kind: "primary"
                    iconName: "download"
                    enabled: page.canAct
                    onClicked: page.store.installPackage(pkg.candidateId, page.displayName)
                }
                NxButton {
                    Layout.alignment: Qt.AlignRight
                    visible: pkg.state === "ready" && pkg.updateAvailable
                    text: qsTr("Update")
                    kind: "primary"
                    iconName: "arrow-up-circle"
                    enabled: page.canAct
                    onClicked: page.store.updatePackage(pkg.candidateId, page.displayName)
                }
                NxButton {
                    Layout.alignment: Qt.AlignRight
                    visible: pkg.state === "ready" && pkg.installed
                    text: qsTr("Remove")
                    kind: "danger"
                    iconName: "trash"
                    enabled: page.canAct
                    onClicked: page.store.removePackage(pkg.installedId, page.displayName)
                }
                Text {
                    Layout.alignment: Qt.AlignRight
                    Layout.maximumWidth: 220
                    horizontalAlignment: Text.AlignRight
                    visible: text !== ""
                    text: pkg.state !== "ready" ? ""
                        : page.store.busy && page.store.jobTarget === page.packageName ? qsTr("In progress — see below")
                        : page.store.busy ? qsTr("Another operation is in progress")
                        : !pkg.installed && pkg.candidateAvailable && pkg.totalDownloadSize >= 0
                            ? qsTr("Download: %1").arg(Theme.formatBytes(pkg.totalDownloadSize))
                        : ""
                    color: Theme.textMuted
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                    wrapMode: Text.WordWrap
                }
            }
        }
    }

    // ---- what stands in the way -----------------------------------------------------------
    PkServiceBanner {
        visible: page.packageName !== "" && pkg.state === "unavailable"
        service: page.store.service
        consequence: qsTr("Versions, sizes and dependencies cannot be shown, and this software cannot be installed or removed, until it is available.")
    }
    NxBanner {
        visible: page.hasComponent && page.packageName === ""
        kind: "info"
        title: qsTr("No package named in the catalogue")
        message: qsTr("The AppStream metadata of this component does not say which DEB package provides it, so it cannot be installed from here.")
    }
    NxBanner {
        visible: pkg.state === "unknown"
        kind: "warning"
        title: qsTr("Not available for this system")
        message: qsTr("PackageKit finds no package named “%1” for the %2 architecture in the enabled software sources. The software source that provides it may be disabled, or the software lists are older than the catalogue.").arg(page.packageName).arg(page.systemArch)
        actionText: qsTr("Search all packages")
        onActionTriggered: page.searchRequested(page.packageName)
    }
    NxBanner {
        visible: pkg.state === "ready" && pkg.foreignOnly
        kind: "warning"
        title: qsTr("Not available for this architecture")
        message: qsTr("“%1” is only offered for %2; this system is %3.").arg(page.packageName).arg(pkg.candidateArch).arg(page.systemArch)
    }
    NxBanner {
        visible: pkg.state === "error"
        kind: "danger"
        title: qsTr("PackageKit could not look up this package")
        message: qsTr("PackageKit reported:") + "\n" + pkg.error
    }
    NxBanner {
        visible: pkg.state === "ready" && pkg.trust === "untrusted"
        kind: "danger"
        title: qsTr("This software cannot be verified")
        message: qsTr("At least one package needed to install it does not come from a signed software source: %1. Installing it requires an explicit confirmation.")
            .arg(page.names(page.changes.untrusted) || qsTr("see the details below"))
    }

    // ---- screenshots --------------------------------------------------------------------------
    ColumnLayout {
        Layout.fillWidth: true
        visible: page.hasComponent
        spacing: Theme.spaceSm

        ListView {
            id: shots
            Layout.fillWidth: true
            visible: page.screenshots.length > 0
            implicitHeight: 270
            orientation: ListView.Horizontal
            spacing: Theme.spaceMd
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: page.screenshots
            ScrollBar.horizontal: ScrollBar { }
            delegate: ColumnLayout {
                id: shotItem
                required property var modelData
                required property int index
                width: 420
                height: ListView.view.height - 12
                spacing: Theme.spaceXs
                Screenshot {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    shot: shotItem.modelData
                    active: page.active
                    onClicked: { viewer.shot = shotItem.modelData; viewer.open() }
                }
                PkText {
                    Layout.fillWidth: true
                    visible: (shotItem.modelData.caption || "") !== ""
                    text: shotItem.modelData.caption || ""
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSizeSmall
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignHCenter
                }
            }
        }
        Text {
            Layout.fillWidth: true
            visible: page.screenshots.length === 0
            text: qsTr("The catalogue has no screenshots for this application.")
            color: Theme.textMuted
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
        }
        Text {
            Layout.fillWidth: true
            visible: page.screenshots.length > 0
            text: qsTr("Screenshots are downloaded from the catalogue's media server when this page is opened.")
            color: Theme.textMuted
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
        }
    }

    // ---- description -------------------------------------------------------------------------
    ColumnLayout {
        Layout.fillWidth: true
        Layout.leftMargin: Theme.spaceXs
        Layout.rightMargin: Theme.spaceXs
        spacing: Theme.spaceSm
        visible: page.hasComponent ? (page.details.descriptionBlocks || []).length > 0 : pkg.description !== ""

        Repeater {
            model: page.hasComponent ? (page.details.descriptionBlocks || []) : []
            RowLayout {
                id: block
                required property var modelData
                Layout.fillWidth: true
                spacing: Theme.spaceSm
                Text {
                    visible: block.modelData.type === "li"
                    Layout.alignment: Qt.AlignTop
                    Layout.leftMargin: Theme.spaceMd
                    Layout.preferredWidth: 18
                    text: block.modelData.ordered ? block.modelData.number + "." : "•"
                    color: Theme.textSecondary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBodyLarge
                }
                Text {
                    Layout.fillWidth: true
                    // Produced by Catalog: escaped text with <i>…</i> as the only markup.
                    textFormat: Text.StyledText
                    text: block.modelData.text
                    color: Theme.textPrimary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBodyLarge
                    wrapMode: Text.WordWrap
                    lineHeight: 1.3
                }
            }
        }
        PkText {
            Layout.fillWidth: true
            visible: !page.hasComponent && pkg.description !== ""
            text: pkg.description
            font.pixelSize: Theme.fontSizeBodyLarge
            wrapMode: Text.WordWrap
            lineHeight: 1.3
        }
    }

    // ---- package ---------------------------------------------------------------------------------
    NxSection {
        title: qsTr("Package")
        description: pkg.state === "loading" ? qsTr("Asking PackageKit…")
            : pkg.state === "ready" ? qsTr("As reported by PackageKit for this system.")
            : qsTr("Package details come from PackageKit and are not available right now.")
        visible: page.packageName !== ""

        Fact { label: qsTr("Package name"); value: page.packageName; mono: true }
        Fact {
            visible: pkg.state === "ready"
            label: qsTr("Version available")
            value: pkg.candidateAvailable ? pkg.candidateVersion
                 : pkg.installed ? qsTr("%1 (installed; no other version on offer)").arg(pkg.installedVersion)
                 : qsTr("None for this architecture")
            mono: pkg.candidateAvailable
        }
        Fact {
            visible: pkg.state === "ready"
            label: qsTr("Installed version")
            value: pkg.installed ? pkg.installedVersion + (pkg.automatic ? "  " + qsTr("(installed as a dependency)") : "") : qsTr("Not installed")
            mono: pkg.installed
        }
        Fact {
            visible: pkg.state === "ready"
            label: qsTr("Origin repository")
            value: pkg.repo !== "" ? pkg.repoText + "  (" + pkg.repo + ")" : qsTr("Not reported by PackageKit")
            NxBadge {
                visible: pkg.repoCategory !== ""
                text: pkg.repoCategory === "debian" ? qsTr("Debian base") : pkg.repoCategory === "shadow" ? qsTr("SHADOW") : qsTr("Third party")
                kind: pkg.repoCategory === "third-party" ? "warning" : "accent"
                dot: false
            }
        }
        Fact {
            visible: pkg.state === "ready"
            label: qsTr("Architecture")
            value: page.archText()
            NxBadge {
                visible: pkg.archRelation === "foreign"
                text: qsTr("Foreign")
                kind: "warning"
            }
        }
        Fact {
            visible: pkg.state === "ready"
            label: qsTr("Download size")
            value: pkg.downloadSize >= 0 ? Theme.formatBytes(pkg.downloadSize) + "  " + qsTr("(this package alone)")
                 : pkg.candidateAvailable ? qsTr("Not reported by PackageKit") : qsTr("Nothing to download")
        }
        Fact {
            visible: pkg.state === "ready"
            label: qsTr("Installed size")
            value: pkg.installedSize >= 0 ? Theme.formatBytes(pkg.installedSize) : qsTr("Not reported by PackageKit")
        }
        Fact {
            visible: pkg.state === "ready"
            label: qsTr("Verification")
            value: pkg.simulationState === "running" ? qsTr("PackageKit is checking the signatures…")
                 : pkg.trust === "trusted" ? qsTr("Verified: every package of this installation comes from a signed software source")
                 : pkg.trust === "untrusted" ? qsTr("NOT verified: PackageKit cannot authenticate %1").arg(page.names(page.changes.untrusted) || qsTr("one or more packages"))
                 : pkg.installed && !pkg.candidateAvailable ? qsTr("Installed. PackageKit checks signatures when a package is downloaded; there is nothing to download now.")
                 : pkg.simulationState === "error" ? qsTr("Could not be checked (see Dependencies)")
                 : qsTr("Not checked")
            NxBadge {
                visible: pkg.trust !== "unknown"
                text: pkg.trust === "trusted" ? qsTr("Signed source") : qsTr("Not signed")
                kind: pkg.trust === "trusted" ? "success" : "danger"
            }
        }
    }

    // ---- dependencies ------------------------------------------------------------------------------
    NxSection {
        title: qsTr("Dependencies")
        description: qsTr("What this package depends on, and what PackageKit would do in a simulated transaction. Nothing has been changed.")
        visible: pkg.state === "ready" && (pkg.simulationState !== "idle" || pkg.dependsState !== "idle")

        Fact {
            visible: pkg.dependsState !== "idle"
            label: qsTr("Depends on")
            value: pkg.dependsState === "loading" ? qsTr("Asking PackageKit…")
                 : pkg.dependsState === "unsupported" ? qsTr("The PackageKit back-end on this system cannot list dependencies")
                 : pkg.dependsState === "error" ? pkg.dependsError
                 : pkg.depends.length === 0 ? qsTr("No other packages")
                 : page.dependencyNames(pkg.depends)
            NxBadge {
                visible: pkg.dependsState === "ready" && pkg.depends.length > 0
                text: qsTr("%1 packages, %2 installed").arg(pkg.depends.length).arg(page.installedCount(pkg.depends))
                kind: "neutral"
            }
        }

        Fact {
            visible: pkg.simulationState === "running"
            label: qsTr("Calculating")
            value: qsTr("PackageKit is resolving the dependencies…")
        }
        Fact {
            visible: pkg.simulationState === "error"
            label: qsTr("Not possible")
            value: pkg.simulationError
        }
        Fact {
            visible: pkg.simulationState === "ready"
            label: qsTr("Additional packages")
            value: (page.changes.additionalCount || 0) === 0
                ? qsTr("None — everything this needs is already installed")
                : qsTr("%1 will be installed or updated with it:").arg(page.changes.additionalCount) + "\n" + page.names(page.changes.additional)
        }
        Fact {
            visible: pkg.simulationState === "ready" && (page.changes.removeCount || 0) > 0
            label: qsTr("Will be REMOVED")
            value: page.names(page.changes.remove)
            NxBadge { text: qsTr("%1 removed").arg(page.changes.removeCount || 0); kind: "danger" }
        }
        Fact {
            visible: pkg.simulationState === "ready"
            label: qsTr("Total download")
            value: pkg.totalDownloadSize > 0 ? Theme.formatBytes(pkg.totalDownloadSize) : qsTr("Nothing to download")
        }
    }

    // ---- about -----------------------------------------------------------------------------------------
    NxSection {
        title: qsTr("About")
        visible: page.hasComponent || pkg.state === "ready"

        Fact {
            visible: page.hasComponent
            label: qsTr("Developer")
            value: page.hasComponent && (page.details.developer || "") !== "" ? page.details.developer : qsTr("Not named in the catalogue")
        }
        Fact {
            label: qsTr("Licence")
            value: page.hasComponent
                ? ((page.details.projectLicense || "") !== "" ? page.details.projectLicense : qsTr("Not stated in the catalogue"))
                : (pkg.license !== "" ? pkg.license : qsTr("Not reported by PackageKit"))
            mono: page.hasComponent && (page.details.projectLicense || "") !== ""
            NxBadge {
                visible: page.hasComponent && (page.details.projectLicense || "") !== ""
                text: page.hasComponent && page.details.licenseIsFree === true ? qsTr("Free licence") : qsTr("Not identified as free")
                kind: page.hasComponent && page.details.licenseIsFree === true ? "success" : "warning"
                dot: false
            }
            NxButton {
                visible: page.hasComponent && (page.details.licenseUrl || "") !== ""
                text: qsTr("Read")
                kind: "ghost"
                iconName: "external-link"
                onClicked: page.store.openUrl(page.details.licenseUrl)
            }
        }
        Repeater {
            model: [
                { key: "homepage", label: qsTr("Homepage") },
                { key: "bugtracker", label: qsTr("Bug tracker") },
                { key: "help", label: qsTr("Documentation") },
                { key: "vcs-browser", label: qsTr("Source code") },
                { key: "donation", label: qsTr("Donations") },
            ]
            Fact {
                id: link
                required property var modelData
                readonly property string url: page.urls[modelData.key] || ""
                visible: url !== ""
                label: modelData.label
                value: url
                NxButton {
                    text: qsTr("Open")
                    kind: "ghost"
                    iconName: "external-link"
                    onClicked: page.store.openUrl(link.url)
                }
            }
        }
        Fact {
            visible: !page.hasComponent && pkg.url !== ""
            label: qsTr("Homepage")
            value: pkg.url
            NxButton {
                text: qsTr("Open")
                kind: "ghost"
                iconName: "external-link"
                enabled: pkg.url.startsWith("http")
                onClicked: page.store.openUrl(pkg.url)
            }
        }
        Fact {
            visible: page.hasComponent
            label: qsTr("Catalogue ID")
            value: page.hasComponent ? page.details.id + "  (" + qsTr("from %1").arg(page.details.origin || qsTr("installed metadata")) + ")" : ""
            mono: true
        }
        Fact {
            visible: page.hasComponent && (page.details.appstreamCategories || []).length > 0
            label: qsTr("Categories")
            value: page.hasComponent ? (page.details.appstreamCategories || []).join(", ") : ""
        }
    }

    // ---- content rating and permissions -------------------------------------------------------------------
    NxSection {
        title: qsTr("Content rating and permissions")
        visible: page.hasComponent || page.fallbackPackage !== ""

        Repeater {
            model: page.hasComponent ? (page.details.contentRatings || []) : []
            Fact {
                required property var modelData
                label: qsTr("Content rating (%1)").arg(modelData.kind.toUpperCase())
                value: (modelData.minimumAge > 0 ? qsTr("Rated for ages %1 and up").arg(modelData.minimumAge)
                                                 : qsTr("No age restriction results from the publisher's declaration"))
                       + (modelData.attributes.length > 0
                          ? "\n" + modelData.attributes.map(a => "• " + (a.description !== "" ? a.description : a.id + ": " + a.value)).join("\n")
                          : "\n" + qsTr("The publisher declares no violence, drugs, sexual content, strong language, social features or purchases."))
            }
        }
        Fact {
            visible: !page.hasComponent || (page.details.contentRatings || []).length === 0
            label: qsTr("Content rating")
            value: page.hasComponent ? qsTr("The publisher provides no content rating in the catalogue.")
                                     : qsTr("Not available: this package has no catalogue entry.")
        }
        Fact {
            label: qsTr("Sandbox permissions")
            value: qsTr("Do not apply. This is a DEB package: it is not sandboxed and has no permission list. Once installed it runs with the full permissions of the user who starts it, and its installation scripts run as administrator.")
        }
    }

    // ---- releases -----------------------------------------------------------------------------------------
    NxSection {
        title: qsTr("Upstream releases")
        description: qsTr("From the application's AppStream metadata. The version offered here is the package version above.")
        visible: page.hasComponent && (page.details.releases || []).length > 0
        Repeater {
            model: page.hasComponent ? (page.details.releases || []) : []
            Fact {
                required property var modelData
                label: modelData.version + (modelData.dateText !== "" ? "\n" + modelData.dateText : "")
                value: modelData.description !== "" ? modelData.description : qsTr("No release notes in the catalogue.")
            }
        }
    }

    // ---- full-size screenshot ------------------------------------------------------------------------------
    Dialog {
        id: viewer
        property var shot: ({})
        title: (shot.caption || "") !== "" ? shot.caption : qsTr("Screenshot")
        width: Math.min(Overlay.overlay ? Overlay.overlay.width - Theme.spaceXxl * 2 : 900, 1100)
        height: Math.min(Overlay.overlay ? Overlay.overlay.height - Theme.spaceXxl * 2 : 640, 760)
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
        footer: null
        contentItem: ColumnLayout {
            spacing: Theme.spaceMd
            Screenshot {
                Layout.fillWidth: true
                Layout.fillHeight: true
                shot: viewer.shot
                fullSize: true
                active: viewer.visible
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                NxButton { text: qsTr("Close"); onClicked: viewer.close() }
            }
        }
    }
}
