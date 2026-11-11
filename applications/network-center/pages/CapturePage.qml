// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.networkcenter

NxPage {
    id: page
    title: qsTr("Packet capture")
    subtitle: qsTr("Record the traffic of an interface to a capture file in %1 for analysis in Wireshark.").arg(Capture.directory)
    maxContentWidth: 1040

    readonly property var capability: Capture.capability
    readonly property bool usable: capability.usable === true
    readonly property var interfaceChoices: NetState.interfaces.map(i => ({
            value: i.name,
            text: i.name + "  —  " + i.typeLabel + (i.status === "up" ? "" : qsTr(" (no link)"))
        })).concat([{ value: "any", text: qsTr("any  —  all interfaces at once") }])
    // The interface that carries the default route, until the user picks one.
    property string chosenIface: ""
    readonly property string iface: chosenIface !== "" && interfaceChoices.some(c => c.value === chosenIface) ? chosenIface
        : NetState.primaryInterface !== "" ? NetState.primaryInterface : "any"
    property bool ring: false
    readonly property string filter: filterField.text.trim()
    // The selection is remembered by path, so it survives refreshes and sorting.
    property string selectedPath: ""
    readonly property var selectedFile: Capture.files.find(f => f.path === selectedPath) || ({})

    Component.onCompleted: Capture.refreshCapability()
    onVisibleChanged: if (visible) Capture.refreshFiles()

    function startCapture() {
        Capture.start({
            interface: page.iface,
            filter: page.filter,
            maxFileMegabytes: sizeBox.value,
            maxSeconds: secondsBox.value,
            ring: page.ring,
            ringFiles: ringBox.value,
        })
    }

    Connections {
        target: Capture
        function onNotice(kind, text) { ApplicationWindow.window.toaster.show(text, kind) }
        // Keep the highlighted row on the selected file when the list changes.
        function onFilesChanged() { Qt.callLater(() => table.currentRow = files.indexOf("path", page.selectedPath)) }
    }

    // Compile the filter a moment after the user stops typing.
    Timer {
        id: filterTimer
        interval: 600
        onTriggered: Capture.validateFilter(page.iface, page.filter)
    }
    onIfaceChanged: if (filter !== "") filterTimer.restart()

    RecordModel {
        id: files
        roles: ["name", "path", "size", "modified", "modifiedText", "active"]
        records: Capture.files
    }

    NxConfirmDialog {
        id: deleteDialog
        property string path: ""
        property string name: ""
        title: qsTr("Delete “%1”?").arg(name)
        message: qsTr("The capture file is deleted permanently.")
        confirmText: qsTr("Delete file")
        destructive: true
        onConfirmed: Capture.deleteFile(path)
    }

    actions: [
        NxIconButton {
            iconName: "refresh"
            tooltip: qsTr("Check capture support again")
            enabled: !Capture.busy
            onClicked: Capture.refreshCapability()
        }
    ]

    // ---- can this system capture, and how ------------------------------------------
    NxBanner {
        visible: !Capture.checking && page.capability.summary !== undefined
        kind: !page.usable ? "danger" : page.capability.backend === "dumpcap" ? "success" : "info"
        title: page.capability.summary || ""
        message: (page.capability.problems || []).join("\n")
    }
    // ---- authorised use ---------------------------------------------------------------
    NxBanner {
        kind: "warning"
        title: qsTr("Capture only traffic you are authorised to see")
        message: qsTr("A capture records everything that crosses the chosen interface, including other people's and other programs' unencrypted data. Use it on networks you own or administer, or with the written permission of whoever does; intercepting communications without authorisation is a criminal offence in most countries. Capture files are stored in %1, readable by your account only — treat them as confidential.").arg(Capture.directory)
    }

    RowLayout {
        Layout.fillWidth: true
        visible: Capture.checking
        spacing: Theme.spaceSm
        BusyIndicator { running: Capture.checking; implicitWidth: 28; implicitHeight: 28 }
        Label { text: qsTr("Checking which capture tool can be used…"); color: Theme.textMuted }
    }

    NxSection {
        title: qsTr("Capture")
        enabled: page.usable

        FormRow {
            label: qsTr("Interface")
            enabled: !Capture.busy
            ComboBox {
                Layout.fillWidth: true
                model: page.interfaceChoices
                textRole: "text"
                valueRole: "value"
                currentIndex: page.interfaceChoices.findIndex(c => c.value === page.iface)
                Accessible.name: qsTr("Interface to capture on")
                onActivated: page.chosenIface = currentValue
            }
        }
        FormRow {
            label: qsTr("Capture filter")
            hint: qsTr("Optional, pcap filter syntax — for example: tcp port 443 and host 192.0.2.1")
            enabled: !Capture.busy
            error: Capture.filterState === "invalid" || Capture.filterState === "unavailable" ? Capture.filterMessage : ""
            TextField {
                id: filterField
                Layout.fillWidth: true
                font.family: Theme.fontMono
                placeholderText: qsTr("Empty: capture everything")
                Accessible.name: qsTr("Capture filter")
                onTextEdited: {
                    Capture.clearFilterState()
                    if (text.trim() !== "")
                        filterTimer.restart()
                    else
                        filterTimer.stop()
                }
            }
            NxBadge {
                visible: Capture.filterState === "valid" || Capture.filterState === "checking"
                kind: Capture.filterState === "valid" ? "success" : "neutral"
                text: Capture.filterState === "valid" ? qsTr("Compiles") : qsTr("Checking…")
            }
        }
        FormRow {
            label: qsTr("Limits per file")
            hint: qsTr("0 means no limit. Megabytes are 1,000,000 bytes.")
            enabled: !Capture.busy
            SpinBox {
                id: sizeBox
                from: 0
                to: 100000
                value: 100
                stepSize: 10
                editable: true
                Accessible.name: qsTr("Maximum file size in megabytes")
            }
            Label { text: qsTr("MB"); color: Theme.textSecondary }
            Item { Layout.preferredWidth: Theme.spaceLg }
            SpinBox {
                id: secondsBox
                from: 0
                to: 604800
                value: 0
                stepSize: 30
                editable: true
                Accessible.name: qsTr("Maximum duration in seconds")
            }
            Label { text: qsTr("seconds"); color: Theme.textSecondary }
            Item { Layout.fillWidth: true }
        }
        FormRow {
            label: qsTr("When a limit is reached")
            hint: page.ring ? qsTr("Continue in a new file and delete the oldest, so the newest traffic is always kept")
                            : qsTr("End the capture")
            enabled: !Capture.busy
            error: page.ring && sizeBox.value === 0 && secondsBox.value === 0 ? qsTr("A ring buffer needs a size or a time limit per file.") : ""
            NxSegmented {
                model: [qsTr("Stop"), qsTr("Ring buffer")]
                currentIndex: page.ring ? 1 : 0
                onActivated: (index) => page.ring = index === 1
            }
            Item { Layout.preferredWidth: Theme.spaceLg; visible: page.ring }
            Label { visible: page.ring; text: qsTr("keep"); color: Theme.textSecondary }
            SpinBox {
                id: ringBox
                visible: page.ring
                from: 2
                to: 1000
                value: 5
                editable: true
                Accessible.name: qsTr("Number of files kept in the ring buffer")
            }
            Label { visible: page.ring; text: qsTr("files"); color: Theme.textSecondary }
            Item { Layout.fillWidth: true }
        }
        FormRow {
            label: Capture.state === "capturing" ? qsTr("Capturing on %1").arg(Capture.interface)
                : Capture.state === "starting" ? qsTr("Starting…")
                : Capture.state === "stopping" ? qsTr("Stopping…") : qsTr("Ready")
            hint: Capture.state === "starting" && page.capability.needsAuthentication === true
                ? qsTr("Waiting for administrator authentication")
                : Capture.state === "idle" && page.capability.needsAuthentication === true ? qsTr("Starting asks for administrator authentication")
                : ""
            BusyIndicator {
                visible: Capture.busy
                running: visible
                implicitWidth: 28
                implicitHeight: 28
            }
            Item { Layout.fillWidth: true }
            NxButton {
                visible: !Capture.busy
                text: qsTr("Start capture")
                kind: "primary"
                iconName: "play"
                enabled: page.usable && page.iface !== "" && Capture.filterState !== "invalid" && Capture.filterState !== "checking"
                    && !(page.ring && sizeBox.value === 0 && secondsBox.value === 0)
                onClicked: page.startCapture()
            }
            NxButton {
                visible: Capture.busy
                text: qsTr("Stop capture")
                kind: "danger"
                iconName: "stop"
                enabled: Capture.state !== "stopping"
                onClicked: Capture.stop()
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        visible: Capture.busy || Capture.packets > 0
        spacing: Theme.spaceLg
        NxStatTile {
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            label: qsTr("Packets")
            icon: "activity"
            value: Number(Capture.packets).toLocaleString(Qt.locale(), "f", 0)
            detail: Capture.state === "capturing" ? qsTr("written so far") : qsTr("in the last capture")
            tone: Capture.state === "capturing" ? "accent" : "neutral"
        }
        NxStatTile {
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            label: qsTr("Size on disk")
            icon: "harddrive"
            value: Theme.formatBytes(Capture.bytesOnDisk)
            detail: Capture.sessionFiles.length > 1 ? qsTr("%1 files kept · %2 captured in all").arg(Capture.sessionFiles.length).arg(Theme.formatBytes(Capture.bytesCaptured))
                : Capture.format !== "" ? qsTr("%1 format").arg(Capture.format) : ""
            tone: "neutral"
        }
        NxStatTile {
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            label: qsTr("Duration")
            icon: "clock"
            value: Theme.formatDuration(Capture.elapsedSeconds)
            detail: secondsBox.value > 0 ? (page.ring ? qsTr("new file every %1 s").arg(secondsBox.value) : qsTr("stops after %1 s").arg(secondsBox.value)) : qsTr("no time limit")
            tone: "neutral"
        }
    }
    Label {
        Layout.fillWidth: true
        visible: Capture.currentFile !== ""
        text: (Capture.busy ? qsTr("Writing to %1") : qsTr("Last file: %1")).arg(Capture.currentFile)
        color: Theme.textSecondary
        font.family: Theme.fontMono
        font.pixelSize: Theme.fontSizeSmall
        elide: Text.ElideMiddle
    }

    NxBanner {
        visible: Capture.message !== ""
        kind: Capture.messageKind === "" ? "info" : Capture.messageKind
        message: Capture.message
    }

    // ---- files -----------------------------------------------------------------------------
    NxSection {
        title: qsTr("Capture files")
        description: Capture.directory
        headerActions: [
            NxButton {
                text: qsTr("Open in Wireshark")
                iconName: "external-link"
                enabled: Capture.wiresharkInstalled && page.selectedFile.path !== undefined
                onClicked: Capture.openInWireshark(page.selectedFile.path)
            },
            NxButton {
                text: qsTr("Show in Files")
                iconName: "folder-open"
                onClicked: Capture.showInFiles(page.selectedFile.path === undefined ? "" : page.selectedFile.path)
            },
            NxButton {
                text: qsTr("Delete")
                iconName: "trash"
                enabled: page.selectedFile.path !== undefined && page.selectedFile.active !== true
                onClicked: {
                    deleteDialog.path = page.selectedFile.path
                    deleteDialog.name = page.selectedFile.name
                    deleteDialog.open()
                }
            }
        ]

        Item {
            Layout.fillWidth: true
            implicitHeight: Math.min(300, 44 + Math.max(2, files.count) * 34) + Theme.spaceSm * 2
            NxDataTable {
                id: table
                anchors.fill: parent
                anchors.margins: Theme.spaceSm
                model: files
                emptyText: qsTr("No capture files yet")
                columns: [
                    { title: qsTr("File"), role: "name", fill: true, mono: true,
                      color: (v, row) => row.active ? Theme.accent : Theme.textPrimary },
                    { title: qsTr("Size"), role: "size", width: 110, align: Qt.AlignRight, mono: true, format: (v) => Theme.formatBytes(v) },
                    { title: qsTr("Modified"), role: "modified", width: 170, format: (v, row) => row.modifiedText },
                ]
                onSortRequested: (role, ascending) => {
                    files.sortBy(role, ascending)
                    table.currentRow = files.indexOf("path", page.selectedPath)
                }
                onRowSelected: (row, data) => page.selectedPath = files.get(row).path
                onRowActivated: (row, data) => { if (Capture.wiresharkInstalled) Capture.openInWireshark(files.get(row).path) }
            }
        }
        NxRow {
            visible: !Capture.wiresharkInstalled
            icon: "info"
            title: qsTr("Wireshark is not installed")
            subtitle: qsTr("Install the wireshark package to open capture files from here. The files can be read by any pcap-capable tool.")
        }
    }
}
