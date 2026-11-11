// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.controlcenter

NxPage {
    id: page
    title: qsTr("Startup Applications")
    subtitle: qsTr("Programs that are started automatically when you log in.")

    actions: [
        NxButton {
            text: qsTr("Add command…")
            iconName: "terminal"
            onClicked: commandDialog.open()
        },
        NxButton {
            text: qsTr("Add application…")
            kind: "primary"
            iconName: "plus"
            onClicked: { startup.loadApplications(); appDialog.open() }
        }
    ]

    readonly property var userEntries: startup.entries.filter(e => !e.system)
    readonly property var systemEntries: startup.entries.filter(e => e.system)
    property string removeId: ""
    property string removeName: ""

    StartupBackend {
        id: startup
        onFailed: (message) => page.ApplicationWindow.window.toaster.show(message, "danger")
    }

    // One autostart entry: application icon, name, command, switch, remove.
    component EntryRow: Item {
        id: row
        required property var modelData
        required property int index

        Layout.fillWidth: true
        implicitHeight: Math.max(Theme.rowHeightLarge, texts.implicitHeight + Theme.spaceMd * 2)

        Rectangle {
            visible: row.index > 0
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: Theme.spaceLg
            height: 1
            color: Theme.borderSubtle
        }
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: Theme.spaceLg
            anchors.rightMargin: Theme.spaceLg
            spacing: Theme.spaceMd

            NxAppIcon {
                name: row.modelData.icon !== "" ? row.modelData.icon : "application-x-executable"
                size: 32
                opacity: row.modelData.enabled ? 1 : 0.45
            }
            ColumnLayout {
                id: texts
                Layout.fillWidth: true
                spacing: 2
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceSm
                    Label {
                        Layout.fillWidth: true
                        Layout.maximumWidth: implicitWidth + 1
                        text: row.modelData.name
                        font.weight: Theme.weightMedium
                        color: row.modelData.enabled ? Theme.textPrimary : Theme.textMuted
                        elide: Text.ElideRight
                    }
                    NxBadge {
                        visible: row.modelData.system && !row.modelData.enabled && row.modelData.overridden
                        text: qsTr("Switched off for you")
                        kind: "neutral"
                        dot: false
                    }
                    NxBadge {
                        visible: row.modelData.condition !== ""
                        text: row.modelData.condition
                        kind: "warning"
                        dot: false
                    }
                    Item { Layout.fillWidth: true }
                }
                Label {
                    Layout.fillWidth: true
                    visible: row.modelData.comment !== ""
                    text: row.modelData.comment
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSizeSmall
                    elide: Text.ElideRight
                }
                Label {
                    Layout.fillWidth: true
                    text: row.modelData.command
                    color: Theme.textMuted
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fontSizeSmall
                    elide: Text.ElideMiddle
                }
            }
            Switch {
                checked: row.modelData.enabled
                Accessible.name: qsTr("Start %1 at login").arg(row.modelData.name)
                onToggled: {
                    const wanted = checked
                    if (!startup.setEnabled(row.modelData.id, wanted))
                        checked = Qt.binding(() => row.modelData.enabled)
                }
            }
            NxIconButton {
                visible: !row.modelData.system
                iconName: "trash"
                tooltip: qsTr("Remove %1 from the startup applications").arg(row.modelData.name)
                onClicked: {
                    page.removeId = row.modelData.id
                    page.removeName = row.modelData.name
                    removeDialog.open()
                }
            }
        }
    }

    NxSection {
        title: qsTr("Added by you")
        description: qsTr("Entries in %1.").arg(startup.userDirectory)

        Repeater {
            model: page.userEntries
            EntryRow { }
        }
        NxEmptyState {
            visible: page.userEntries.length === 0
            Layout.fillWidth: true
            icon: "play"
            title: qsTr("No startup applications of your own")
            message: qsTr("Add an installed application or a command to start it every time you log in.")
        }
    }

    NxSection {
        visible: page.systemEntries.length > 0
        title: qsTr("Installed with the system")
        description: qsTr("Entries in %1. They cannot be removed here, but each can be switched off for your account.")
                         .arg(startup.systemDirectories.join(", "))
        Repeater {
            model: page.systemEntries
            EntryRow { }
        }
    }

    NxConfirmDialog {
        id: removeDialog
        title: qsTr("Remove %1?").arg(page.removeName)
        message: qsTr("The entry is deleted from your startup applications. The program itself stays installed.")
        confirmText: qsTr("Remove")
        destructive: true
        onConfirmed: startup.remove(page.removeId)
    }

    // ---- add an installed application ---------------------------------------------
    Dialog {
        id: appDialog
        title: qsTr("Add an application")
        width: Math.min(520, page.width - Theme.spaceXxl)
        height: Math.min(600, page.height - Theme.spaceXxl)
        modal: true
        footer: null

        readonly property var matches: {
            const needle = appSearch.text.trim().toLowerCase()
            if (needle === "") return startup.applications
            return startup.applications.filter(a => a.name.toLowerCase().indexOf(needle) >= 0
                                                    || a.command.toLowerCase().indexOf(needle) >= 0
                                                    || a.comment.toLowerCase().indexOf(needle) >= 0)
        }
        onOpened: { appSearch.text = ""; appSearch.forceActiveFocus() }

        contentItem: ColumnLayout {
            spacing: Theme.spaceMd

            NxSearchField {
                id: appSearch
                Layout.fillWidth: true
                placeholderText: qsTr("Search installed applications")
            }
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: Theme.bgInput
                radius: Theme.radiusSm
                border.width: Theme.borderWidth
                border.color: Theme.border
                clip: true

                ListView {
                    id: appList
                    anchors.fill: parent
                    anchors.margins: Theme.spaceXs
                    model: appDialog.matches
                    boundsBehavior: Flickable.StopAtBounds
                    clip: true
                    ScrollBar.vertical: ScrollBar { }

                    delegate: ItemDelegate {
                        id: appDelegate
                        required property var modelData
                        width: ListView.view.width
                        height: Theme.rowHeightLarge
                        Accessible.name: modelData.name
                        onClicked: {
                            if (startup.addApplication(modelData.path)) {
                                page.ApplicationWindow.window.toaster.show(qsTr("%1 starts at login from now on").arg(modelData.name), "success")
                                appDialog.close()
                            }
                        }
                        contentItem: RowLayout {
                            spacing: Theme.spaceMd
                            NxAppIcon {
                                name: appDelegate.modelData.icon !== "" ? appDelegate.modelData.icon : "application-x-executable"
                                size: 28
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0
                                Label {
                                    Layout.fillWidth: true
                                    text: appDelegate.modelData.name
                                    font.weight: Theme.weightMedium
                                    elide: Text.ElideRight
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: appDelegate.modelData.comment !== "" ? appDelegate.modelData.comment : appDelegate.modelData.command
                                    color: Theme.textMuted
                                    font.pixelSize: Theme.fontSizeSmall
                                    elide: Text.ElideRight
                                }
                            }
                        }
                    }
                }
                Label {
                    anchors.centerIn: parent
                    width: parent.width - Theme.spaceXl * 2
                    visible: appList.count === 0
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    color: Theme.textMuted
                    text: startup.applications.length === 0 ? qsTr("No installed applications were found.")
                                                           : qsTr("No application matches “%1”.").arg(appSearch.text)
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                NxButton { text: qsTr("Close"); onClicked: appDialog.close() }
            }
        }
    }

    // ---- add a custom command --------------------------------------------------------
    Dialog {
        id: commandDialog
        title: qsTr("Add a command")
        width: Math.min(480, page.width - Theme.spaceXxl)
        modal: true
        footer: null

        property string problem: ""
        onOpened: { nameField.text = ""; commandField.text = ""; problem = ""; nameField.forceActiveFocus() }

        function submit() {
            problem = nameField.text.trim() === "" ? qsTr("Enter a name.") : startup.commandProblem(commandField.text)
            if (problem !== "") return
            if (startup.addCommand(nameField.text, commandField.text)) close()
        }

        contentItem: ColumnLayout {
            spacing: Theme.spaceMd

            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceXs
                Label { text: qsTr("Name"); color: Theme.textSecondary; font.pixelSize: Theme.fontSizeSmall }
                TextField {
                    id: nameField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Shown in this list")
                    Accessible.name: qsTr("Name")
                    onAccepted: commandField.forceActiveFocus()
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceXs
                Label { text: qsTr("Command"); color: Theme.textSecondary; font.pixelSize: Theme.fontSizeSmall }
                TextField {
                    id: commandField
                    Layout.fillWidth: true
                    font.family: Theme.fontMono
                    placeholderText: qsTr("Program and arguments")
                    Accessible.name: qsTr("Command")
                    onAccepted: commandDialog.submit()
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("The command is started directly, not through a shell: pipes, redirections and variables are not interpreted.")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSizeSmall
                    wrapMode: Text.WordWrap
                }
            }
            Label {
                Layout.fillWidth: true
                visible: commandDialog.problem !== ""
                text: commandDialog.problem
                color: Theme.danger
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spaceXs
                spacing: Theme.spaceSm
                Item { Layout.fillWidth: true }
                NxButton { text: qsTr("Cancel"); onClicked: commandDialog.close() }
                NxButton { text: qsTr("Add"); kind: "primary"; onClicked: commandDialog.submit() }
            }
        }
    }
}
