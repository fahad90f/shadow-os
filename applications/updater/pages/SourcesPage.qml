// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.updater

// The APT sources updates come from, and whether each one is signed.
NxPage {
    id: page

    required property QtObject updater
    readonly property var sources: updater.sources

    function inCategory(category) {
        return sources.filter(s => s.category === category)
    }
    readonly property var debianSources: inCategory("debian")
    readonly property var nexusSources: inCategory("nexus")
    readonly property var otherSources: inCategory("third-party")
    readonly property int unsignedCount: sources.filter(s => s.enabled && (s.verification === "unsigned" || s.verification === "signature-check-disabled")).length

    title: qsTr("Software sources")
    subtitle: qsTr("Updates are downloaded from these APT repositories. APT only accepts a repository whose index is signed by a key this system trusts; the state below is read from the sources files and the downloaded indexes.")
    actions: [
        NxIconButton {
            iconName: "refresh"
            tooltip: qsTr("Read the sources again")
            onClicked: page.updater.refreshSources()
        }
    ]

    NxBanner {
        visible: page.unsignedCount > 0
        kind: "danger"
        title: qsTr("%1 enabled source(s) are not verified").arg(page.unsignedCount)
        message: qsTr("Packages from an unsigned source cannot be authenticated. NEXUS Updater refuses to install them unless you explicitly allow it for a transaction.")
    }

    component SourceRows: Repeater {
        NxRow {
            id: sourceRow
            required property var modelData
            readonly property string verification: modelData.verification
            readonly property bool bad: verification === "unsigned" || verification === "signature-check-disabled"
            icon: verification === "signed" ? "shield-check" : bad ? "shield-off" : "shield"
            iconColor: verification === "signed" ? Theme.success : bad ? Theme.danger : Theme.textSecondary
            title: modelData.uri + "  " + modelData.suite
            subtitle: modelData.verificationText + "\n"
                + qsTr("Components: %1 · %2").arg(modelData.components.join(" ")).arg(modelData.file)
                + (modelData.signedBy !== "" ? "\n" + qsTr("Signed-By: %1").arg(modelData.signedBy) : "")
            NxBadge {
                text: sourceRow.verification === "signed" ? qsTr("Signed")
                    : sourceRow.verification === "unsigned" ? qsTr("Not signed")
                    : sourceRow.verification === "signature-check-disabled" ? qsTr("Check disabled")
                    : sourceRow.verification === "disabled" ? qsTr("Disabled") : qsTr("Not downloaded")
                kind: sourceRow.verification === "signed" ? "success" : sourceRow.bad ? "danger"
                    : sourceRow.verification === "disabled" ? "neutral" : "warning"
            }
        }
    }

    NxSection {
        title: qsTr("Debian base")
        description: qsTr("The Debian archive: the operating system and the applications packaged by Debian, including Debian's security updates.")
        SourceRows { model: page.debianSources }
        NxRow {
            visible: page.debianSources.length === 0
            icon: "info"
            title: page.updater.sourcesLoading ? qsTr("Reading the APT sources…") : qsTr("No Debian repository is configured")
            subtitle: page.updater.sourcesLoading ? "" : qsTr("Neither /etc/apt/sources.list nor /etc/apt/sources.list.d names a Debian archive.")
        }
    }

    NxSection {
        title: qsTr("NEXUS")
        description: qsTr("The NEXUS repository: the NEXUS HORIZON desktop and the NEXUS applications.")
        SourceRows { model: page.nexusSources }
        NxRow {
            visible: page.nexusSources.length === 0
            icon: "info"
            title: page.updater.sourcesLoading ? qsTr("Reading the APT sources…") : qsTr("No NEXUS repository is configured on this system")
            subtitle: page.updater.sourcesLoading ? "" : qsTr("NEXUS packages installed here are not updated from a repository.")
        }
    }

    NxSection {
        title: qsTr("Third-party sources")
        description: qsTr("Repositories that belong neither to Debian nor to NEXUS.")
        visible: page.otherSources.length > 0
        SourceRows { model: page.otherSources }
    }
}
