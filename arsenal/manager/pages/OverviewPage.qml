// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.arsenal

// Profile cards. Counts and sizes come from dpkg and APT on this system.
NxPage {
    id: page

    signal profileOpened(string profileId)

    readonly property var profiles: arsenal.profiles

    title: qsTr("Security tool profiles")
    subtitle: arsenal.loadError !== "" ? ""
        : !arsenal.stateReady ? qsTr("Reading the package database…")
        : qsTr("%1 of %2 tools installed on this system (%3). Nothing is installed until you ask for it.")
              .arg(arsenal.installedTotal).arg(arsenal.toolTotal).arg(arsenal.architecture)
    maxContentWidth: 1120
    actions: [
        NxIconButton {
            iconName: "refresh"
            tooltip: qsTr("Read the package database again")
            enabled: arsenal.loadError === "" && !arsenal.refreshing
            onClicked: arsenal.refresh()
        }
    ]

    NxEmptyState {
        Layout.fillWidth: true
        Layout.topMargin: Theme.spaceXxl
        visible: arsenal.loadError !== ""
        icon: "package"
        tone: "warning"
        title: qsTr("The profile manifests are not available")
        message: arsenal.loadError
    }

    NxBanner {
        Layout.fillWidth: true
        visible: arsenal.loadError === "" && arsenal.loadWarnings.length > 0
        kind: "warning"
        title: qsTr("Some manifests could not be read")
        message: arsenal.loadWarnings.join("\n")
    }
    NxBanner {
        Layout.fillWidth: true
        visible: arsenal.loadError === "" && arsenal.dpkgError !== ""
        kind: "danger"
        title: qsTr("Installed state unknown")
        message: arsenal.dpkgError
    }
    NxBanner {
        Layout.fillWidth: true
        visible: arsenal.loadError === "" && arsenal.aptError !== ""
        kind: "warning"
        title: qsTr("Repository information unavailable")
        message: arsenal.aptError
    }
    NxBanner {
        Layout.fillWidth: true
        visible: arsenal.loadError === "" && !arsenal.packageKit.available && !arsenal.packageKit.checking
        kind: "warning"
        title: qsTr("PackageKit is not available — installing from here is not possible")
        message: qsTr("%1\nYou can still browse the profiles; each install button gives you the exact apt command instead.").arg(arsenal.packageKit.error)
        actionText: qsTr("Try again")
        onActionTriggered: arsenal.packageKit.probe()
    }

    GridLayout {
        Layout.fillWidth: true
        visible: arsenal.loadError === ""
        columns: Math.max(1, Math.floor((width + columnSpacing) / (300 + columnSpacing)))
        columnSpacing: Theme.spaceLg
        rowSpacing: Theme.spaceLg

        Repeater {
            model: page.profiles
            ProfileCard {
                required property var modelData
                profile: modelData
                Layout.fillWidth: true
                Layout.preferredWidth: 300
                Layout.alignment: Qt.AlignTop
                onOpened: (id) => page.profileOpened(id)
            }
        }
    }
}
