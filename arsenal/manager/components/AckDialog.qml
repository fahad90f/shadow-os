// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui

// First start: the conditions of use have to be acknowledged before the
// manager can be used. The answer is stored per user.
Dialog {
    id: root

    signal declined()

    modal: true
    title: qsTr("Before you use NEXUS Arsenal")
    width: Math.min(640, parent ? parent.width - Theme.spaceXxl : 640)
    height: Math.min(implicitHeight, parent ? parent.height - Theme.spaceXl : implicitHeight)
    anchors.centerIn: parent
    closePolicy: Popup.NoAutoClose
    footer: null

    contentItem: ColumnLayout {
        spacing: Theme.spaceLg

        Flickable {
            id: flick
            Layout.fillWidth: true
            Layout.fillHeight: true
            implicitHeight: statement.implicitHeight
            contentHeight: statement.implicitHeight
            contentWidth: width
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { }
            UseStatement {
                id: statement
                width: flick.width - (flick.contentHeight > flick.height ? Theme.spaceMd : 0)
            }
        }

        CheckBox {
            id: agree
            Layout.fillWidth: true
            text: qsTr("I have read this and will use the tools only where I am authorised to")
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceSm
            Item { Layout.fillWidth: true }
            NxButton {
                text: qsTr("Quit")
                onClicked: root.declined()
            }
            NxButton {
                text: qsTr("Continue")
                kind: "primary"
                enabled: agree.checked
                onClicked: {
                    arsenal.acknowledge()
                    root.close()
                }
            }
        }
    }
}
