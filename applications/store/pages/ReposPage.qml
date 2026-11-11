// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.store

// The software sources (APT repositories): which ones exist, which belong to
// the Debian base and which to SHADOW, whether each is signed, and switches to
// enable or disable them through PackageKit.
NxPage {
    id: page

    required property QtObject store
    readonly property bool listed: store.reposState === "ready"
    readonly property bool canChange: store.service.available && !store.busy

    function inCategory(list, category) { return list.filter(r => r.category === category) }
    readonly property var debianRepos: inCategory(store.repos, "debian")
    readonly property var shadowRepos: inCategory(store.repos, "shadow")
    readonly property var otherRepos: inCategory(store.repos, "third-party")

    function toggle(repo, on) {
        if (on) {
            store.setRepoEnabled(repo.repoId, true, repo.description)
        } else {
            disableDialog.repo = repo
            disableDialog.open()
        }
    }

    title: qsTr("Repositories")
    subtitle: qsTr("Where software and updates come from. Shadow OS uses the Debian archive as its base and the SHADOW repository for the SHADOW desktop and applications.")
    actions: [
        NxButton {
            text: qsTr("Refresh software lists")
            iconName: "refresh"
            enabled: page.canChange
            onClicked: page.store.refreshMetadata()
        }
    ]

    PkServiceBanner {
        service: page.store.service
        consequence: qsTr("The sources below were read from the APT configuration; they cannot be switched on or off until PackageKit is available.")
    }
    NxBanner {
        visible: page.store.reposState === "error"
        kind: "danger"
        title: qsTr("PackageKit could not list the repositories")
        message: qsTr("PackageKit reported:") + "\n" + page.store.reposError
    }
    NxBanner {
        kind: "info"
        title: qsTr("Debian base and SHADOW")
        message: qsTr("Debian base repositories provide the operating system, most applications and Debian's security updates. The SHADOW repository provides the Shadow Horizon desktop and the SHADOW applications. Every repository must be signed with a key this system trusts; packages from an unsigned source are only installed after an explicit warning.")
    }

    // One repository as PackageKit lists it.
    component RepoRows: Repeater {
        id: rows
        property bool canChange: false
        signal toggled(var repo, bool on)
        NxRow {
            id: repoRow
            required property var modelData
            readonly property string verification: modelData.verification || "unknown"
            icon: verification === "signed" ? "shield-check" : verification === "unsigned" || verification === "signature-check-disabled" ? "shield-off" : "shield"
            iconColor: verification === "signed" ? Theme.success
                : verification === "unsigned" || verification === "signature-check-disabled" ? Theme.danger : Theme.textSecondary
            title: modelData.description !== "" ? modelData.description : modelData.repoId
            subtitle: (modelData.uri ? modelData.uri + "  " + modelData.suite + "\n" : "") + (modelData.verificationText || "")
            NxBadge {
                text: repoRow.verification === "signed" ? qsTr("Signed") : repoRow.verification === "unsigned" ? qsTr("Not signed")
                    : repoRow.verification === "signature-check-disabled" ? qsTr("Check disabled")
                    : repoRow.verification === "not-downloaded" ? qsTr("Not downloaded") : repoRow.verification === "disabled" ? qsTr("Disabled")
                    : qsTr("Unknown")
                kind: repoRow.verification === "signed" ? "success"
                    : repoRow.verification === "unsigned" || repoRow.verification === "signature-check-disabled" ? "danger"
                    : repoRow.verification === "not-downloaded" ? "warning" : "neutral"
            }
            Switch {
                checked: repoRow.modelData.enabled
                enabled: rows.canChange
                Accessible.name: qsTr("Use the repository %1").arg(repoRow.modelData.description)
                onToggled: {
                    const on = checked
                    // The switch shows the real state; it changes when PackageKit has changed it.
                    checked = Qt.binding(() => repoRow.modelData.enabled)
                    rows.toggled(repoRow.modelData, on)
                }
            }
        }
    }

    // One APT source as read from the configuration (PackageKit not reachable).
    component SourceRows: Repeater {
        NxRow {
            id: sourceRow
            required property var modelData
            readonly property string verification: modelData.verification
            icon: verification === "signed" ? "shield-check" : verification === "unsigned" || verification === "signature-check-disabled" ? "shield-off" : "shield"
            iconColor: verification === "signed" ? Theme.success
                : verification === "unsigned" || verification === "signature-check-disabled" ? Theme.danger : Theme.textSecondary
            title: modelData.uri + "  " + modelData.suite
            subtitle: modelData.verificationText + "\n" + qsTr("Components: %1 · %2").arg(modelData.components.join(" ")).arg(modelData.file)
            NxBadge { text: sourceRow.modelData.categoryText; dot: false }
            NxBadge {
                text: sourceRow.verification === "signed" ? qsTr("Signed") : sourceRow.verification === "unsigned" ? qsTr("Not signed")
                    : sourceRow.verification === "signature-check-disabled" ? qsTr("Check disabled")
                    : sourceRow.verification === "disabled" ? qsTr("Disabled") : qsTr("Not downloaded")
                kind: sourceRow.verification === "signed" ? "success"
                    : sourceRow.verification === "unsigned" || sourceRow.verification === "signature-check-disabled" ? "danger"
                    : sourceRow.verification === "disabled" ? "neutral" : "warning"
            }
        }
    }

    // ---- with PackageKit: grouped, switchable -------------------------------------
    NxSection {
        visible: page.listed
        title: qsTr("Debian base")
        description: qsTr("The Debian archive this system is built on. Disabling one of these stops software and security updates from it.")
        RepoRows { model: page.debianRepos; canChange: page.canChange; onToggled: (repo, on) => page.toggle(repo, on) }
        NxRow {
            visible: page.debianRepos.length === 0
            icon: "info"
            title: qsTr("PackageKit lists no Debian repository")
        }
    }
    NxSection {
        visible: page.listed
        title: qsTr("SHADOW")
        description: qsTr("The SHADOW desktop and applications.")
        RepoRows { model: page.shadowRepos; canChange: page.canChange; onToggled: (repo, on) => page.toggle(repo, on) }
        NxRow {
            visible: page.shadowRepos.length === 0
            icon: "info"
            title: qsTr("No SHADOW repository is configured on this system")
            subtitle: qsTr("SHADOW packages installed here are not updated from a repository.")
        }
    }
    NxSection {
        visible: page.listed && page.otherRepos.length > 0
        title: qsTr("Third-party repositories")
        description: qsTr("Sources that belong neither to Debian nor to SHADOW. Software from them is not reviewed by either project.")
        RepoRows { model: page.otherRepos; canChange: page.canChange; onToggled: (repo, on) => page.toggle(repo, on) }
    }

    // ---- without PackageKit: what the APT configuration says, read-only -------------
    NxSection {
        visible: !page.listed
        title: qsTr("Configured APT sources")
        description: page.store.reposState === "loading" ? qsTr("Reading…")
            : qsTr("Read from /etc/apt/sources.list and /etc/apt/sources.list.d (read-only).")
        SourceRows { model: page.store.sources }
        NxRow {
            visible: page.store.sources.length === 0
            icon: "info"
            title: page.store.reposState === "loading" ? qsTr("Reading the APT sources…") : qsTr("No APT source is configured")
        }
    }

    NxConfirmDialog {
        id: disableDialog
        property var repo: ({})
        title: qsTr("Disable this repository?")
        message: (repo.category === "debian"
                  ? qsTr("“%1” is a Debian base repository. While it is disabled, no software and no security updates are installed from it.")
                  : qsTr("While “%1” is disabled, no software and no updates are installed from it."))
                 .arg(repo.description || "")
                 + " " + qsTr("PackageKit asks for administrator authorisation to change the software sources.")
        confirmText: qsTr("Disable")
        destructive: true
        onConfirmed: page.store.setRepoEnabled(repo.repoId, false, repo.description)
    }
}
