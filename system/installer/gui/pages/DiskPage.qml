// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.installer

StepPage {
    id: page
    required property WizardState wiz

    readonly property bool ok: Installer.probeState === "ready"
    readonly property var report: Installer.probe
    readonly property var targets: ok ? wiz.targetDisks() : []
    readonly property var excluded: ok ? wiz.disks.filter(d => !(d.usable && !d.is_live_medium)) : []
    readonly property var otherSystems: report.other_systems || []
    readonly property bool systemsScanned: ok && report.other_systems !== null && report.other_systems !== undefined
    readonly property bool anyFreeSpace: targets.some(d => wiz.freeSpaceProblem(d) === "")
    // Partitions that manual mode may use: every disk except the installation medium.
    readonly property var manualPartitions: {
        const out = []
        if (ok)
            for (const d of wiz.disks) {
                if (d.is_live_medium || d.read_only) continue
                for (const p of d.partitions) out.push({ disk: d, part: p })
            }
        return out
    }
    readonly property bool guided: wiz.mode !== "manual"

    function diskProblem(d) { return wiz.mode === "free-space" ? wiz.freeSpaceProblem(d) : wiz.eraseProblem(d) }
    function diskName(d) { return [d.vendor, d.model].filter(x => x).join(" ") || qsTr("Unknown model") }
    function systemOn(path) {
        for (const s of otherSystems)
            if (s.device === path) return s.name
        return ""
    }
    function partitionText(p) {
        const bits = [p.fstype ? p.fstype : qsTr("no filesystem")]
        if (p.label) bits.push("“" + p.label + "”")
        else if (p.parttype_name) bits.push(p.parttype_name)
        return bits.join(" · ")
    }
    // Keep the selection consistent with what the engine would accept.
    function reconcile() {
        if (!ok) return
        if (wiz.mode === "free-space" && !anyFreeSpace) wiz.mode = "erase-disk"
        if (wiz.mode === "manual" && manualPartitions.length === 0) wiz.mode = "erase-disk"
        if (!guided) return
        const valid = targets.filter(d => diskProblem(d) === "")
        if (!valid.some(d => d.path === wiz.diskPath))
            wiz.diskPath = valid.length === 1 ? valid[0].path : ""
    }
    function entered() { reconcile() }
    Connections {
        target: Installer
        function onProbeChanged() { page.reconcile() }
    }

    readonly property string manualProblem: {
        if (wiz.mountOf("/") === "") return qsTr("Assign a partition to “/” (the system).")
        if (wiz.firmware === "uefi" && wiz.mountOf("/boot/efi") === "")
            return qsTr("Assign the EFI System Partition to “/boot/efi” (this computer starts in UEFI mode).")
        if (wiz.firmware === "bios" && wiz.bootloaderDevice === "")
            return qsTr("Choose the disk that receives the boot loader.")
        return ""
    }

    title: qsTr("Disk")
    subtitle: qsTr("Choose where and how NEXUS OS is installed. This step only records your choice — the disk is "
                 + "written after you confirm the plan on the summary page.")
    ready: ok && (guided ? (wiz.diskPath !== "" && wiz.disk(wiz.diskPath) !== null && diskProblem(wiz.disk(wiz.diskPath)) === ""
                            && (!wiz.encrypt || Installer.passphraseSet))
                         : manualProblem === "")
    blockReason: !ok ? qsTr("The system check has to succeed first.")
               : guided ? (wiz.diskPath === "" ? qsTr("Choose the target disk.")
                           : wiz.encrypt && !Installer.passphraseSet ? qsTr("Enter the encryption passphrase twice.") : "")
               : manualProblem

    headerActions: NxButton {
        visible: page.ok && !page.systemsScanned
        text: qsTr("Look for installed systems")
        iconName: "search"
        busy: Installer.scanningSystems
        onClicked: scanDialog.open()
    }

    NxConfirmDialog {
        id: scanDialog
        title: qsTr("Look for installed operating systems?")
        message: qsTr("The engine runs os-prober, which mounts each filesystem read-only for a moment to see whether "
                    + "it holds an operating system. This can take a few minutes. Without the scan the disks are "
                    + "still listed with their partitions and filesystem types.")
        confirmText: qsTr("Scan")
        onConfirmed: Installer.runProbe(true)
    }

    NxEmptyState {
        Layout.fillWidth: true
        Layout.fillHeight: true
        visible: !page.ok
        icon: "harddrive"
        tone: "warning"
        title: qsTr("No information about the disks")
        message: qsTr("The system check has not produced a hardware report. Go back to “System check”.")
    }

    ScrollColumn {
        visible: page.ok

        NxBanner {
            visible: Installer.probeError !== "" && page.ok
            kind: "warning"
            title: Installer.probeError
            message: Installer.probeErrorDetail
        }

        // ---------------------------------------------------------------- type
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceSm
            SectionTitle { text: qsTr("Installation type") }
            ChoiceCard {
                icon: "trash"
                title: qsTr("Erase a disk and install NEXUS OS")
                description: qsTr("The whole disk is used. Everything on it — every partition and operating system — is destroyed.")
                selected: page.wiz.mode === "erase-disk"
                danger: true
                onChosen: { page.wiz.mode = "erase-disk"; page.reconcile() }
            }
            ChoiceCard {
                icon: "columns"
                title: qsTr("Install alongside, in unallocated space")
                description: qsTr("NEXUS OS gets new partitions in the largest unallocated region of a disk. Existing partitions "
                                + "are not changed, and other systems are added to the boot menu.")
                problem: page.anyFreeSpace ? "" : qsTr("Not possible here: the engine needs a disk with a GPT partition table and "
                                                       + "unallocated space. Shrink a partition with the system that owns it, then check again.")
                selected: page.wiz.mode === "free-space"
                onChosen: { page.wiz.mode = "free-space"; page.reconcile() }
            }
            ChoiceCard {
                icon: "sliders"
                title: qsTr("Manual: use existing partitions")
                description: qsTr("You assign partitions that already exist to mount points. The installer does not create, "
                                + "resize or delete partitions in this mode. Encryption is not available.")
                problem: page.manualPartitions.length > 0 ? "" : qsTr("Not possible here: no disk other than the installation medium has partitions.")
                selected: page.wiz.mode === "manual"
                onChosen: { page.wiz.mode = "manual"; page.reconcile() }
            }
        }

        // ---------------------------------------------------------------- disk
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceSm
            visible: page.guided
            SectionTitle {
                text: qsTr("Target disk")
                detail: page.systemsScanned ? qsTr("Operating systems found by the scan are named next to their partition.")
                                            : qsTr("Partitions are listed as the engine sees them; operating systems are only named after “Look for installed systems”.")
            }
            Repeater {
                model: page.targets
                ChoiceCard {
                    id: card
                    required property var modelData
                    readonly property var d: modelData
                    icon: d.transport === "usb" ? "usb" : "harddrive"
                    title: page.diskName(d) + " — " + Theme.formatBytes(d.size)
                    description: d.path + (d.transport ? " · " + d.transport.toUpperCase() : "")
                               + (d.rotational ? " · " + qsTr("hard disk") : "")
                               + " · " + (d.partition_table ? qsTr("%1 partition table").arg(d.partition_table.toUpperCase()) : qsTr("no partition table"))
                    problem: page.diskProblem(d)
                    selected: page.wiz.diskPath === d.path
                    danger: page.wiz.mode === "erase-disk"
                    onChosen: page.wiz.diskPath = d.path
                    badges: [
                        NxBadge { visible: card.d.removable; text: qsTr("Removable"); kind: "warning" },
                        NxBadge { visible: card.d.has_luks; text: qsTr("Encrypted volume"); kind: "neutral" }
                    ]

                    Repeater {
                        model: card.d.partitions
                        RowLayout {
                            id: partRow
                            required property var modelData
                            readonly property string system: page.systemOn(modelData.path)
                            Layout.fillWidth: true
                            spacing: Theme.spaceMd
                            Text {
                                Layout.preferredWidth: 130
                                text: partRow.modelData.name
                                color: Theme.textSecondary
                                font.family: Theme.fontMono
                                font.pixelSize: Theme.fontSizeSmall
                                elide: Text.ElideRight
                            }
                            Text {
                                Layout.preferredWidth: 80
                                horizontalAlignment: Text.AlignRight
                                text: Theme.formatBytes(partRow.modelData.size)
                                color: Theme.textSecondary
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fontSizeSmall
                            }
                            Text {
                                Layout.fillWidth: true
                                text: page.partitionText(partRow.modelData) + (partRow.system !== "" ? " · " + partRow.system : "")
                                color: partRow.system !== "" ? Theme.textPrimary : Theme.textMuted
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fontSizeSmall
                                elide: Text.ElideRight
                            }
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeSmall
                        color: card.selected && page.wiz.mode === "erase-disk" && card.d.partitions.length > 0 ? Theme.danger : Theme.textMuted
                        text: card.d.partitions.length === 0
                              ? (card.d.whole_disk_fstype ? qsTr("No partitions; the whole disk holds a %1 filesystem.").arg(card.d.whole_disk_fstype)
                                                           : qsTr("No partitions."))
                              : page.wiz.mode === "erase-disk"
                                ? (card.selected ? (card.d.partitions.length === 1 ? qsTr("The partition above will be destroyed.") : qsTr("All %1 partitions above will be destroyed.").arg(card.d.partitions.length))
                                                 : qsTr("Unallocated: %1.").arg(Theme.formatBytes(card.d.largest_free)))
                                : qsTr("Largest unallocated region: %1. The partitions above stay as they are.").arg(Theme.formatBytes(card.d.largest_free))
                    }
                }
            }
            Text {
                Layout.fillWidth: true
                visible: page.excluded.length > 0
                wrapMode: Text.WordWrap
                color: Theme.textMuted
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                text: qsTr("Not offered: ") + page.excluded.map(d => d.path + " (" + page.diskName(d) + ", " + Theme.formatBytes(d.size)
                                                                    + ") — " + (d.unusable_reason || qsTr("not usable"))).join("  ")
            }
        }

        // ------------------------------------------------------------- options
        NxSection {
            title: qsTr("Options")
            visible: page.guided

            NxRow {
                icon: "disk-lock"
                title: qsTr("Encrypt the system")
                subtitle: qsTr("LUKS2 full-disk encryption. The passphrase is asked at every start and cannot be recovered if it is lost. "
                             + "The small /boot partition with the kernels stays unencrypted.")
                Switch {
                    anchors.verticalCenter: parent.verticalCenter
                    checked: page.wiz.encrypt
                    Accessible.name: qsTr("Encrypt the system")
                    onToggled: page.wiz.encrypt = checked
                }
            }
            Item {
                Layout.fillWidth: true
                visible: page.wiz.encrypt
                implicitHeight: passphrase.implicitHeight + Theme.spaceMd * 2
                SecretFields {
                    id: passphrase
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.leftMargin: Theme.spaceLg
                    anchors.rightMargin: Theme.spaceLg
                    kind: "passphrase"
                    label: qsTr("Encryption passphrase (at least 8 characters)")
                    confirmLabel: qsTr("Repeat passphrase")
                    personalWords: [page.wiz.userName, page.wiz.hostName].concat(page.wiz.fullName.split(" "))
                    held: Installer.passphraseSet
                    onCommitted: (secret) => Installer.setPassphrase(secret)
                }
            }
            NxRow {
                icon: "home"
                title: qsTr("Separate /home")
                subtitle: qsTr("Personal files get their own partition (or logical volume), so the system can be reinstalled without touching them.")
                Switch {
                    anchors.verticalCenter: parent.verticalCenter
                    checked: page.wiz.separateHome
                    Accessible.name: qsTr("Separate /home")
                    onToggled: page.wiz.separateHome = checked
                }
            }
            NxRow {
                icon: "memory"
                title: qsTr("Swap")
                subtitle: qsTr("Space on disk for memory that does not fit into RAM. The engine chooses the size from the amount of memory; the summary shows it.")
                NxSegmented {
                    anchors.verticalCenter: parent.verticalCenter
                    model: [qsTr("File"), qsTr("Partition"), qsTr("None")]
                    currentIndex: ["file", "partition", "none"].indexOf(page.wiz.swap)
                    onActivated: (index) => page.wiz.swap = ["file", "partition", "none"][index]
                }
            }
        }

        // -------------------------------------------------------------- manual
        NxSection {
            title: qsTr("Partitions")
            description: qsTr("“/” is always formatted. Other partitions keep their files unless “Format” is ticked.")
            visible: !page.guided

            Repeater {
                model: page.manualPartitions
                Item {
                    id: row
                    required property var modelData
                    required property int index
                    readonly property var p: modelData.part
                    readonly property var assignment: page.wiz.manualMounts[p.path]
                    readonly property string use: assignment ? assignment.use : ""
                    readonly property bool mounted: p.mountpoints.length > 0
                    readonly property var uses: ["", "/"].concat(page.wiz.firmware === "uefi" ? ["/boot/efi"] : [])
                                                         .concat(["/boot", "/home", "/var", "/srv", "/opt", "/tmp", "swap"])
                    Layout.fillWidth: true
                    implicitHeight: Theme.rowHeightLarge

                    Rectangle {
                        visible: row.index > 0
                        anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
                        anchors.leftMargin: Theme.spaceLg
                        height: 1; color: Theme.borderSubtle
                    }
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spaceLg
                        anchors.rightMargin: Theme.spaceLg
                        spacing: Theme.spaceMd
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1
                            Text {
                                Layout.fillWidth: true
                                text: row.p.path + "  ·  " + Theme.formatBytes(row.p.size)
                                color: Theme.textPrimary
                                font.family: Theme.fontMono
                                font.pixelSize: Theme.fontSizeSmall
                                elide: Text.ElideRight
                            }
                            Text {
                                Layout.fillWidth: true
                                readonly property string system: page.systemOn(row.p.path)
                                text: row.mounted ? qsTr("In use (mounted at %1) — cannot be assigned").arg(row.p.mountpoints.join(", "))
                                    : page.partitionText(row.p) + (system !== "" ? " · " + system : "") + " · " + page.diskName(row.modelData.disk)
                                color: row.mounted ? Theme.warning : Theme.textMuted
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fontSizeSmall
                                elide: Text.ElideRight
                            }
                        }
                        ComboBox {
                            Layout.preferredWidth: 150
                            enabled: !row.mounted
                            model: row.uses.map(u => u === "" ? qsTr("Do not use") : u === "swap" ? qsTr("Swap") : u)
                            currentIndex: Math.max(0, row.uses.indexOf(row.use))
                            Accessible.name: qsTr("Use of %1").arg(row.p.path)
                            onActivated: (index) => page.wiz.setMount(row.p.path, row.uses[index], false)
                        }
                        CheckBox {
                            Layout.preferredWidth: 96
                            text: qsTr("Format")
                            enabled: row.use !== "" && row.use !== "/" && row.use !== "swap"
                            checked: row.assignment ? row.assignment.format : false
                            Accessible.name: qsTr("Format %1").arg(row.p.path)
                            onToggled: page.wiz.setMount(row.p.path, row.use, checked)
                        }
                    }
                }
            }
        }

        NxSection {
            title: qsTr("Boot loader and swap")
            visible: !page.guided

            NxRow {
                visible: page.wiz.firmware === "bios"
                icon: "harddrive"
                title: qsTr("Disk for the boot loader")
                subtitle: qsTr("This computer starts in legacy BIOS mode: GRUB is written to the boot sector of this disk.")
                ComboBox {
                    id: bootDisk
                    readonly property var choices: page.ok ? page.wiz.disks.filter(d => !d.is_live_medium && !d.read_only) : []
                    anchors.verticalCenter: parent.verticalCenter
                    width: 260
                    model: [qsTr("Choose…")].concat(choices.map(d => d.path + " — " + page.diskName(d)))
                    currentIndex: 1 + choices.findIndex(d => d.path === page.wiz.bootloaderDevice)
                    Accessible.name: qsTr("Disk for the boot loader")
                    onActivated: (index) => page.wiz.bootloaderDevice = index > 0 ? choices[index - 1].path : ""
                }
            }
            NxRow {
                icon: "memory"
                title: qsTr("Swap file")
                subtitle: qsTr("In addition to (or instead of) a partition assigned to “Swap” above.")
                Switch {
                    anchors.verticalCenter: parent.verticalCenter
                    checked: page.wiz.swap === "file"
                    Accessible.name: qsTr("Swap file")
                    onToggled: page.wiz.swap = checked ? "file" : "none"
                }
            }
        }
    }
}
