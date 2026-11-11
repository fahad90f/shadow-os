// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.controlcenter

NxPage {
    id: page
    title: qsTr("Storage")
    subtitle: qsTr("Disks, free space and what takes up room in your home folder.")

    property string removeDiskId: ""
    property string removeDiskName: ""

    function categoryColor(id) {
        switch (id) {
        case "documents": return Theme.accentBlue
        case "downloads": return Theme.accentCyan
        case "pictures": return Theme.accentEmerald
        case "music": return Theme.accentViolet
        case "videos": return Theme.accentRose
        case "desktop": return Theme.accentAmber
        case "appdata": return Theme.mix(Theme.accentBlue, Theme.textMuted, 0.6)
        case "caches": return Theme.mix(Theme.accentAmber, Theme.textMuted, 0.65)
        case "trash": return Theme.mix(Theme.accentRose, Theme.textMuted, 0.65)
        default: return Theme.textMuted
        }
    }
    function diskIcon(kind) {
        return kind === "usb" || kind === "removable" || kind === "card" ? "usb"
             : kind === "optical" ? "circle" : kind === "virtual" ? "layers" : "harddrive"
    }
    function kindText(kind) {
        switch (kind) {
        case "ssd": return qsTr("Solid-state drive")
        case "hdd": return qsTr("Hard disk")
        case "usb": return qsTr("USB drive")
        case "card": return qsTr("Memory card")
        case "optical": return qsTr("Optical drive")
        case "removable": return qsTr("Removable drive")
        case "virtual": return qsTr("Virtual device")
        default: return qsTr("Disk")
        }
    }
    function partitionText(p) {
        switch (p.kind) {
        case "swap": return qsTr("Swap space")
        case "encrypted": return qsTr("Encrypted (%1)").arg(p.fsType)
        case "lvm": return qsTr("LVM physical volume")
        case "raid": return qsTr("RAID member")
        case "filesystem": return p.fsType
        default: return qsTr("No known file system")
        }
    }

    StorageBackend {
        id: storage
        onFailed: (message) => page.ApplicationWindow.window.toaster.show(message, "danger")
        onDone: (message) => page.ApplicationWindow.window.toaster.show(message, "success")
        Component.onCompleted: startHomeScan()
    }

    NxBanner {
        visible: storage.ready && storage.source !== "udisks"
        kind: storage.source === "lsblk" ? "info" : "warning"
        title: storage.source === "lsblk" ? qsTr("Limited disk information") : qsTr("Disks cannot be listed")
        message: storage.statusMessage
    }

    // ---- mounted file systems -----------------------------------------------------
    NxSection {
        title: qsTr("Free space")
        description: qsTr("Mounted file systems, as reported by the kernel.")

        Repeater {
            model: storage.volumes
            Item {
                id: volume
                required property var modelData
                required property int index
                Layout.fillWidth: true
                implicitHeight: volumeColumn.implicitHeight + Theme.spaceMd * 2

                Rectangle {
                    visible: volume.index > 0
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: Theme.spaceLg
                    height: 1
                    color: Theme.borderSubtle
                }
                ColumnLayout {
                    id: volumeColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: Theme.spaceLg
                    anchors.rightMargin: Theme.spaceLg
                    spacing: Theme.spaceXs

                    NxMeter {
                        Layout.fillWidth: true
                        label: volume.modelData.name + "  —  " + volume.modelData.mountPoint
                        valueText: qsTr("%1 of %2 used").arg(Theme.formatBytes(volume.modelData.used)).arg(Theme.formatBytes(volume.modelData.total))
                        value: volume.modelData.fraction
                    }
                    Label {
                        Layout.fillWidth: true
                        text: [qsTr("%1 available").arg(Theme.formatBytes(volume.modelData.free)),
                               volume.modelData.device, volume.modelData.fsType,
                               volume.modelData.readOnly ? qsTr("read-only") : ""].filter(s => s !== "").join("  ·  ")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSizeSmall
                        elide: Text.ElideMiddle
                    }
                }
            }
        }
        NxEmptyState {
            visible: storage.volumes.length === 0
            Layout.fillWidth: true
            icon: "harddrive"
            title: qsTr("No mounted file systems were found")
        }
    }

    // ---- home folder -------------------------------------------------------------------
    NxSection {
        title: qsTr("Your home folder")
        description: storage.homePath
        headerActions: [
            NxButton {
                visible: storage.homeState === "scanning"
                text: qsTr("Cancel")
                onClicked: storage.cancelHomeScan()
            },
            NxButton {
                visible: storage.homeState !== "scanning"
                text: storage.homeState === "idle" ? qsTr("Measure") : qsTr("Measure again")
                iconName: "refresh"
                onClicked: storage.startHomeScan()
            }
        ]

        Item {
            Layout.fillWidth: true
            implicitHeight: homeColumn.implicitHeight + Theme.spaceLg * 2

            ColumnLayout {
                id: homeColumn
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: Theme.spaceLg
                spacing: Theme.spaceMd

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceSm
                    BusyIndicator {
                        visible: storage.homeState === "scanning"
                        running: visible
                        implicitWidth: 20
                        implicitHeight: 20
                        padding: 0
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: storage.homeState === "cancelled" ? Theme.warning : Theme.textSecondary
                        text: {
                            const files = Number(storage.homeFiles).toLocaleString(Qt.locale(), "f", 0)
                            const size = Theme.formatBytes(storage.homeBytes)
                            switch (storage.homeState) {
                            case "scanning": return qsTr("Measuring… %1 in %2 files so far").arg(size).arg(files)
                            case "done": return qsTr("%1 in %2 files").arg(size).arg(files)
                            case "cancelled": return qsTr("Cancelled — the figures cover only the %1 files (%2) measured before that.").arg(files).arg(size)
                            default: return qsTr("Not measured yet.")
                            }
                        }
                    }
                }

                // Stacked bar: one segment per kind, in proportion to its size.
                Rectangle {
                    Layout.fillWidth: true
                    visible: storage.homeBytes > 0
                    implicitHeight: 14
                    radius: 7
                    color: Theme.bgPressed
                    clip: true
                    Row {
                        anchors.fill: parent
                        Repeater {
                            model: storage.homeCategories
                            Rectangle {
                                required property var modelData
                                height: parent.height
                                width: storage.homeBytes > 0 ? parent.width * modelData.bytes / storage.homeBytes : 0
                                color: page.categoryColor(modelData.id)
                            }
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceXs
                    Repeater {
                        model: storage.homeCategories
                        RowLayout {
                            id: legend
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: Theme.spaceSm
                            Rectangle {
                                implicitWidth: 10
                                implicitHeight: 10
                                radius: 5
                                color: page.categoryColor(legend.modelData.id)
                            }
                            Label {
                                Layout.fillWidth: true
                                text: legend.modelData.title
                                elide: Text.ElideRight
                            }
                            Label {
                                text: qsTr("%1 files").arg(Number(legend.modelData.files).toLocaleString(Qt.locale(), "f", 0))
                                color: Theme.textMuted
                                font.pixelSize: Theme.fontSizeSmall
                            }
                            Label {
                                Layout.preferredWidth: 90
                                horizontalAlignment: Text.AlignRight
                                text: Theme.formatBytes(legend.modelData.bytes)
                                color: Theme.textSecondary
                                font.family: Theme.fontMono
                                font.pixelSize: Theme.fontSizeSmall
                            }
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: storage.homeUnreadable > 0
                    text: qsTr("%1 folders or files could not be read and are not included.").arg(storage.homeUnreadable)
                    color: Theme.warning
                    font.pixelSize: Theme.fontSizeSmall
                    wrapMode: Text.WordWrap
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Space really occupied on disk; other file systems mounted inside the folder are left out, and a file with several names is counted once.")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSizeSmall
                    wrapMode: Text.WordWrap
                }
            }
        }
    }

    // ---- disks ---------------------------------------------------------------------------
    Repeater {
        model: storage.disks
        NxSection {
            id: diskSection
            required property var modelData
            readonly property bool busy: storage.busyDevice === modelData.device
            title: modelData.name
            description: [page.kindText(modelData.kind), modelData.device,
                          modelData.size > 0 ? Theme.formatBytes(modelData.size) : "",
                          modelData.bus !== "" ? modelData.bus.toUpperCase() : ""].filter(s => s !== "").join("  ·  ")
            headerActions: [
                NxBadge {
                    visible: diskSection.modelData.smart.text !== ""
                    anchors.verticalCenter: parent.verticalCenter
                    text: diskSection.modelData.smart.state === "unknown"
                        ? qsTr("Health: %1").arg(diskSection.modelData.smart.text)
                        : diskSection.modelData.smart.text
                    kind: diskSection.modelData.smart.state === "ok" ? "success"
                        : diskSection.modelData.smart.state === "failing" ? "danger" : "neutral"
                },
                NxButton {
                    visible: diskSection.modelData.canRemoveSafely
                    text: qsTr("Remove safely")
                    iconName: "eject"
                    busy: diskSection.busy
                    enabled: storage.busyDevice === ""
                    onClicked: {
                        page.removeDiskId = diskSection.modelData.id
                        page.removeDiskName = diskSection.modelData.name
                        removeDialog.open()
                    }
                }
            ]

            NxRow {
                visible: diskSection.modelData.smart.state !== "unknown"
                icon: diskSection.modelData.smart.state === "ok" ? "check-circle" : "alert-triangle"
                iconColor: diskSection.modelData.smart.state === "ok" ? Theme.success : Theme.danger
                title: qsTr("Health (SMART)")
                subtitle: diskSection.modelData.smart.detail
                value: diskSection.modelData.smart.text
            }
            Repeater {
                model: diskSection.modelData.partitions
                NxRow {
                    id: partitionRow
                    required property var modelData
                    icon: modelData.kind === "encrypted" ? "disk-lock" : modelData.kind === "swap" ? "memory"
                        : modelData.depth > 0 ? "lock-open" : "database"
                    title: modelData.label !== "" ? modelData.label + "  —  " + modelData.device : modelData.device
                    subtitle: {
                        const parts = [page.partitionText(modelData), Theme.formatBytes(modelData.size)]
                        if (modelData.mountPoints.length > 0) parts.push(qsTr("mounted at %1").arg(modelData.mountPoints.join(", ")))
                        else if (modelData.kind === "swap") parts.push(modelData.mounted ? qsTr("in use") : qsTr("not in use"))
                        else if (modelData.kind === "filesystem") parts.push(qsTr("not mounted"))
                        if (modelData.usageKnown)
                            parts.push(qsTr("%1 available").arg(Theme.formatBytes(modelData.available)))
                        if (modelData.readOnly) parts.push(qsTr("read-only"))
                        return parts.join("  ·  ")
                    }
                    NxMeter {
                        visible: partitionRow.modelData.usageKnown
                        anchors.verticalCenter: parent.verticalCenter
                        width: 120
                        value: partitionRow.modelData.fraction
                        Accessible.name: qsTr("Usage of %1").arg(partitionRow.modelData.device)
                    }
                    NxButton {
                        visible: partitionRow.modelData.canMount
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Mount")
                        enabled: storage.busyDevice === ""
                        busy: storage.busyDevice === partitionRow.modelData.device
                        onClicked: storage.mount(partitionRow.modelData.objectPath)
                    }
                    NxButton {
                        visible: partitionRow.modelData.canUnmount
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Unmount")
                        enabled: storage.busyDevice === ""
                        busy: storage.busyDevice === partitionRow.modelData.device
                        onClicked: storage.unmount(partitionRow.modelData.objectPath)
                    }
                }
            }
            NxRow {
                visible: diskSection.modelData.partitions.length === 0
                icon: "info"
                title: diskSection.modelData.mediaAvailable ? qsTr("No partitions or file systems were found on this device")
                                                            : qsTr("No medium is inserted")
            }
        }
    }

    NxEmptyState {
        visible: storage.ready && storage.disks.length === 0 && storage.source !== "none"
        Layout.fillWidth: true
        icon: "harddrive"
        title: qsTr("No disks were found")
        message: storage.source === "udisks" ? qsTr("The disk service (UDisks2) reports no drives.")
                                             : qsTr("lsblk lists no block devices in this environment.")
    }

    NxSection {
        visible: storage.diskManagerAvailable
        NxRow {
            icon: "wrench"
            title: qsTr("Partition, format and check disks")
            subtitle: qsTr("Opens NEXUS Disk Manager.")
            chevron: true
            onClicked: storage.openDiskManager()
        }
    }

    NxConfirmDialog {
        id: removeDialog
        title: qsTr("Remove %1 safely?").arg(page.removeDiskName)
        message: qsTr("All file systems on the drive are unmounted, then the drive is ejected or powered off. Save your work on it first.")
        confirmText: qsTr("Remove safely")
        onConfirmed: storage.removeSafely(page.removeDiskId)
    }
}
