// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui

NxWindow {
    id: window
    title: qsTr("Disk Manager")
    width: 1220
    height: 840
    minimumWidth: 980
    minimumHeight: 620

    property string selectedDevice: ""          // object path of the selected whole device
    property string selectedVolume: ""          // key of the selected volume / free space
    property var lastResult: null               // {operation, ok, message} of the last finished action

    readonly property var device: {
        const list = disks.devices
        for (let i = 0; i < list.length; ++i)
            if (list[i].path === selectedDevice) return list[i]
        return list.length > 0 ? list[0] : null
    }
    readonly property var volumes: device ? device.volumes : []
    readonly property var volume: {
        for (let i = 0; i < volumes.length; ++i)
            if (volumeKey(volumes[i]) === selectedVolume) return volumes[i]
        return volumes.length > 0 ? volumes[0] : null
    }
    readonly property var typeColors: [Theme.accentBlue, Theme.accentViolet, Theme.accentEmerald, Theme.accentAmber, Theme.accentCyan, Theme.accentRose]

    function volumeKey(v) { return v.free ? "free:" + v.offset : v.path }
    function shortName(dev) { return dev.replace("/dev/", "") }
    function contentText(v) {
        if (v.free) return qsTr("Free space")
        if (v.isContainer) return qsTr("Extended partition")
        if (v.encrypted && !v.unlocked) return qsTr("Encrypted (%1), locked").arg(v.containerType === "crypto_LUKS" ? "LUKS" : v.containerType)
        const base = v.isSwap || v.fsType === "swap" ? qsTr("Swap") : v.fsType !== "" ? v.fsType : qsTr("No file system detected")
        return v.encrypted ? qsTr("%1 (encrypted, unlocked)").arg(base) : base
    }
    function volumeLine(v) {
        let text = v.device + " — " + contentText(v)
        if (v.label !== "") text += " “" + v.label + "”"
        text += ", " + Theme.formatBytes(v.size)
        if (v.usageKnown) text += qsTr(" (%1 in use)").arg(Theme.formatBytes(v.fsUsed))
        if (v.mounted) text += qsTr(", mounted at %1").arg(v.mountPoints.join(", "))
        return text
    }
    // Lines for the destructive dialog: everything a whole device holds.
    function deviceLosses(d) {
        const lines = []
        for (let i = 0; i < d.volumes.length; ++i)
            if (!d.volumes[i].free) lines.push(volumeLine(d.volumes[i]))
        if (d.hasTable) lines.push(qsTr("the %1 partition table of %2").arg(tableName(d.tableType)).arg(d.device))
        if (lines.length === 0) lines.push(qsTr("all data on %1 (%2) — no file system or partition table was recognised, but anything stored there is overwritten").arg(d.device).arg(Theme.formatBytes(d.size)))
        return lines
    }
    function tableName(t) { return t === "gpt" ? "GPT" : t === "dos" ? "MBR" : t === "" ? qsTr("none") : t }
    function deviceDescription(d) {
        return [d.name, Theme.formatBytes(d.size), d.bus].filter(s => s !== "").join(" — ")
    }
    function nextFree(v) {
        for (let i = 0; i < volumes.length - 1; ++i)
            if (!volumes[i].free && volumes[i].path === v.path && volumes[i + 1].free
                    && Math.abs(volumes[i + 1].offset - (v.offset + v.size)) <= 2 * 1048576) return volumes[i + 1].size
        return 0
    }
    function caps(v) { const c = disks.capabilities[v.fsType]; return c ? c : ({}) }
    // Why destructive actions on this volume are disabled; "" when they are possible.
    function blocked(v) {
        if (!v || !device) return ""
        if (v.protectedReason !== "") return qsTr("%1. Formatting, deleting and overwriting are disabled for this disk.").arg(v.protectedReason)
        if (v.mounted) return qsTr("Mounted at %1 — unmount it before formatting or deleting.").arg(v.mountPoints.join(", "))
        if (v.swapActive) return qsTr("In use as swap space.")
        if (v.unlocked) return qsTr("Unlocked — lock it before formatting or deleting.")
        if (v.readOnly) return qsTr("The device is read-only.")
        return ""
    }
    function deviceBlocked(d) {
        if (!d) return ""
        if (d.protectedReason !== "") return qsTr("%1. A new partition table and restoring an image are disabled for this disk.").arg(d.protectedReason)
        if (d.mountPoints.length > 0) return qsTr("In use: %1 — unmount before replacing the partition table or restoring an image.").arg(d.mountPoints.join(", "))
        if (d.readOnly) return qsTr("The device is read-only.")
        return ""
    }
    function duration(seconds) { return seconds > 0 ? qsTr("about %1 left").arg(Theme.formatDuration(Math.ceil(seconds))) : "" }

    Connections {
        target: disks
        function onOperationFinished(operation, ok, message) {
            window.lastResult = { "operation": operation, "ok": ok, "message": message }
            window.toaster.show(ok ? (message !== "" ? operation + ": " + message : qsTr("%1: done").arg(operation))
                                   : qsTr("%1 failed").arg(operation), ok ? "success" : "danger")
        }
    }

    DestructiveDialog {
        id: destructive
        property string targetPath: ""
        formatTypes: disks.formatTypes
        onImagePathEdited: (path) => imageInfo = disks.imageFileInfo(path)
        onConfirmed: {
            if (action === "format") disks.format(targetPath, fsType, label)
            else if (action === "delete") disks.deletePartition(targetPath)
            else if (action === "table") disks.createPartitionTable(targetPath, tableType)
            else if (action === "restore") disks.restoreImage(targetPath, imageInfo.path)
        }
        function ask(what, path, dev, description, lossLines, size) {
            // The back-end decides; the dialog is not even shown for a protected disk.
            const refusal = disks.destructiveRefusal(path)
            if (refusal !== "") {
                window.lastResult = { "operation": title, "ok": false, "message": refusal }
                window.toaster.show(refusal, "danger")
                return
            }
            action = what; targetPath = path; device = dev; deviceDescription = description
            losses = lossLines; deviceSize = size; imageInfo = ({})
            present()
        }
    }

    ActionDialog {
        id: input
        property string targetPath: ""
        property double offset: 0
        formatTypes: disks.formatTypes
        onImagePathEdited: (path) => imageInfo = disks.imageFileInfo(path)
        onConfirmed: {
            if (mode === "unlock") disks.unlock(targetPath, passphrase)
            else if (mode === "label") disks.setLabel(targetPath, label)
            else if (mode === "resize") disks.resize(targetPath, sizeBytes)
            else if (mode === "create") disks.createPartition(targetPath, offset, sizeBytes, fsType, label)
            else if (mode === "image") disks.createImage(targetPath, imageInfo.path)
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        NxSidebar {
            Layout.fillHeight: true
            visible: disks.available && disks.devices.length > 0
            title: qsTr("Disks")
            currentKey: window.device ? window.device.path : ""
            model: disks.devices.map(d => ({
                "key": d.path,
                "title": d.name + " (" + window.shortName(d.device) + ")",
                "icon": d.isLoop ? "file" : d.bus === "USB" ? "usb" : d.removable ? "eject" : "harddrive",
                "section": !d.hasDrive ? qsTr("Other devices") : d.removable ? qsTr("Removable") : qsTr("Internal"),
                "badge": Theme.formatBytes(d.size)
            }))
            onActivated: (key) => { window.selectedDevice = key; window.selectedVolume = "" }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            NxEmptyState {
                anchors.fill: parent
                visible: !disks.loaded
                icon: "harddrive"
                title: qsTr("Looking for disks…")
            }
            NxEmptyState {
                anchors.fill: parent
                visible: disks.loaded && !disks.available
                icon: "harddrive"
                tone: "warning"
                title: qsTr("Disk management is unavailable")
                message: disks.unavailableReason
                actionText: qsTr("Try again")
                onActionTriggered: disks.reload()
            }
            NxEmptyState {
                anchors.fill: parent
                visible: disks.available && disks.devices.length === 0
                icon: "harddrive"
                title: qsTr("No disks found")
                message: qsTr("UDisks %1 reports no drives or other block devices.").arg(disks.version)
            }

            NxPage {
                id: page
                anchors.fill: parent
                visible: disks.available && window.device !== null
                maxContentWidth: 940
                title: window.device ? window.device.name : ""
                subtitle: window.device ? [window.device.device, Theme.formatBytes(window.device.size), window.device.bus,
                                           window.device.removable ? qsTr("removable") : ""].filter(s => s !== "").join(" · ") : ""
                actions: [
                    NxButton {
                        visible: window.device !== null && window.device.ejectable
                        text: qsTr("Eject")
                        iconName: "eject"
                        onClicked: disks.eject(window.device.drivePath)
                    },
                    NxButton {
                        visible: window.device !== null && window.device.canPowerOff
                        text: qsTr("Power off")
                        iconName: "power"
                        onClicked: disks.powerOff(window.device.drivePath)
                    }
                ]

                NxBanner {
                    visible: window.lastResult !== null && !window.lastResult.ok
                    kind: "danger"
                    title: window.lastResult ? qsTr("%1 failed").arg(window.lastResult.operation) : ""
                    message: window.lastResult ? window.lastResult.message : ""
                    dismissible: true
                    onDismissed: window.lastResult = null
                }
                NxBanner {
                    visible: window.device !== null && window.device.protectedReason !== ""
                    kind: "warning"
                    title: qsTr("This disk is used by the running system")
                    message: window.device ? qsTr("%1. Formatting, deleting partitions, a new partition table and restoring an image are refused for it.").arg(window.device.protectedReason) : ""
                }
                NxBanner {
                    visible: window.device !== null && window.device.hasDrive && !window.device.mediaAvailable
                    kind: "info"
                    title: qsTr("No medium")
                    message: qsTr("The drive is empty.")
                }

                // ---- running work ------------------------------------------------
                NxSection {
                    visible: disks.jobs.length > 0 || disks.imageJob.active === true
                    title: qsTr("Running")
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.margins: Theme.spaceLg
                        visible: disks.imageJob.active === true
                        spacing: Theme.spaceXs
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spaceMd
                            Label {
                                Layout.fillWidth: true
                                elide: Text.ElideMiddle
                                text: disks.imageJob.active === true ? disks.imageJob.label + "  ·  " + disks.imageJob.file : ""
                                color: Theme.textPrimary
                            }
                            NxButton {
                                text: disks.imageJob.cancelling === true ? qsTr("Cancelling…") : qsTr("Cancel")
                                enabled: disks.imageJob.cancelling !== true
                                onClicked: disks.cancelImageJob()
                            }
                        }
                        ProgressBar {
                            Layout.fillWidth: true
                            value: disks.imageJob.active === true ? disks.imageJob.progress : 0
                        }
                        Label {
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSizeSmall
                            text: disks.imageJob.active === true
                                ? qsTr("%1 of %2 · %3 · %4").arg(Theme.formatBytes(disks.imageJob.done)).arg(Theme.formatBytes(disks.imageJob.total))
                                      .arg(Theme.formatRate(disks.imageJob.rate)).arg(window.duration(disks.imageJob.secondsLeft)) : ""
                        }
                    }
                    Repeater {
                        model: disks.jobs
                        ColumnLayout {
                            id: jobItem
                            required property var modelData
                            Layout.fillWidth: true
                            Layout.margins: Theme.spaceLg
                            spacing: Theme.spaceXs
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spaceMd
                                Label {
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                    text: jobItem.modelData.label + (jobItem.modelData.targets.length > 0 ? "  ·  " + jobItem.modelData.targets.join(", ") : "")
                                    color: Theme.textPrimary
                                }
                                Label {
                                    visible: jobItem.modelData.progressValid
                                    text: Math.round(jobItem.modelData.progress * 100) + " %"
                                    color: Theme.textSecondary
                                    font.family: Theme.fontMono
                                }
                                NxButton {
                                    visible: jobItem.modelData.cancelable
                                    text: qsTr("Cancel")
                                    onClicked: disks.cancelJob(jobItem.modelData.path)
                                }
                            }
                            ProgressBar {
                                Layout.fillWidth: true
                                indeterminate: !jobItem.modelData.progressValid
                                value: jobItem.modelData.progress
                            }
                            Label {
                                visible: text !== ""
                                color: Theme.textMuted
                                font.pixelSize: Theme.fontSizeSmall
                                text: [jobItem.modelData.progressValid ? "" : qsTr("UDisks reports no progress figure for this operation"),
                                       jobItem.modelData.rate > 0 ? Theme.formatRate(jobItem.modelData.rate) : "",
                                       window.duration(jobItem.modelData.secondsLeft)].filter(s => s !== "").join(" · ")
                            }
                        }
                    }
                }

                // ---- partition map -----------------------------------------------
                NxSection {
                    title: qsTr("Volumes")
                    description: window.device === null ? "" : window.device.hasTable
                        ? qsTr("Partition table: %1").arg(window.tableName(window.device.tableType))
                        : qsTr("No partition table — the device is used as a whole")

                    Item {
                        id: map
                        Layout.fillWidth: true
                        Layout.margins: Theme.spaceLg
                        implicitHeight: 64
                        // Widths follow the sizes; very small volumes keep a minimum so they stay clickable.
                        readonly property var shown: window.volumes.filter(v => !v.isContainer)
                        readonly property var weights: {
                            let total = 0
                            for (let i = 0; i < shown.length; ++i) total += shown[i].size
                            const w = shown.map(v => Math.max(total > 0 ? v.size / total : 1, 0.07))
                            let sum = 0
                            for (let i = 0; i < w.length; ++i) sum += w[i]
                            return w.map(x => x / sum)
                        }
                        Row {
                            anchors.fill: parent
                            spacing: 2
                            Repeater {
                                model: map.shown
                                Rectangle {
                                    id: segment
                                    required property var modelData
                                    required property int index
                                    readonly property bool selected: window.volume !== null && window.volumeKey(window.volume) === window.volumeKey(modelData)
                                    readonly property color tone: window.typeColors[index % window.typeColors.length]
                                    width: Math.max(0, (map.width - 2 * (map.shown.length - 1)) * map.weights[index])
                                    height: map.height
                                    radius: Theme.radiusSm
                                    color: modelData.free ? Theme.bgInput : Theme.alpha(tone, selected ? 0.40 : 0.22)
                                    border.width: selected ? 2 : Theme.borderWidth
                                    border.color: selected ? Theme.accent : modelData.free ? Theme.border : Theme.alpha(tone, 0.6)
                                    activeFocusOnTab: true
                                    clip: true
                                    Accessible.role: Accessible.Button
                                    Accessible.name: modelData.free ? qsTr("Free space, %1").arg(Theme.formatBytes(modelData.size)) : window.volumeLine(modelData)
                                    Keys.onReturnPressed: window.selectedVolume = window.volumeKey(modelData)
                                    Keys.onSpacePressed: window.selectedVolume = window.volumeKey(modelData)

                                    // Used share of a mounted file system.
                                    Rectangle {
                                        visible: segment.modelData.usageKnown === true
                                        anchors.left: parent.left
                                        anchors.bottom: parent.bottom
                                        anchors.margins: 2
                                        height: 4
                                        radius: 2
                                        width: segment.modelData.usageKnown === true ? (parent.width - 4) * segment.modelData.fsUsed / segment.modelData.fsTotal : 0
                                        color: segment.tone
                                    }
                                    Column {
                                        anchors.fill: parent
                                        anchors.margins: Theme.spaceSm
                                        spacing: 2
                                        Text {
                                            width: parent.width
                                            elide: Text.ElideRight
                                            text: segment.modelData.free ? qsTr("Free") : window.shortName(segment.modelData.device)
                                            color: Theme.textPrimary
                                            font.family: Theme.fontUi
                                            font.pixelSize: Theme.fontSizeSmall
                                            font.weight: Theme.weightSemibold
                                        }
                                        Text {
                                            width: parent.width
                                            elide: Text.ElideRight
                                            text: Theme.formatBytes(segment.modelData.size) + (segment.modelData.free ? "" : " · " + window.contentText(segment.modelData))
                                            color: Theme.textSecondary
                                            font.family: Theme.fontUi
                                            font.pixelSize: Theme.fontSizeCaption
                                        }
                                    }
                                    Rectangle {
                                        anchors.fill: parent
                                        anchors.margins: -2
                                        radius: parent.radius + 2
                                        color: "transparent"
                                        border.width: Theme.focusRing
                                        border.color: Theme.accent
                                        visible: segment.activeFocus
                                    }
                                    TapHandler { onTapped: window.selectedVolume = window.volumeKey(segment.modelData) }
                                }
                            }
                        }
                    }

                    // ---- the selected volume -------------------------------------
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: Theme.spaceLg
                        Layout.rightMargin: Theme.spaceLg
                        Layout.bottomMargin: Theme.spaceLg
                        visible: window.volume !== null
                        spacing: Theme.spaceXs

                        NxKeyValue { label: qsTr("Device"); mono: true
                            value: !window.volume ? "" : window.volume.free ? qsTr("unallocated, starts at %1").arg(Theme.formatBytes(window.volume.offset)) : window.volume.device }
                        NxKeyValue { label: qsTr("Size"); value: window.volume ? Theme.formatBytes(window.volume.size) + qsTr(" (%1 bytes)").arg(window.volume.size) : "" }
                        NxKeyValue { visible: window.volume !== null && !window.volume.free; label: qsTr("Content"); value: window.volume && !window.volume.free ? window.contentText(window.volume) : "" }
                        NxKeyValue { visible: window.volume !== null && !window.volume.free; label: qsTr("Label"); value: window.volume && !window.volume.free && window.volume.label !== "" ? window.volume.label : "—" }
                        NxKeyValue { visible: window.volume !== null && !window.volume.free; label: qsTr("UUID"); mono: true; value: window.volume && !window.volume.free && window.volume.uuid !== "" ? window.volume.uuid : "—" }
                        NxKeyValue { visible: window.volume !== null && !window.volume.free && window.volume.unlocked; label: qsTr("Unlocked as"); mono: true; value: window.volume && window.volume.unlocked ? window.volume.cleartextDevice : "" }
                        NxKeyValue { visible: window.volume !== null && !window.volume.free; label: qsTr("Mounted at"); mono: true
                            value: !window.volume || window.volume.free ? "" : window.volume.mounted ? window.volume.mountPoints.join(", ")
                                 : window.volume.swapActive ? qsTr("active swap") : qsTr("not mounted") }
                        NxKeyValue { visible: window.volume !== null && window.volume.isPartition === true; label: qsTr("Partition")
                            value: window.volume && window.volume.isPartition ? qsTr("number %1, type %2%3").arg(window.volume.number).arg(window.volume.partitionType)
                                       .arg(window.volume.partitionName !== "" ? qsTr(", name “%1”").arg(window.volume.partitionName) : "") : "" }
                        NxMeter {
                            Layout.fillWidth: true
                            Layout.topMargin: Theme.spaceXs
                            visible: window.volume !== null && window.volume.usageKnown === true
                            label: qsTr("Usage")
                            value: window.volume && window.volume.usageKnown ? window.volume.fsUsed / window.volume.fsTotal : 0
                            valueText: window.volume && window.volume.usageKnown
                                ? qsTr("%1 used of %2 · %3 available").arg(Theme.formatBytes(window.volume.fsUsed)).arg(Theme.formatBytes(window.volume.fsTotal)).arg(Theme.formatBytes(window.volume.fsAvailable)) : ""
                        }
                        NxKeyValue {
                            visible: window.volume !== null && !window.volume.free && window.volume.hasFilesystem === true && window.volume.usageKnown !== true
                            label: qsTr("Usage"); value: qsTr("unknown while not mounted")
                        }

                        Flow {
                            Layout.fillWidth: true
                            Layout.topMargin: Theme.spaceMd
                            spacing: Theme.spaceSm
                            visible: window.volume !== null

                            readonly property var v: window.volume ? window.volume : ({})
                            readonly property bool busy: disks.pendingOperations > 0
                            readonly property bool locked: window.blocked(window.volume) !== ""

                            NxButton {
                                visible: parent.v.free === true
                                kind: "primary"; iconName: "plus"; text: qsTr("Create partition…")
                                enabled: !parent.busy
                                onClicked: {
                                    input.mode = "create"; input.targetPath = parent.v.tablePath; input.device = window.device.device
                                    input.offset = parent.v.offset; input.maxSize = parent.v.size; input.note = ""
                                    input.present()
                                }
                            }
                            NxButton {
                                visible: parent.v.free !== true && parent.v.hasFilesystem === true && !parent.v.mounted
                                kind: "primary"; text: qsTr("Mount"); enabled: !parent.busy
                                onClicked: disks.mount(parent.v.contentPath)
                            }
                            NxButton {
                                visible: parent.v.mounted === true
                                text: qsTr("Unmount"); iconName: "eject"; enabled: !parent.busy
                                onClicked: disks.unmount(parent.v.contentPath)
                            }
                            NxButton {
                                visible: parent.v.canUnlock === true && !parent.v.unlocked
                                kind: "primary"; text: qsTr("Unlock…"); iconName: "lock-open"; enabled: !parent.busy
                                onClicked: {
                                    input.mode = "unlock"; input.targetPath = parent.v.path; input.device = parent.v.device
                                    input.note = qsTr("Enter the passphrase of this encrypted volume. It is passed to the storage service and not kept.")
                                    input.present()
                                }
                            }
                            NxButton {
                                visible: parent.v.unlocked === true
                                text: qsTr("Lock"); iconName: "lock"; enabled: !parent.busy && !parent.v.mounted
                                onClicked: disks.lock(parent.v.path)
                            }
                            NxButton {
                                visible: parent.v.hasFilesystem === true
                                text: qsTr("Change label…"); enabled: !parent.busy
                                onClicked: {
                                    input.mode = "label"; input.targetPath = parent.v.contentPath; input.device = parent.v.device
                                    input.currentLabel = parent.v.label; input.note = ""
                                    input.present()
                                }
                            }
                            NxButton {
                                visible: parent.v.hasFilesystem === true && window.caps(parent.v).check === true
                                text: qsTr("Check"); enabled: !parent.busy && !parent.v.mounted
                                onClicked: disks.check(parent.v.contentPath)
                            }
                            NxButton {
                                visible: parent.v.hasFilesystem === true && window.caps(parent.v).repair === true
                                text: qsTr("Repair"); iconName: "wrench"; enabled: !parent.busy && !parent.v.mounted
                                onClicked: disks.repair(parent.v.contentPath)
                            }
                            NxButton {
                                readonly property bool grow: !parent.v.hasFilesystem || window.caps(parent.v).offlineGrow === true
                                readonly property bool shrink: !parent.v.hasFilesystem || window.caps(parent.v).offlineShrink === true
                                visible: parent.v.isPartition === true && !parent.v.isContainer && !parent.v.encrypted && (grow || shrink)
                                text: qsTr("Resize…"); enabled: !parent.busy && !parent.v.mounted && !parent.v.swapActive
                                onClicked: {
                                    input.mode = "resize"; input.targetPath = parent.v.path; input.device = parent.v.device
                                    input.currentSize = parent.v.size
                                    input.minSize = shrink ? 1048576 : parent.v.size
                                    input.maxSize = grow ? parent.v.size + window.nextFree(parent.v) : parent.v.size
                                    input.note = parent.v.hasFilesystem
                                        ? qsTr("The %1 file system is resized together with the partition. How far it can shrink depends on the data it holds; the tool refuses a size that is too small.").arg(parent.v.fsType)
                                        : qsTr("The partition holds no file system UDisks can resize; only the partition boundary changes.")
                                    input.present()
                                }
                            }
                            NxButton {
                                visible: parent.v.free !== true && !parent.v.isContainer
                                kind: "danger"; text: qsTr("Format…"); enabled: !parent.busy && !parent.locked
                                onClicked: destructive.ask("format", parent.v.path, parent.v.device, window.deviceDescription(window.device),
                                                           [window.volumeLine(parent.v)], parent.v.size)
                            }
                            NxButton {
                                visible: parent.v.isPartition === true
                                kind: "danger"; iconName: "trash"; text: qsTr("Delete…"); enabled: !parent.busy && !parent.locked
                                onClicked: destructive.ask("delete", parent.v.path, parent.v.device, window.deviceDescription(window.device),
                                                           [window.volumeLine(parent.v)], parent.v.size)
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: text !== ""
                            wrapMode: Text.WordWrap
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSizeSmall
                            text: window.volume && !window.volume.free ? window.blocked(window.volume) : ""
                        }
                    }
                }

                // ---- the drive ---------------------------------------------------
                NxSection {
                    title: qsTr("Drive")
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.margins: Theme.spaceLg
                        spacing: Theme.spaceXs
                        NxKeyValue { label: qsTr("Model"); value: window.device ? window.device.name : "" }
                        NxKeyValue { visible: window.device !== null && window.device.isLoop; label: qsTr("Backing file"); mono: true; value: window.device && window.device.isLoop ? window.device.backingFile : "" }
                        NxKeyValue { visible: window.device !== null && window.device.hasDrive; label: qsTr("Serial number"); mono: true; value: window.device && window.device.serial !== "" ? window.device.serial : "—" }
                        NxKeyValue { visible: window.device !== null && window.device.hasDrive; label: qsTr("Firmware"); mono: true; value: window.device && window.device.revision !== "" ? window.device.revision : "—" }
                        NxKeyValue { label: qsTr("Connection"); value: window.device && window.device.bus !== "" ? window.device.bus : "—" }
                        NxKeyValue { visible: window.device !== null && window.device.hasDrive; label: qsTr("Type")
                            value: !window.device ? "" : window.device.rotationRate === 0 ? qsTr("Solid state (no rotation)")
                                 : window.device.rotationRate > 0 ? qsTr("Hard disk, %1 rpm").arg(window.device.rotationRate) : qsTr("Rotating medium") }
                        NxKeyValue { label: qsTr("Removable"); value: window.device && window.device.removable ? qsTr("Yes") : qsTr("No") }
                        NxKeyValue { label: qsTr("Capacity"); value: window.device ? Theme.formatBytes(window.device.size) + qsTr(" (%1 bytes)").arg(window.device.size) : "" }
                        NxKeyValue { label: qsTr("Partitioning"); value: window.device ? window.tableName(window.device.tableType) : "" }

                        Flow {
                            Layout.fillWidth: true
                            Layout.topMargin: Theme.spaceMd
                            spacing: Theme.spaceSm
                            readonly property bool busy: disks.pendingOperations > 0 || disks.imageJob.active === true
                            readonly property bool locked: window.deviceBlocked(window.device) !== ""
                            NxButton {
                                text: qsTr("Create image…"); iconName: "download"
                                enabled: !parent.busy && window.device !== null && window.device.size > 0
                                onClicked: {
                                    input.mode = "image"; input.targetPath = window.device.path; input.device = window.device.device
                                    input.currentSize = window.device.size
                                    input.defaultPath = disks.defaultImagePath(window.device.kernelName)
                                    input.note = qsTr("Copies the whole device, byte for byte, into a file. For a consistent copy, unmount its file systems first.")
                                    input.imageInfo = disks.imageFileInfo(input.defaultPath)
                                    input.present()
                                }
                            }
                            NxButton {
                                kind: "danger"; text: qsTr("Restore image…"); iconName: "upload"
                                enabled: !parent.busy && !parent.locked && window.device !== null && window.device.size > 0
                                onClicked: destructive.ask("restore", window.device.path, window.device.device, window.deviceDescription(window.device),
                                                           window.deviceLosses(window.device), window.device.size)
                            }
                            NxButton {
                                kind: "danger"; text: qsTr("New partition table…")
                                enabled: !parent.busy && !parent.locked && window.device !== null && window.device.size > 0
                                onClicked: destructive.ask("table", window.device.path, window.device.device, window.deviceDescription(window.device),
                                                           window.deviceLosses(window.device), window.device.size)
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: text !== ""
                            wrapMode: Text.WordWrap
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSizeSmall
                            text: window.deviceBlocked(window.device)
                        }
                    }
                }

                // ---- health --------------------------------------------------------
                NxSection {
                    id: health
                    readonly property var smart: window.device ? window.device.smart : ({})
                    title: qsTr("Health (SMART)")
                    headerActions: [
                        NxIconButton {
                            visible: health.smart.supported === true
                            iconName: "refresh"
                            tooltip: qsTr("Read SMART data from the drive now")
                            onClicked: disks.refreshSmart(window.device.drivePath)
                        }
                    ]
                    NxRow {
                        visible: health.smart.supported !== true
                        icon: "info"
                        title: window.device !== null && !window.device.hasDrive ? qsTr("Not a physical drive") : qsTr("No SMART data")
                        subtitle: window.device !== null && !window.device.hasDrive
                            ? qsTr("SMART health data exists only for physical drives.")
                            : qsTr("UDisks reports no SMART support for this drive (not an ATA or NVMe device, or its bridge does not pass SMART commands).")
                    }
                    NxRow {
                        visible: health.smart.supported === true && health.smart.enabled !== true
                        icon: "info"
                        title: qsTr("SMART is switched off on this drive")
                    }
                    NxRow {
                        visible: health.smart.supported === true && health.smart.enabled === true && health.smart.hasData !== true
                        icon: "info"
                        title: qsTr("No SMART data read yet")
                        subtitle: qsTr("Use the refresh button to read it from the drive.")
                    }
                    NxRow {
                        visible: health.smart.hasData === true
                        icon: health.smart.failing === true ? "alert-triangle" : "check-circle"
                        iconColor: health.smart.failing === true ? Theme.danger : Theme.success
                        title: qsTr("Overall assessment")
                        subtitle: health.smart.updated > 0 ? qsTr("Read %1").arg(new Date(health.smart.updated * 1000).toLocaleString(Qt.locale(), Locale.ShortFormat)) : ""
                        NxBadge {
                            text: health.smart.failing === true ? qsTr("Drive reports failure") : qsTr("Passed")
                            kind: health.smart.failing === true ? "danger" : "success"
                        }
                    }
                    NxRow {
                        visible: health.smart.hasData === true && health.smart.temperatureKnown === true
                        icon: "thermometer"; title: qsTr("Temperature")
                        value: health.smart.temperatureKnown === true ? qsTr("%1 °C").arg(Math.round(health.smart.temperatureCelsius)) : ""
                    }
                    NxRow {
                        visible: health.smart.hasData === true && health.smart.powerOnSeconds > 0
                        icon: "clock"; title: qsTr("Powered on")
                        value: health.smart.powerOnSeconds > 0 ? qsTr("%1 hours").arg(Math.round(health.smart.powerOnSeconds / 3600)) : ""
                    }
                    NxRow {
                        visible: health.smart.hasData === true && health.smart.kind === "ata"
                        icon: "harddrive"; title: qsTr("Bad sectors")
                        value: health.smart.badSectors >= 0 ? String(health.smart.badSectors) : qsTr("not reported")
                    }
                    NxRow {
                        visible: health.smart.hasData === true && health.smart.selfTest !== undefined && health.smart.selfTest !== ""
                        icon: "activity"; title: qsTr("Last self-test"); value: health.smart.selfTest !== undefined ? health.smart.selfTest : ""
                    }
                    NxDataTable {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(420, 40 + rowHeight * count)
                        visible: health.smart.hasData === true && health.smart.attributes.length > 0
                        model: health.smart.attributes !== undefined ? health.smart.attributes : []
                        columns: health.smart.kind === "ata" ? [
                            { "title": qsTr("ID"), "role": "id", "width": 50, "align": Text.AlignRight, "mono": true },
                            { "title": qsTr("Attribute"), "role": "name", "fill": true },
                            { "title": qsTr("Value"), "role": "value", "width": 70, "align": Text.AlignRight, "mono": true,
                              "color": (v, row) => row.failing ? Theme.danger : Theme.textPrimary },
                            { "title": qsTr("Worst"), "role": "worst", "width": 70, "align": Text.AlignRight, "mono": true },
                            { "title": qsTr("Threshold"), "role": "threshold", "width": 90, "align": Text.AlignRight, "mono": true },
                            { "title": qsTr("Reading"), "role": "raw", "width": 130, "align": Text.AlignRight, "mono": true }
                        ] : [
                            { "title": qsTr("Attribute"), "role": "name", "fill": true },
                            { "title": qsTr("Reading"), "role": "raw", "width": 200, "align": Text.AlignRight, "mono": true }
                        ]
                    }
                }
            }
        }
    }
}
