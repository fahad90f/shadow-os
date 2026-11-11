// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.arsenal

// One profile: what it is, its state, install / remove, its tools grouped by
// category, and the well-known tools that are not packaged.
NxPage {
    id: page

    property string profileId: ""
    readonly property var profile: { arsenal.revision; return arsenal.profile(profileId) }
    readonly property var groups: { arsenal.revision; return arsenal.toolGroups(profileId, search.text, stateKeys[stateFilter.currentIndex], interfaceKeys[interfaceFilter.currentIndex]) }
    readonly property var external: arsenal.externalTools(profileId)
    readonly property var stateKeys: ["all", "installed", "missing"]
    readonly property var interfaceKeys: ["all", "gui", "cli"]
    readonly property int missing: (profile.toolCount || 0) - (profile.installedCount || 0) - (profile.unavailableCount || 0)

    signal toolOpened(string packageName)
    signal actionRequested(string kind, string target)
    signal message(string text, string kind)

    maxContentWidth: 960

    // ---- header -------------------------------------------------------------------
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spaceLg

        ProfileIcon { profile: page.profile; size: 72; Layout.alignment: Qt.AlignTop }
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceXs
            Text {
                Layout.fillWidth: true
                text: page.profile.name || ""
                color: Theme.textPrimary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeHeading
                font.weight: Theme.weightSemibold
                elide: Text.ElideRight
            }
            Text {
                Layout.fillWidth: true
                text: page.profile.description || ""
                color: Theme.textSecondary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
                wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                visible: (page.profile.includeNames || []).length > 0
                text: qsTr("Combines: %1").arg((page.profile.includeNames || []).join(", "))
                color: Theme.textMuted
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WordWrap
            }
        }
    }

    NxSection {
        title: qsTr("On this system")
        NxRow {
            icon: "package"
            title: qsTr("Tools installed")
            subtitle: (page.profile.unsupportedCount || 0) > 0
                ? (page.profile.unsupportedCount === 1 ? qsTr("1 tool of this profile is not built for %1 and is not counted").arg(arsenal.architecture) : qsTr("%1 tools of this profile are not built for %2 and are not counted").arg(page.profile.unsupportedCount).arg(arsenal.architecture)) : ""
            value: !arsenal.stateReady ? qsTr("Reading…")
                 : arsenal.dpkgError !== "" ? qsTr("Unknown")
                 : qsTr("%1 of %2").arg(page.profile.installedCount).arg(page.profile.toolCount)
        }
        NxRow {
            icon: "layers"
            title: qsTr("Profile package")
            subtitle: page.profile.metapackageInstalled ? qsTr("Installed; removing it also removes the tools it brought in that nothing else needs")
                    : page.profile.metapackageAvailable ? qsTr("Installs all tools of the profile and keeps them together")
                    : arsenal.stateReady ? qsTr("Not offered by the repositories enabled on this system (it is published in the NEXUS security repository)") : ""
            value: page.profile.metapackage || ""
            monoValue: true
        }
        NxRow {
            icon: "harddrive"
            title: qsTr("Still to install")
            subtitle: page.profile.sizeState === "error" ? (page.profile.sizeError || "")
                    : page.profile.sizeState === "ready" && page.profile.pendingPackages > 0
                        ? (page.profile.pendingPackages === 1 ? qsTr("1 package, as calculated by APT for this system") : qsTr("%1 packages including dependencies, as calculated by APT for this system").arg(page.profile.pendingPackages)) : ""
            value: !arsenal.stateReady || page.profile.sizeState === "pending" || page.profile.sizeState === "none" ? qsTr("Calculating…")
                 : page.profile.sizeState === "error" ? qsTr("Could not be calculated")
                 : page.profile.pendingPackages === 0 ? qsTr("Nothing")
                 : qsTr("%1 download · %2 on disk").arg(Theme.formatBytes(page.profile.downloadBytes)).arg(Theme.formatBytes(page.profile.installBytes))
        }
        NxRow {
            visible: (page.profile.unavailableCount || 0) > 0
            icon: "alert-triangle"
            iconColor: Theme.warning
            title: qsTr("Not in the configured repositories")
            subtitle: qsTr("These tools cannot be installed until the package lists offer them; refresh the lists with NEXUS Updater")
            value: (page.profile.unavailableCount === 1 ? qsTr("1 tool") : qsTr("%1 tools").arg(page.profile.unavailableCount))
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: Theme.spaceMd
            spacing: Theme.spaceSm
            Text {
                Layout.fillWidth: true
                text: page.profile.metapackageInstalled ? ""
                    : (page.profile.installedCount || 0) > 0 ? qsTr("Tools installed one by one are removed one by one, from their details.") : ""
                color: Theme.textMuted
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WordWrap
            }
            NxButton {
                visible: page.profile.metapackageInstalled === true
                text: qsTr("Remove profile…")
                iconName: "trash"
                kind: "danger"
                onClicked: page.actionRequested("remove-profile", page.profileId)
            }
            NxButton {
                visible: arsenal.stateReady && !page.profile.metapackageInstalled && (page.missing > 0 || page.profile.metapackageAvailable === true)
                text: page.profile.metapackageAvailable ? qsTr("Install profile…") : (page.missing === 1 ? qsTr("Install 1 missing tool…") : qsTr("Install %1 missing tools…").arg(page.missing))
                iconName: "download"
                kind: "primary"
                onClicked: page.actionRequested(page.profile.metapackageAvailable ? "install-profile" : "install-profile-tools", page.profileId)
            }
        }
    }

    // ---- filters ------------------------------------------------------------------
    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.spaceMd
        NxSearchField {
            id: search
            Layout.fillWidth: true
            placeholderText: qsTr("Filter the tools of this profile")
            Accessible.name: qsTr("Filter tools")
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
    }

    Repeater {
        model: page.groups
        ColumnLayout {
            id: group
            required property var modelData
            Layout.fillWidth: true
            spacing: Theme.spaceSm

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.spaceXs
                spacing: Theme.spaceSm
                NxIcon { name: group.modelData.icon; size: Theme.icon; color: Theme.textSecondary }
                Text {
                    text: group.modelData.name
                    color: Theme.textPrimary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBodyLarge
                    font.weight: Theme.weightSemibold
                }
                Text {
                    Layout.fillWidth: true
                    text: (group.modelData.tools.length === 1 ? qsTr("1 tool") : qsTr("%1 tools").arg(group.modelData.tools.length))
                    color: Theme.textMuted
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                }
            }
            ToolList {
                tools: group.modelData.tools
                onOpened: (name) => page.toolOpened(name)
                onInstallRequested: (name) => page.actionRequested("install-tool", name)
            }
        }
    }

    NxEmptyState {
        Layout.fillWidth: true
        visible: page.groups.length === 0
        icon: "search"
        title: qsTr("No tool matches")
        message: qsTr("No tool of this profile matches the filters.")
        actionText: qsTr("Clear filters")
        onActionTriggered: { search.text = ""; stateFilter.currentIndex = 0; interfaceFilter.currentIndex = 0 }
    }

    // ---- external tools -----------------------------------------------------------
    ColumnLayout {
        Layout.fillWidth: true
        visible: page.external.length > 0
        spacing: Theme.spaceMd

        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spaceXs
            spacing: Theme.spaceXxs
            Text {
                text: qsTr("Not installable from NEXUS repositories")
                color: Theme.textPrimary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBodyLarge
                font.weight: Theme.weightSemibold
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("Well-known tools of this field that Debian main does not carry. NEXUS Arsenal never downloads them; get them from their vendors.")
                color: Theme.textMuted
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WordWrap
            }
        }
        Repeater {
            model: page.external
            ExternalCard {
                required property var modelData
                entry: modelData
                onMessage: (text, kind) => page.message(text, kind)
            }
        }
    }
}
