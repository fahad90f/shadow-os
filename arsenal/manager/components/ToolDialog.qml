// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui

// Details of one tool: what it is, its state in the package database, how to
// start it, and install / remove.
Dialog {
    id: root

    property string packageName: ""
    readonly property var tool: { arsenal.revision; return packageName !== "" ? arsenal.tool(packageName) : ({}) }
    readonly property bool known: tool.package !== undefined

    signal actionRequested(string kind, string target)
    signal message(string text, string kind)
    signal profileRequested(string profileId)

    function show(name) {
        packageName = name
        open()
    }

    modal: true
    title: known ? tool.name : ""
    width: Math.min(640, parent ? parent.width - Theme.spaceXxl : 640)
    height: Math.min(implicitHeight, parent ? parent.height - Theme.spaceXxl : implicitHeight)
    anchors.centerIn: parent
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    footer: null

    contentItem: Flickable {
        id: flick
        implicitHeight: body.implicitHeight
        contentHeight: body.implicitHeight
        contentWidth: width
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { }

        ColumnLayout {
            id: body
            width: flick.width - (flick.contentHeight > flick.height ? Theme.spaceMd : 0)
            spacing: Theme.spaceLg

            Text {
                Layout.fillWidth: true
                text: root.known ? root.tool.summary : ""
                color: Theme.textSecondary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBodyLarge
                wrapMode: Text.WordWrap
            }

            Flow {
                Layout.fillWidth: true
                spacing: Theme.spaceSm
                NxBadge {
                    text: !root.known ? "" : root.tool.state === "installed" ? qsTr("Installed")
                        : root.tool.state === "available" ? qsTr("Not installed")
                        : root.tool.state === "unsupported" ? qsTr("Not built for %1").arg(arsenal.architecture)
                        : root.tool.state === "unavailable" ? qsTr("Not in the configured repositories")
                        : qsTr("State unknown")
                    kind: !root.known ? "neutral" : root.tool.state === "installed" ? "success"
                        : root.tool.state === "available" ? "neutral" : "warning"
                }
                NxBadge { text: root.known ? root.tool.categoryName : ""; kind: "neutral"; dot: false }
                NxBadge {
                    text: !root.known ? "" : root.tool.interface === "gui" ? qsTr("Graphical")
                        : root.tool.interface === "both" ? qsTr("Graphical and command line") : qsTr("Command line")
                    kind: "accent"; dot: false
                }
            }

            Text {
                Layout.fillWidth: true
                visible: root.known && root.tool.description !== ""
                text: root.known ? root.tool.description : ""
                color: Theme.textPrimary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
                wrapMode: Text.WordWrap
            }

            NxBanner {
                Layout.fillWidth: true
                visible: root.known && root.tool.notes !== ""
                kind: "info"
                title: qsTr("Good to know")
                message: root.known ? root.tool.notes : ""
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceXs
                NxKeyValue { Layout.fillWidth: true; label: qsTr("Package"); value: root.known ? root.tool.package : ""; mono: true }
                NxKeyValue {
                    Layout.fillWidth: true
                    label: qsTr("Installed version")
                    value: !root.known ? "" : root.tool.installed ? root.tool.installedVersion : qsTr("Not installed")
                    mono: root.known && root.tool.installed
                }
                NxKeyValue {
                    Layout.fillWidth: true
                    label: qsTr("Version in repositories")
                    value: !root.known ? "" : root.tool.candidateVersion !== "" ? root.tool.candidateVersion
                        : arsenal.aptError !== "" ? qsTr("Unknown (%1)").arg(arsenal.aptError) : qsTr("None — no enabled repository offers this package")
                    mono: root.known && root.tool.candidateVersion !== ""
                }
                NxKeyValue {
                    Layout.fillWidth: true
                    visible: root.known && root.tool.downloadBytes > 0 && !root.tool.installed
                    label: qsTr("Download size")
                    value: root.known ? qsTr("%1 (this package alone, without dependencies)").arg(Theme.formatBytes(root.tool.downloadBytes)) : ""
                }
                NxKeyValue {
                    Layout.fillWidth: true
                    visible: root.known && root.tool.installedBytes > 0
                    label: qsTr("Size on disk")
                    value: root.known ? Theme.formatBytes(root.tool.installedBytes) : ""
                }
                NxKeyValue {
                    Layout.fillWidth: true
                    visible: root.known && (root.tool.architectures || []).length > 0
                    label: qsTr("Architectures")
                    value: root.known ? (root.tool.architectures || []).join(", ") : ""
                }
                NxKeyValue { Layout.fillWidth: true; label: qsTr("Homepage"); value: root.known ? root.tool.homepage : ""; mono: true }
            }

            RowLayout {
                Layout.fillWidth: true
                visible: root.known && (root.tool.profiles || []).length > 0
                spacing: Theme.spaceSm
                Text {
                    text: qsTr("Part of")
                    color: Theme.textMuted
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: Theme.spaceXs
                    Repeater {
                        model: root.known ? root.tool.profiles : []
                        NxButton {
                            required property var modelData
                            text: modelData.name
                            kind: "ghost"
                            Accessible.name: qsTr("Show the %1 profile").arg(modelData.name)
                            onClicked: { root.close(); root.profileRequested(modelData.id) }
                        }
                    }
                }
            }

            // ---- how to start it ---------------------------------------------------
            ColumnLayout {
                Layout.fillWidth: true
                visible: root.known
                spacing: Theme.spaceSm

                Text {
                    text: qsTr("How to start")
                    color: Theme.textPrimary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBodyLarge
                    font.weight: Theme.weightSemibold
                }
                Text {
                    Layout.fillWidth: true
                    visible: root.known && !root.tool.installed
                    text: qsTr("Install the tool first.")
                    color: Theme.textMuted
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: root.known && root.tool.interface !== "cli"
                    spacing: Theme.spaceSm
                    Text {
                        Layout.fillWidth: true
                        text: !root.known ? "" : root.tool.canLaunch ? qsTr("Graphical interface, from its desktop entry %1.desktop — also in the application launcher.").arg(root.tool.desktopId)
                            : root.tool.installed ? qsTr("The desktop entry %1.desktop was not found on this system.").arg(root.tool.desktopId)
                            : qsTr("Graphical interface (desktop entry %1.desktop).").arg(root.tool.desktopId)
                        color: Theme.textSecondary
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeSmall
                        wrapMode: Text.WordWrap
                    }
                    NxButton {
                        visible: root.known && root.tool.canLaunch === true
                        text: qsTr("Open")
                        iconName: "play"
                        kind: "primary"
                        onClicked: {
                            const error = arsenal.launch(root.tool.package)
                            if (error !== "")
                                root.message(error, "danger")
                            else
                                root.message(qsTr("%1 was started").arg(root.tool.name), "success")
                        }
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: root.known && root.tool.interface !== "gui"
                    spacing: Theme.spaceSm
                    Text {
                        Layout.fillWidth: true
                        text: !root.known ? "" : root.tool.installed && !root.tool.canOpenTerminal
                                ? qsTr("The command “%1” was not found on this system.").arg(root.tool.command)
                                : qsTr("Command-line tool. Run it in a terminal; this prints its usage:")
                        color: Theme.textSecondary
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeSmall
                        wrapMode: Text.WordWrap
                    }
                    CommandBox {
                        command: root.known ? arsenal.terminalCommand(root.tool.package) : ""
                        onCopied: root.message(qsTr("Command copied to the clipboard"), "success")
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        visible: root.known && root.tool.canOpenTerminal === true
                        spacing: Theme.spaceSm
                        Text {
                            Layout.fillWidth: true
                            visible: !arsenal.terminalAvailable
                            text: qsTr("NEXUS Terminal (package nexus-terminal) is not installed, so the usage cannot be opened from here.")
                            color: Theme.textMuted
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeSmall
                            wrapMode: Text.WordWrap
                        }
                        Item { Layout.fillWidth: true; visible: arsenal.terminalAvailable }
                        NxButton {
                            visible: arsenal.terminalAvailable
                            text: qsTr("Show usage in terminal")
                            iconName: "terminal"
                            onClicked: {
                                const error = arsenal.openInTerminal(root.tool.package)
                                if (error !== "")
                                    root.message(error, "danger")
                            }
                        }
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                visible: root.known && root.tool.removeBlocked !== ""
                text: root.known ? root.tool.removeBlocked : ""
                color: Theme.textMuted
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WordWrap
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceSm
                Item { Layout.fillWidth: true }
                NxButton {
                    visible: root.known && root.tool.installed && root.tool.removeBlocked === ""
                    text: qsTr("Remove…")
                    iconName: "trash"
                    kind: "danger"
                    onClicked: root.actionRequested("remove-tool", root.tool.package)
                }
                NxButton {
                    visible: root.known && root.tool.state === "available"
                    text: qsTr("Install…")
                    iconName: "download"
                    kind: "primary"
                    onClicked: root.actionRequested("install-tool", root.tool.package)
                }
                NxButton {
                    text: qsTr("Close")
                    onClicked: root.close()
                }
            }
        }
    }
}
