// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.nexus.ui
import "Fmt.js" as Fmt

// Section "encryption": LUKS devices and whether /, /home and swap are on them.
SectionPage {
    id: page

    readonly property var enc: ready ? section : ({})
    readonly property var sys: Fmt.has(enc.root) ? enc.root : ({})
    readonly property var home: Fmt.has(enc.home) ? enc.home : ({})
    readonly property var swap: Fmt.has(enc.swap) ? enc.swap : ({})
    readonly property var luks: Fmt.list(enc.luks_devices)

    function stateText(v) { return Fmt.tri(v, qsTr("Encrypted"), qsTr("Not encrypted"), qsTr("Not determined")) }
    function stateKind(v) { return Fmt.tri(v, "success", "warning", "neutral") }

    sectionKey: "encryption"
    title: qsTr("Encryption")
    sectionTitle: qsTr("The encryption state")
    subtitle: qsTr("Whether the data on this computer's storage is protected by LUKS when the computer is off.")

    NxBanner {
        visible: page.ready && page.enc.live_session === true
        kind: "info"
        title: qsTr("This is a live session")
        message: qsTr("The system runs from installation media. Encryption is chosen when NEXUS OS is installed.")
    }

    NxSection {
        visible: page.ready
        title: qsTr("Mounted file systems")
        description: qsTr("A file system counts as encrypted when a LUKS container lies between it and the disk.")

        NxRow {
            icon: "harddrive"
            title: qsTr("System ( / )")
            subtitle: Fmt.has(page.sys.device) ? page.sys.device
                : qsTr("lsblk lists no block device mounted at /, so the tool cannot tell (container, chroot or network root).")
            NxBadge { text: page.stateText(page.sys.encrypted); kind: page.stateKind(page.sys.encrypted) }
        }
        NxRow {
            icon: "home"
            title: qsTr("Home ( /home )")
            subtitle: page.home.separate ? (page.home.device || "")
                : qsTr("Not a separate file system — it is part of the system file system.")
            NxBadge { text: page.stateText(page.home.encrypted); kind: page.stateKind(page.home.encrypted) }
        }
        NxRow {
            icon: "memory"
            title: qsTr("Swap")
            subtitle: !page.swap.present ? qsTr("No swap space is in use.")
                : Fmt.has(page.swap.device) ? page.swap.device
                : qsTr("Swap is in use, but not on a block device lsblk reports as [SWAP] (swap file or zram).")
            NxBadge {
                text: page.swap.present ? page.stateText(page.swap.encrypted) : qsTr("No swap")
                kind: page.swap.present ? page.stateKind(page.swap.encrypted) : "neutral"
            }
        }
    }
    Note {
        visible: page.ready && !Array.isArray(page.enc.filesystems)
        text: qsTr("The status tool evaluates /, /home and swap. Other mounted file systems (data disks, removable media) are not part of its report.")
    }
    NxSection {
        visible: page.ready && Array.isArray(page.enc.filesystems)
        title: qsTr("All mounted file systems")
        TableCard {
            Layout.margins: Theme.spaceXs
            color: "transparent"
            border.width: 0
            rows: Fmt.list(page.enc.filesystems)
            sortRole: "mountpoint"
            columns: [
                { title: qsTr("Mount point"), role: "mountpoint", fill: true, mono: true },
                { title: qsTr("Device"), role: "device", fill: true, mono: true },
                { title: qsTr("File system"), role: "fstype", width: 120 },
                { title: qsTr("Encrypted"), role: "encrypted", width: 130,
                  format: (v) => page.stateText(v), color: (v) => v === true ? Theme.success : v === false ? Theme.warning : Theme.textSecondary }
            ]
        }
    }

    NxSection {
        visible: page.ready
        title: qsTr("LUKS devices")
        description: qsTr("Block devices that hold a LUKS container, with the header details from cryptsetup luksDump.")
        NxRow {
            visible: page.luks.length === 0
            icon: "lock-open"
            title: qsTr("No LUKS device was found")
            subtitle: qsTr("lsblk reports no block device with a LUKS header.")
        }
        TableCard {
            visible: page.luks.length > 0
            Layout.margins: Theme.spaceXs
            color: "transparent"
            border.width: 0
            rows: page.luks
            sortRole: "device"
            columns: [
                { title: qsTr("Device"), role: "device", fill: true, mono: true },
                { title: qsTr("LUKS version"), role: "version", width: 130, format: (v) => "LUKS" + v },
                { title: qsTr("Cipher"), role: "cipher", width: 200, mono: true },
                { title: qsTr("Key derivation"), role: "pbkdf", width: 150, mono: true },
                { title: qsTr("Unlocked"), role: "unlocked", width: 110,
                  format: (v) => v ? qsTr("Yes") : qsTr("No") }
            ]
        }
    }
}
