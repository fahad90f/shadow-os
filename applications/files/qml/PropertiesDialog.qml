// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.files

// Properties of one item or of a selection: type, size (recursive for
// folders, computed in the background), location, dates, ownership, and the
// permission matrix, which is applied with KIO::chmod.
Dialog {
    id: dialog

    // The rwx bits as edited in the matrix (511 = 0777).
    property int pendingMode: 0
    readonly property bool single: info.count === 1
    readonly property bool modeChanged: single && info.mode >= 0 && pendingMode !== (info.mode & 511)

    function show(urls) {
        info.urls = []
        info.urls = urls
        tabs.currentIndex = 0
        recursive.checked = false
        applied.visible = false
        open()
    }

    title: info.count > 1 ? qsTr("Properties of %1").arg(info.name) : qsTr("Properties")
    width: Math.min(520, parent ? parent.width - Theme.spaceXxl : 520)
    closePolicy: Popup.CloseOnEscape
    footer: null
    onClosed: info.urls = []

    FileProperties {
        id: info
        computeSize: true
        onChanged: dialog.pendingMode = info.mode >= 0 ? (info.mode & 511) : 0
        onPermissionsApplied: applied.visible = true
    }

    contentItem: ColumnLayout {
        spacing: Theme.spaceLg

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceLg
            NxAppIcon {
                visible: info.iconName !== ""
                name: info.iconName
                size: Theme.iconXL * 2
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceXs
                Text {
                    Layout.fillWidth: true
                    text: info.name
                    color: Theme.textPrimary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeTitle
                    font.weight: Theme.weightSemibold
                    wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                    maximumLineCount: 2
                    elide: Text.ElideRight
                }
                Text {
                    Layout.fillWidth: true
                    text: info.loading ? qsTr("Reading…") : info.typeName
                    color: Theme.textSecondary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBody
                    elide: Text.ElideRight
                }
            }
        }

        NxBanner {
            visible: info.error !== ""
            kind: "danger"
            title: qsTr("The properties could not be read")
            message: info.error
        }

        TabBar {
            id: tabs
            Layout.fillWidth: true
            visible: dialog.single
            TabButton { text: qsTr("General"); width: implicitWidth }
            TabButton { text: qsTr("Permissions"); width: implicitWidth }
        }

        StackLayout {
            Layout.fillWidth: true
            currentIndex: dialog.single ? tabs.currentIndex : 0

            // ---- General ----
            ColumnLayout {
                spacing: Theme.spaceSm
                NxKeyValue { visible: dialog.single; label: qsTr("Type"); value: info.typeName }
                NxKeyValue { visible: dialog.single && info.mimeType !== ""; label: qsTr("MIME type"); value: info.mimeType; mono: true }
                NxKeyValue { label: qsTr("Location"); value: info.location }
                NxKeyValue { visible: info.isLink; label: qsTr("Link target"); value: info.linkTarget; mono: true }
                NxKeyValue {
                    visible: dialog.single && !info.isDir
                    label: qsTr("Size")
                    value: info.size >= 0 ? qsTr("%1 (%2 bytes)").arg(Theme.formatBytes(info.size)).arg(Number(info.size).toLocaleString(Qt.locale(), "f", 0)) : ""
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: info.isDir || info.count > 1
                    spacing: Theme.spaceMd
                    NxKeyValue {
                        Layout.fillWidth: true
                        label: info.count > 1 ? qsTr("Total size") : qsTr("Contents")
                        value: info.sizeBusy ? qsTr("Calculating…")
                            : info.sizeError !== "" ? info.sizeError : info.contentsText
                    }
                    BusyIndicator {
                        visible: info.sizeBusy
                        running: visible
                        implicitWidth: Theme.icon
                        implicitHeight: Theme.icon
                        padding: 0
                    }
                }
                NxDivider { Layout.fillWidth: true; visible: dialog.single }
                NxKeyValue { visible: dialog.single; label: qsTr("Modified"); value: info.modified }
                NxKeyValue { visible: dialog.single && info.created !== ""; label: qsTr("Created"); value: info.created }
                NxKeyValue { visible: dialog.single; label: qsTr("Accessed"); value: info.accessed }
            }

            // ---- Permissions ----
            ColumnLayout {
                spacing: Theme.spaceSm
                NxKeyValue { label: qsTr("Owner"); value: info.owner }
                NxKeyValue { label: qsTr("Group"); value: info.group }
                NxKeyValue { label: qsTr("Access"); value: info.permissionsText; mono: true }

                GridLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: Theme.spaceXs
                    columns: 4
                    columnSpacing: Theme.spaceLg
                    rowSpacing: Theme.spaceXs
                    enabled: info.canChangePermissions && !info.chmodBusy

                    Item { Layout.fillWidth: true }
                    Repeater {
                        model: [qsTr("Read"), qsTr("Write"), info.isDir ? qsTr("Enter") : qsTr("Execute")]
                        Text {
                            required property string modelData
                            Layout.alignment: Qt.AlignHCenter
                            text: modelData
                            color: Theme.textMuted
                            font.family: Theme.fontUi
                            font.pixelSize: Theme.fontSizeSmall
                            font.weight: Theme.weightSemibold
                        }
                    }

                    // Three rows (owner, group, others) of a label and r / w / x boxes.
                    Repeater {
                        model: 12
                        Item {
                            id: cell
                            required property int index
                            readonly property int who: Math.floor(index / 4)
                            readonly property int column: index % 4
                            readonly property int bit: column === 0 ? 0 : (4 >> (column - 1)) << (6 - 3 * who)
                            readonly property string whoLabel: [qsTr("Owner"), qsTr("Group"), qsTr("Others")][who]
                            Layout.fillWidth: column === 0
                            Layout.alignment: column === 0 ? Qt.AlignVCenter : Qt.AlignHCenter
                            implicitWidth: column === 0 ? whoText.implicitWidth : box.implicitWidth
                            implicitHeight: Math.max(whoText.implicitHeight, box.implicitHeight)
                            Text {
                                id: whoText
                                visible: cell.column === 0
                                anchors.verticalCenter: parent.verticalCenter
                                text: cell.whoLabel
                                color: Theme.textPrimary
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fontSizeBody
                            }
                            CheckBox {
                                id: box
                                visible: cell.column > 0
                                anchors.centerIn: parent
                                checked: (dialog.pendingMode & cell.bit) !== 0
                                Accessible.name: cell.whoLabel + ": " + [qsTr("read"), qsTr("write"), qsTr("execute")][Math.max(0, cell.column - 1)]
                                onToggled: {
                                    dialog.pendingMode = checked ? (dialog.pendingMode | cell.bit) : (dialog.pendingMode & ~cell.bit)
                                    applied.visible = false
                                }
                            }
                        }
                    }
                }

                CheckBox {
                    id: recursive
                    visible: dialog.single && info.isDir && info.canChangePermissions
                    enabled: !info.chmodBusy
                    text: qsTr("Also apply to everything inside this folder")
                    onToggled: applied.visible = false
                }
                NxBanner {
                    visible: info.permissionsNote !== ""
                    kind: "info"
                    message: info.permissionsNote
                }
                NxBanner {
                    visible: info.chmodError !== ""
                    kind: "danger"
                    title: qsTr("The permissions were not changed")
                    message: info.chmodError
                }
                NxBanner {
                    id: applied
                    visible: false
                    kind: "success"
                    message: qsTr("Permissions changed to %1.").arg(info.permissionsText)
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Theme.spaceXs
            spacing: Theme.spaceSm
            Item { Layout.fillWidth: true }
            NxButton {
                visible: dialog.single && tabs.currentIndex === 1 && info.canChangePermissions
                text: qsTr("Apply permissions")
                kind: "primary"
                busy: info.chmodBusy
                enabled: (dialog.modeChanged || (recursive.visible && recursive.checked)) && !info.chmodBusy
                onClicked: info.applyMode(dialog.pendingMode, recursive.visible && recursive.checked)
            }
            NxButton {
                text: qsTr("Close")
                onClicked: dialog.close()
            }
        }
    }
}
