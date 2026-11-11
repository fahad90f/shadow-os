// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui

// Shown instead of starting a transaction when PackageKit cannot be reached:
// says why, and gives the exact apt command that does the same thing.
Dialog {
    id: root

    property string kind: ""
    property string target: ""
    property string heading: ""
    readonly property string command: { arsenal.revision; return kind !== "" ? arsenal.aptCommand(kind, target) : "" }

    signal message(string text, string kind)

    function show(actionKind, actionTarget, actionHeading) {
        kind = actionKind
        target = actionTarget
        heading = actionHeading
        open()
    }

    modal: true
    title: heading
    width: Math.min(560, parent ? parent.width - Theme.spaceXxl : 560)
    anchors.centerIn: parent
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    footer: null

    contentItem: ColumnLayout {
        spacing: Theme.spaceLg

        NxBanner {
            Layout.fillWidth: true
            kind: "warning"
            title: arsenal.packageKit.checking ? qsTr("Still contacting PackageKit") : qsTr("PackageKit is not available")
            message: arsenal.packageKit.checking
                ? qsTr("The package service has not answered yet.")
                : qsTr("NEXUS Arsenal installs and removes software through the PackageKit system service, which cannot be reached: %1").arg(arsenal.packageKit.error)
        }
        Text {
            Layout.fillWidth: true
            text: root.command !== "" ? qsTr("You can do the same in a terminal. The command asks for your password and shows what it will change before it does anything:")
                                      : qsTr("There is nothing to do: no package of this selection is available and missing.")
            color: Theme.textSecondary
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeBody
            wrapMode: Text.WordWrap
        }
        CommandBox {
            visible: root.command !== ""
            command: root.command
            onCopied: root.message(qsTr("Command copied to the clipboard"), "success")
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceSm
            Item { Layout.fillWidth: true }
            NxButton {
                text: qsTr("Try PackageKit again")
                iconName: "refresh"
                busy: arsenal.packageKit.checking
                enabled: !arsenal.packageKit.checking
                onClicked: arsenal.packageKit.probe()
            }
            NxButton {
                visible: root.command !== ""
                text: qsTr("Copy command")
                iconName: "copy"
                kind: "primary"
                onClicked: {
                    arsenal.copyText(root.command)
                    root.message(qsTr("Command copied to the clipboard"), "success")
                }
            }
            NxButton { text: qsTr("Close"); onClicked: root.close() }
        }
    }
}
