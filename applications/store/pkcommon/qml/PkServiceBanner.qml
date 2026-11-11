// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.shadow.ui

// Shown wherever a view needs PackageKit and the daemon cannot be reached.
// It states exactly that, with the D-Bus error, and offers to ask again.
NxBanner {
    id: root

    // ShadowPk::Service
    required property QtObject service
    // What the user cannot do right now, e.g. "Software cannot be installed, removed or updated."
    property string consequence: qsTr("Software cannot be installed, removed or updated until it is available.")

    visible: service !== null && !service.available && !service.checking
    kind: "danger"
    title: qsTr("PackageKit service is not reachable")
    // Static sentence first: the Text behind NxBanner decides between plain and
    // rich text by looking at the first line only.
    message: consequence + "\n" + (service ? service.error : "")
    actionText: qsTr("Try again")
    onActionTriggered: service.probe()
}
