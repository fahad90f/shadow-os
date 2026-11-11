// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.nexus.ui

// Section header of the update list: one per group of NexusPk::UpdateChecker.
Item {
    id: root

    // The group key ListView passes to a section delegate.
    required property string section
    // NexusPk::UpdateChecker
    property QtObject checker: null

    readonly property int count: !checker ? 0
        : section === "security" ? checker.securityCount
        : section === "kernel" ? checker.kernelCount
        : section === "nexus" ? checker.nexusCount
        : section === "blocked" ? checker.blockedCount : checker.otherCount
    readonly property string title: section === "security" ? qsTr("Security updates")
        : section === "kernel" ? qsTr("Kernel")
        : section === "nexus" ? qsTr("NEXUS desktop and applications")
        : section === "blocked" ? qsTr("Held back") : qsTr("Other system packages")
    readonly property string note: section === "security" ? qsTr("Classified as security fixes by PackageKit (from the security archive).")
        : section === "blocked" ? qsTr("Kept back by the package manager: installing these would add or remove other packages. They are not installed from here.")
        : ""

    width: ListView.view ? ListView.view.rowWidth : implicitWidth
    implicitHeight: box.implicitHeight + Theme.spaceMd

    ColumnLayout {
        id: box
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: Theme.spaceXs
        spacing: 2
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceSm
            NxIcon {
                name: root.section === "security" ? "shield-alert" : root.section === "kernel" ? "cpu"
                    : root.section === "nexus" ? "nexus" : root.section === "blocked" ? "lock" : "package"
                size: Theme.iconSmall
                color: root.section === "security" ? Theme.danger : root.section === "blocked" ? Theme.warning : Theme.textSecondary
            }
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                text: root.title + "  (" + root.count + ")"
                color: Theme.textPrimary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBodyLarge
                font.weight: Theme.weightSemibold
                elide: Text.ElideRight
            }
        }
        Text {
            textFormat: Text.PlainText
            Layout.fillWidth: true
            visible: root.note !== ""
            text: root.note
            color: Theme.textMuted
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
            wrapMode: Text.WordWrap
        }
    }
}
