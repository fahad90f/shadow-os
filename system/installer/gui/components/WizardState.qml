// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.shadow.installer

// Everything the user has chosen so far, except the two secrets (those go
// straight to Installer.setPassword()/setPassphrase() and cannot be read
// back). answers() turns the choices into the engine's answer file.
QtObject {
    id: root

    property string locale: Locales.defaultCode !== "" ? Locales.defaultCode : "en_US.UTF-8"
    property string layout: "us"
    property string variant: ""
    property bool layoutChosen: false       // false: follow the language's suggestion
    property string timezone: "Etc/UTC"
    property bool timezoneChosen: false

    property string diskPath: ""
    property string mode: "erase-disk"      // erase-disk | free-space | manual
    property bool encrypt: false
    property bool separateHome: false
    property string swap: "file"            // file | partition | none
    // manual mode: device path → { use: "/" | "/boot/efi" | … | "swap", format: bool }
    property var manualMounts: ({})
    property string bootloaderDevice: ""

    property string fullName: ""
    property string userName: ""
    property bool userNameEdited: false
    property string hostName: "shadow"
    property bool autologin: false

    property var profiles: []

    // Set when the engine reported exit status 3 (plan changed) and the
    // summary has to explain why the user is looking at it again.
    property string planChangedNotice: ""

    readonly property string firmware: Installer.probeState === "ready" ? Installer.probe.firmware.type : ""
    readonly property var disks: Installer.probeState === "ready" ? Installer.probe.disks : []

    function disk(path) {
        for (const d of disks)
            if (d.path === path) return d
        return null
    }
    // Disks that may be chosen as a target. The installation medium is never among them.
    function targetDisks() { return disks.filter(d => d.usable && !d.is_live_medium) }
    // Why "erase" is impossible right now (the engine's own rule), or "".
    function eraseProblem(d) {
        if (!d) return ""
        if (d.mountpoints.length > 0)
            return qsTr("In use: mounted at %1. Unmount it first.").arg(d.mountpoints.join(", "))
        if (d.active_mappings.length > 0)
            return qsTr("Has active encrypted or logical volumes (%1). Close them first.").arg(d.active_mappings.join(", "))
        return ""
    }
    // Why "alongside" is impossible (the engine's own rules), or "".
    function freeSpaceProblem(d) {
        if (!d) return ""
        if (d.partition_table !== "gpt")
            return qsTr("Needs a GPT partition table; this disk has %1.").arg(d.partition_table ? d.partition_table : qsTr("none"))
        if (d.free_regions.length === 0)
            return qsTr("The disk has no unallocated space. Shrink a partition with the system that owns it first.")
        return ""
    }
    function setMount(device, use, format) {
        const copy = Object.assign({}, manualMounts)
        if (use === "")
            delete copy[device]
        else {
            // a mount point can be assigned once
            for (const other in copy)
                if (other !== device && use !== "swap" && copy[other].use === use) delete copy[other]
            copy[device] = { use: use, format: use === "/" || use === "swap" ? true : format }
        }
        manualMounts = copy
    }
    function mountOf(use) {
        for (const device in manualMounts)
            if (manualMounts[device].use === use) return device
        return ""
    }

    function answers() {
        const storage = { mode: mode }
        if (mode === "manual") {
            storage.swap = swap === "partition" ? "none" : swap
            storage.mounts = []
            for (const device in manualMounts) {
                const m = manualMounts[device]
                if (m.use === "swap") storage.mounts.push({ device: device, use: "swap" })
                else storage.mounts.push({ device: device, mountpoint: m.use, format: m.format })
            }
            if (firmware === "bios" && bootloaderDevice !== "")
                storage.bootloader_device = bootloaderDevice
        } else {
            storage.disk = diskPath
            storage.encrypt = encrypt
            storage.separate_home = separateHome
            storage.swap = swap
        }
        const doc = {
            version: 1,
            locale: locale,
            keyboard: { layout: layout, variant: variant },
            timezone: timezone,
            hostname: hostName,
            user: { fullname: fullName.trim(), username: userName, autologin: autologin },
            storage: storage
        }
        if (profiles.length > 0)
            doc.packages = { profiles: profiles, install_now: true }
        return doc
    }

    // --preset (test mode): an answer file without its secrets.
    function applyPreset(p) {
        if (!p || p.version === undefined) return
        if (p.locale) locale = p.locale
        if (p.keyboard && p.keyboard.layout) { layout = p.keyboard.layout; variant = p.keyboard.variant || ""; layoutChosen = true }
        if (p.timezone) { timezone = p.timezone; timezoneChosen = true }
        if (p.hostname) hostName = p.hostname
        if (p.user) {
            fullName = p.user.fullname || ""
            userName = p.user.username || ""
            userNameEdited = true
            autologin = p.user.autologin === true
        }
        const s = p.storage || {}
        if (s.mode) mode = s.mode
        if (s.disk) diskPath = s.disk
        encrypt = s.encrypt === true
        separateHome = s.separate_home === true
        if (s.swap) swap = s.swap
        if (s.bootloader_device) bootloaderDevice = s.bootloader_device
        const mounts = {}
        for (const m of (s.mounts || []))
            mounts[m.device] = m.use === "swap" ? { use: "swap", format: true } : { use: m.mountpoint, format: m.format === true }
        manualMounts = mounts
        if (p.packages && p.packages.profiles) profiles = p.packages.profiles
    }
}
