// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.networkcenter

// Shown in place of a feature that needs NetworkManager while it cannot be
// used: says which of the possible reasons applies and what the feature
// would have done.
NxEmptyState {
    id: root

    // What cannot be offered, e.g. "Wi-Fi networks are managed through NetworkManager."
    property string feature: ""

    icon: Nm.status === "unknown" ? "clock" : "plug"
    tone: Nm.status === "unknown" ? "neutral" : "warning"
    title: Nm.status === "unknown" ? qsTr("Contacting NetworkManager…")
        : Nm.status === "missing" ? qsTr("NetworkManager is not installed")
        : Nm.status === "not-running" ? qsTr("NetworkManager is not running")
        : qsTr("NetworkManager cannot be reached")
    message: Nm.status === "unknown" ? "" : [Nm.statusText, root.feature].filter(s => s !== "").join("\n\n")
    actionText: Nm.status === "unknown" ? "" : qsTr("Check again")
    onActionTriggered: Nm.refresh()
}
