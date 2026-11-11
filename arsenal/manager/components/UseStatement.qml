// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.nexus.ui

// The conditions of use, worded once and shown both in the first-start
// acknowledgement and on the permanent "Responsible use" page.
ColumnLayout {
    id: root

    spacing: Theme.spaceMd

    readonly property var points: [
        { icon: "check-circle", title: qsTr("Authorised testing only"),
          text: qsTr("Use these tools only on systems, networks and applications that you own, or that you have explicit written permission to test from someone entitled to give it. Agree the scope, the time window and the contact person in writing before you start, and stay inside it.") },
        { icon: "flag", title: qsTr("The law that applies to you"),
          text: qsTr("Accessing or disrupting computer systems without authorisation, intercepting other people's communications and bypassing access controls are criminal offences in most countries. Radio transmission and possession of some kinds of software are regulated in some. You are responsible for knowing and following the law where you are and where the target is.") },
        { icon: "lab", title: qsTr("Practise in a lab"),
          text: qsTr("Learn on an isolated practice environment — virtual machines on a host-only network — not on the internet or on a network you share with others. The documentation shipped with NEXUS Arsenal describes how to build one.") },
        { icon: "privacy", title: qsTr("Protect what you find"),
          text: qsTr("Data you come across during a test belongs to someone else. Collect only what the engagement needs, keep it encrypted, report weaknesses to the owner and delete the data when the work is done.") },
        { icon: "alert-triangle", title: qsTr("No warranty"),
          text: qsTr("The tools are free software from the Debian archive and come without any warranty. Several can interrupt services or damage data when used carelessly. Neither NEXUS OS nor the authors of the tools are liable for what you do with them.") }
    ]

    Repeater {
        model: root.points
        RowLayout {
            required property var modelData
            Layout.fillWidth: true
            spacing: Theme.spaceMd
            NxIcon {
                Layout.alignment: Qt.AlignTop
                Layout.topMargin: Theme.spaceXxs
                name: modelData.icon
                size: Theme.icon
                color: Theme.accent
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceXxs
                Text {
                    Layout.fillWidth: true
                    text: modelData.title
                    color: Theme.textPrimary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBody
                    font.weight: Theme.weightSemibold
                    wrapMode: Text.WordWrap
                }
                Text {
                    Layout.fillWidth: true
                    text: modelData.text
                    color: Theme.textSecondary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBody
                    wrapMode: Text.WordWrap
                }
            }
        }
    }
}
