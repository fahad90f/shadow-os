// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.arsenal

// The conditions of use, always available.
NxPage {
    id: page

    signal message(string text, string kind)

    title: qsTr("Responsible use")
    subtitle: qsTr("SHADOW Arsenal is for security professionals, students and developers doing authorised testing, research, defence and forensics.")
    maxContentWidth: 760

    NxCard {
        Layout.fillWidth: true
        implicitHeight: statement.implicitHeight + padding * 2
        UseStatement {
            id: statement
            anchors.left: parent.left
            anchors.right: parent.right
        }
    }

    NxSection {
        title: qsTr("Your acknowledgement")
        NxRow {
            icon: arsenal.acknowledged ? "check-circle" : "alert-triangle"
            iconColor: arsenal.acknowledged ? Theme.success : Theme.warning
            title: arsenal.acknowledged ? qsTr("You acknowledged these conditions") : qsTr("Not acknowledged yet")
            subtitle: qsTr("Stored for your user account only, in ~/.config/shadow/arsenal.conf")
            value: arsenal.acknowledgedAt
        }
    }

    NxSection {
        title: qsTr("Further reading")
        description: qsTr("Documents installed with SHADOW Arsenal")
        Repeater {
            model: [
                { file: "legal-use.md", icon: "file-text", title: qsTr("Legal and responsible use") },
                { file: "lab-setup.md", icon: "lab", title: qsTr("Building an isolated practice lab") },
                { file: "profiles.md", icon: "list", title: qsTr("What each profile contains") }
            ]
            NxRow {
                required property var modelData
                readonly property string path: arsenal.documentPath(modelData.file)
                icon: modelData.icon
                title: modelData.title
                subtitle: path !== "" ? path : qsTr("Not installed on this system")
                chevron: path !== ""
                Accessible.name: qsTr("Open %1").arg(modelData.title)
                onClicked: {
                    if (!arsenal.openDocument(modelData.file)) {
                        arsenal.copyText(path)
                        page.message(qsTr("No application could open the document. Its path was copied instead: %1").arg(path), "warning")
                    }
                }
            }
        }
    }
}
