// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.shadow.ui
import "Fmt.js" as Fmt

// Common frame of the pages that render one section of the report: refresh
// action, time of the check, and the loading / failed / unavailable states.
// Content declared by the page should be visible only while 'ready'.
NxPage {
    id: page

    required property var center
    property string sectionKey: ""
    property string sectionTitle: title
    readonly property var section: center.report[sectionKey]
    readonly property bool loaded: Fmt.has(section)
    readonly property bool ready: loaded && section.available !== false
    readonly property string checkedAt: center.sectionTimes[sectionKey] || ""

    maxContentWidth: 1040

    actions: [
        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: page.checkedAt !== ""
            text: qsTr("Checked %1").arg(page.center.localTime(page.checkedAt, false))
            color: Theme.textMuted
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
        },
        NxButton {
            text: qsTr("Refresh")
            iconName: "refresh"
            busy: page.center.loading
            onClicked: page.center.refresh()
        }
    ]

    NxBanner {
        visible: page.loaded && page.center.error !== ""
        kind: "warning"
        title: qsTr("The last refresh failed — this is the state at %1").arg(page.center.localTime(page.checkedAt, false))
        message: page.center.error
    }
    NxEmptyState {
        Layout.fillWidth: true
        visible: !page.loaded && page.center.loading
        icon: "refresh"
        title: qsTr("Reading the security state…")
        message: qsTr("shadow-security-status is checking this computer.")
    }
    NxEmptyState {
        Layout.fillWidth: true
        visible: !page.loaded && !page.center.loading && page.center.error !== ""
        icon: "shield-off"
        tone: "danger"
        title: qsTr("The security state could not be read")
        message: page.center.error
        actionText: qsTr("Try again")
        onActionTriggered: page.center.refresh()
    }
    NxEmptyState {
        Layout.fillWidth: true
        visible: page.loaded && !page.ready
        icon: "alert-triangle"
        tone: "warning"
        title: qsTr("%1 could not be determined").arg(page.sectionTitle)
        message: page.loaded && !page.ready ? (page.section.reason || qsTr("The status tool gave no reason.")) : ""
        actionText: qsTr("Check again")
        onActionTriggered: page.center.refresh()
    }
}
