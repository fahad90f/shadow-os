// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.files

// "Open with…": applications registered for the file's type first, every
// other installed application below, with a filter.
Dialog {
    id: dialog

    property url fileUrl
    property string mimeType: ""
    property string fileName: ""
    property var recommended: []
    property var others: []
    property string selectedId: ""

    readonly property var shown: {
        const needle = filter.text.trim().toLowerCase()
        const match = (a) => needle === "" || a.name.toLowerCase().indexOf(needle) >= 0
        const rec = dialog.recommended.filter(match).map(a => ({ id: a.id, name: a.name, icon: a.icon, isDefault: a.isDefault, group: qsTr("Recommended") }))
        const known = {}
        dialog.recommended.forEach(a => known[a.id] = true)
        const rest = dialog.others.filter(a => !known[a.id] && match(a)).map(a => ({ id: a.id, name: a.name, icon: a.icon, isDefault: false, group: qsTr("Other applications") }))
        return rec.concat(rest)
    }

    function show(url, mimeType, name) {
        dialog.fileUrl = url
        dialog.mimeType = mimeType
        dialog.fileName = name
        dialog.recommended = Launcher.applicationsFor(mimeType)
        dialog.others = Launcher.allApplications()
        dialog.selectedId = dialog.recommended.length > 0 ? dialog.recommended[0].id : ""
        filter.text = ""
        makeDefault.checked = false
        open()
    }

    function launch() {
        if (dialog.selectedId === "") return
        Launcher.openWith([dialog.fileUrl], dialog.selectedId, dialog.mimeType, makeDefault.checked)
        close()
    }

    title: qsTr("Open “%1” with").arg(dialog.fileName)
    width: Math.min(480, parent ? parent.width - Theme.spaceXxl : 480)
    height: Math.min(560, parent ? parent.height - Theme.spaceXxl : 560)
    closePolicy: Popup.CloseOnEscape
    footer: null
    onOpened: filter.forceActiveFocus()

    contentItem: ColumnLayout {
        spacing: Theme.spaceSm

        NxSearchField {
            id: filter
            Layout.fillWidth: true
            placeholderText: qsTr("Find an application")
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: Theme.radiusSm
            color: Theme.bgInput
            border.width: Theme.borderWidth
            border.color: Theme.border
            clip: true

            ListView {
                id: apps
                anchors.fill: parent
                anchors.margins: Theme.borderWidth
                model: dialog.shown
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { }
                Accessible.role: Accessible.List
                Accessible.name: qsTr("Applications")
                section.property: "group"
                section.delegate: Text {
                    required property string section
                    width: apps.width
                    topPadding: Theme.spaceSm
                    bottomPadding: Theme.spaceXs
                    leftPadding: Theme.spaceMd
                    text: section
                    color: Theme.textMuted
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeCaption
                    font.weight: Theme.weightSemibold
                    font.capitalization: Font.AllUppercase
                }

                delegate: Rectangle {
                    id: app
                    required property var modelData
                    width: apps.width
                    height: Theme.rowHeight - Theme.spaceXs
                    radius: Theme.radiusXs
                    color: dialog.selectedId === modelData.id ? Theme.accentFill
                        : appMouse.containsMouse ? Theme.hoverOverlay : "transparent"
                    Accessible.role: Accessible.ListItem
                    Accessible.name: modelData.name

                    MouseArea {
                        id: appMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: dialog.selectedId = app.modelData.id
                        onDoubleClicked: { dialog.selectedId = app.modelData.id; dialog.launch() }
                    }
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spaceMd
                        anchors.rightMargin: Theme.spaceMd
                        spacing: Theme.spaceMd
                        FileIcon {
                            name: app.modelData.icon
                            size: Theme.iconLarge
                        }
                        Text {
                            Layout.fillWidth: true
                            text: app.modelData.name
                            color: Theme.textPrimary
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeBody
                            elide: Text.ElideRight
                        }
                        NxBadge {
                            visible: app.modelData.isDefault
                            text: qsTr("Default")
                            kind: "accent"
                            dot: false
                        }
                    }
                }
            }

            NxEmptyState {
                anchors.fill: parent
                visible: apps.count === 0
                icon: "apps"
                title: filter.text.trim() !== "" ? qsTr("No application matches “%1”").arg(filter.text.trim())
                                                 : qsTr("No applications are installed")
                message: filter.text.trim() !== "" ? "" : qsTr("No launcher (.desktop) entries were found on this system.")
            }
        }

        CheckBox {
            id: makeDefault
            visible: dialog.mimeType !== "" && dialog.selectedId !== ""
            text: qsTr("Always use this application for %1 files").arg(dialog.mimeType)
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceSm
            Item { Layout.fillWidth: true }
            NxButton {
                text: qsTr("Cancel")
                onClicked: dialog.close()
            }
            NxButton {
                text: qsTr("Open")
                kind: "primary"
                enabled: dialog.selectedId !== ""
                onClicked: dialog.launch()
            }
        }
    }
}
