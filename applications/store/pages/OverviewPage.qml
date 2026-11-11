// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.store

// Landing page. Everything on it is a fact about this system: what the
// catalogue contains, what is installed, what can be updated, where software
// comes from, and the applications with the most recent upstream releases
// according to their AppStream metadata. Nothing is curated or sponsored.
NxPage {
    id: page

    required property QtObject store
    readonly property QtObject catalog: store.catalog
    readonly property QtObject service: store.service
    readonly property QtObject updates: store.updates

    readonly property var enabledSources: store.sources.filter(s => s.enabled)
    readonly property int unverifiedSources: enabledSources.filter(
        s => s.verification === "unsigned" || s.verification === "signature-check-disabled").length

    signal openCategory(string key)
    signal openApp(string id)
    signal openView(string view)

    title: qsTr("SHADOW Store")
    subtitle: qsTr("Applications and packages from the software sources configured on this system, installed as DEB packages through PackageKit.")
    maxContentWidth: 980
    actions: [
        NxButton {
            text: qsTr("Refresh software lists")
            iconName: "refresh"
            enabled: page.service.available && !page.store.busy
            onClicked: page.store.refreshMetadata()
        }
    ]

    PkServiceBanner {
        service: page.service
        consequence: qsTr("The catalogue can be browsed, but software cannot be installed, removed or updated until it is available.")
    }

    NxBanner {
        visible: page.catalog.state === "error"
        kind: "danger"
        title: qsTr("The application catalogue could not be loaded")
        message: qsTr("AppStream reported:") + "\n" + page.catalog.error
    }

    NxBanner {
        visible: page.catalog.state === "ready" && page.catalog.applicationCount === 0
        kind: "warning"
        title: qsTr("No application catalogue on this system yet")
        message: qsTr("AppStream knows %1 software components here, none of them an application. The catalogue is downloaded together with the software lists once the “appstream” package is installed. Packages can still be found with “All packages” in the search.").arg(page.catalog.componentCount)
        actionText: page.service.available && !page.store.busy ? qsTr("Refresh now") : ""
        onActionTriggered: page.store.refreshMetadata()
    }

    // ---- figures ----------------------------------------------------------------
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spaceLg

        NxStatTile {
            Layout.fillWidth: true
            label: qsTr("Applications available")
            icon: "apps"
            tone: "accent"
            loading: page.catalog.loading
            value: String(page.catalog.applicationCount)
            detail: page.catalog.loading ? qsTr("Loading the catalogue")
                : qsTr("of %1 components").arg(page.catalog.componentCount)
        }
        NxStatTile {
            Layout.fillWidth: true
            label: qsTr("Installed applications")
            icon: "package"
            loading: page.store.installedState === "loading" || page.catalog.loading
            value: page.store.installedState === "ready" ? String(page.store.installedAppCount) : "—"
            detail: page.store.installedState === "ready"
                ? qsTr("%1 packages installed").arg(page.store.installedPackageCount)
                : page.store.installedState === "error" ? qsTr("Could not be read") : qsTr("Reading")
        }
        NxStatTile {
            Layout.fillWidth: true
            label: qsTr("Updates available")
            icon: "arrow-up-circle"
            loading: page.updates.busy
            value: page.updates.state === "ready" ? String(page.updates.count) : "—"
            tone: page.updates.state !== "ready" ? "neutral"
                : page.updates.securityCount > 0 ? "danger" : page.updates.count > 0 ? "warning" : "success"
            detail: page.updates.state === "ready"
                ? (page.updates.securityCount > 0 ? qsTr("%1 security updates").arg(page.updates.securityCount)
                   : page.updates.count > 0 ? qsTr("No security updates among them") : qsTr("Up to date"))
                : page.updates.state === "error" ? qsTr("Not available") : qsTr("Not checked yet")
        }
        NxStatTile {
            Layout.fillWidth: true
            label: qsTr("Software sources")
            icon: page.unverifiedSources > 0 ? "shield-alert" : "shield-check"
            value: String(page.enabledSources.length)
            tone: page.unverifiedSources > 0 ? "danger" : page.enabledSources.length > 0 ? "success" : "neutral"
            detail: page.unverifiedSources > 0 ? qsTr("%1 not signed").arg(page.unverifiedSources)
                : page.enabledSources.length > 0 ? qsTr("All signed") : qsTr("None configured")
        }
    }

    // ---- categories ---------------------------------------------------------------
    ColumnLayout {
        Layout.fillWidth: true
        spacing: Theme.spaceSm

        Text {
            Layout.leftMargin: Theme.spaceXs
            text: qsTr("Categories")
            color: Theme.textPrimary
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeBodyLarge
            font.weight: Theme.weightSemibold
        }
        Text {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spaceXs
            visible: page.catalog.uncategorizedCount > 0
            text: qsTr("%1 applications carry none of these categories; they are found with the search.").arg(page.catalog.uncategorizedCount)
            color: Theme.textMuted
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
            wrapMode: Text.WordWrap
        }

        GridLayout {
            Layout.fillWidth: true
            columns: width > 760 ? 5 : width > 460 ? 3 : 2
            columnSpacing: Theme.spaceMd
            rowSpacing: Theme.spaceMd

            Repeater {
                model: page.catalog.categories
                NxCard {
                    id: card
                    required property var modelData
                    Layout.fillWidth: true
                    Layout.preferredWidth: 150
                    implicitHeight: 104
                    hoverable: true
                    color: cardTap.pressed ? Theme.bgPressed : card.hovered || card.activeFocus ? Theme.bgHover : Theme.bgRaised
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: modelData.title + ", " + qsTr("%1 applications").arg(modelData.count)
                    Keys.onReturnPressed: page.openCategory(modelData.key)
                    Keys.onSpacePressed: page.openCategory(modelData.key)

                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                    TapHandler { id: cardTap; onTapped: page.openCategory(card.modelData.key) }

                    ColumnLayout {
                        anchors.fill: parent
                        spacing: Theme.spaceXs
                        Rectangle {
                            implicitWidth: 36
                            implicitHeight: 36
                            radius: Theme.radiusSm
                            color: Theme.accentFill
                            NxIcon { anchors.centerIn: parent; name: card.modelData.icon; size: Theme.icon; color: Theme.accent }
                        }
                        Item { Layout.fillHeight: true }
                        Text {
                            Layout.fillWidth: true
                            text: card.modelData.title
                            color: Theme.textPrimary
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeBody
                            font.weight: Theme.weightSemibold
                            elide: Text.ElideRight
                        }
                        Text {
                            Layout.fillWidth: true
                            text: page.catalog.loading ? qsTr("Loading…")
                                : card.modelData.count === 1 ? qsTr("1 application") : qsTr("%1 applications").arg(card.modelData.count)
                            color: Theme.textMuted
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeSmall
                            elide: Text.ElideRight
                        }
                    }
                }
            }
        }
    }

    // ---- recently released -----------------------------------------------------------
    ColumnLayout {
        Layout.fillWidth: true
        visible: recent.count > 0
        spacing: Theme.spaceSm

        Text {
            Layout.leftMargin: Theme.spaceXs
            text: qsTr("Recently released")
            color: Theme.textPrimary
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeBodyLarge
            font.weight: Theme.weightSemibold
        }
        Text {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spaceXs
            text: qsTr("The applications in the catalogue with the newest upstream release dates in their AppStream metadata.")
            color: Theme.textMuted
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
            wrapMode: Text.WordWrap
        }
        Repeater {
            id: recent
            model: page.store.recentApps
            AppRow {
                Layout.fillWidth: true
                showReleaseDate: true
                onClicked: page.openApp(appId)
            }
        }
    }
}
