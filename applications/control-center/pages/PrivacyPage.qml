// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.controlcenter

NxPage {
    id: page
    title: qsTr("Privacy")
    subtitle: qsTr("Screen lock, traces of what you used, and how this computer identifies itself on networks.")

    // Bumped when a change is refused, so controls show the real value again.
    property int revision: 0
    // Makes a binding depend on 'revision' as well as on the value itself.
    function live(revision, value) { return value }
    property string cleanId: ""

    function choices(values, current, format) {
        const list = values.slice()
        if (list.indexOf(current) < 0) list.push(current)
        list.sort((a, b) => a - b)
        return list.map(v => ({ value: v, text: format(v) }))
    }
    function indexOfValue(items, value) {
        for (let i = 0; i < items.length; i++) if (items[i].value === value) return i
        return -1
    }
    function minutesText(m) {
        if (m === 1) return qsTr("1 minute")
        if (m < 60 || m % 60 !== 0) return qsTr("%1 minutes").arg(m)
        return m === 60 ? qsTr("1 hour") : qsTr("%1 hours").arg(m / 60)
    }
    function graceText(s) {
        if (s === 0) return qsTr("Immediately")
        if (s < 60 || s % 60 !== 0) return qsTr("After %1 seconds").arg(s)
        return s === 60 ? qsTr("After 1 minute") : qsTr("After %1 minutes").arg(s / 60)
    }

    function cleanupTitle(id) {
        return id === "trash" ? qsTr("Trash") : id === "thumbnails" ? qsTr("Thumbnail cache") : qsTr("Old temporary files")
    }
    function cleanupIcon(id) {
        return id === "trash" ? "trash" : id === "thumbnails" ? "image" : "file"
    }
    function cleanupDescription(item) {
        if (item.id === "trash") return qsTr("Deleted files that can still be restored (%1)").arg(item.locations[0])
        if (item.id === "thumbnails") return qsTr("Previews of pictures and documents; recreated when needed (%1)").arg(item.locations[0])
        return qsTr("Your own files in /tmp and /var/tmp that have not changed for 24 hours")
    }
    function cleanupValue(item) {
        if (item.bytes < 0) return qsTr("Measuring…")
        if (item.files === 0) return qsTr("Empty")
        return item.files === 1 ? qsTr("%1 in 1 file").arg(Theme.formatBytes(item.bytes))
                                : qsTr("%1 in %2 files").arg(Theme.formatBytes(item.bytes)).arg(item.files)
    }
    function cleanupConfirm(id) {
        return id === "trash" ? qsTr("Everything in the trash is deleted permanently. This cannot be undone.")
             : id === "thumbnails" ? qsTr("All cached previews are deleted. Applications create them again when they show the files.")
             : qsTr("Your files in /tmp and /var/tmp that have not been modified for 24 hours are deleted. An application that is still running may be using one of them.")
    }

    function macTitle(key) {
        return key === "wifi.scan-rand-mac-address" ? qsTr("Hardware address while searching for Wi-Fi networks")
             : key === "wifi.cloned-mac-address" ? qsTr("Hardware address on Wi-Fi networks")
             : key === "ethernet.cloned-mac-address" ? qsTr("Hardware address on wired networks")
             : qsTr("IPv6 privacy extensions")
    }
    function macValue(item) {
        const v = item.value
        if (item.key === "wifi.scan-rand-mac-address") {
            if (!item.set) return qsTr("Random (NetworkManager default)")
            return v === "yes" || v === "true" || v === "1" ? qsTr("Random") : qsTr("Real address")
        }
        if (item.key === "ipv6.ip6-privacy") {
            const mode = item.set ? v : (item.kernelDefault !== undefined ? item.kernelDefault : "")
            const text = mode === "2" ? qsTr("Temporary addresses preferred")
                       : mode === "1" ? qsTr("Temporary addresses, public address preferred")
                       : mode === "0" ? qsTr("Off") : mode === "" ? qsTr("Unknown") : mode
            return item.set ? text : qsTr("%1 (kernel default)").arg(text)
        }
        if (!item.set) return qsTr("Unchanged (NetworkManager default)")
        return v === "stable" ? qsTr("Different per network, stable")
             : v === "random" ? qsTr("Random for every connection")
             : v === "permanent" ? qsTr("Real address")
             : v === "preserve" ? qsTr("Unchanged")
             : v === "stable-ssid" ? qsTr("Different per network name, stable") : v
    }
    function macSubtitle(item) {
        return item.set ? qsTr("%1 = %2  ·  %3").arg(item.key).arg(item.value).arg(item.source)
                        : qsTr("%1 is not set in NetworkManager's configuration").arg(item.key)
    }

    PrivacyBackend {
        id: privacy
        Component.onCompleted: { scan(); refreshAnon() }
        onFailed: (message) => {
            page.revision++
            page.ApplicationWindow.window.toaster.show(message, "danger")
        }
        onSucceeded: (message) => page.ApplicationWindow.window.toaster.show(message, "success")
    }

    // ---- Hide Me -----------------------------------------------------------------------
    NxSection {
        title: qsTr("Hide Me")
        description: privacy.ghostMode
            ? qsTr("GHOST MODE ACTIVE — All identity protections are enabled.")
            : qsTr("Full identity obliteration. Ghost Mode activates every protection at once.")

        // Ghost Mode master control
        NxRow {
            icon: "eye-off"
            title: qsTr("Ghost Mode")
            subtitle: qsTr("MAC random · transparent Tor proxy · DNS-over-Tor · Bluetooth kill · kernel hardening · log wipe")
            NxButton {
                anchors.verticalCenter: parent.verticalCenter
                text: privacy.ghostMode ? qsTr("Disable Ghost Mode") : qsTr("Enable Ghost Mode")
                busy: privacy.anonBusy
                onClicked: privacy.ghostMode ? privacy.disableGhostMode() : privacy.enableGhostMode()
            }
        }

        // Threat Level
        NxRow {
            icon: "shield"
            title: qsTr("Threat Level")
            subtitle: privacy.threatModel === "maximum" ? qsTr("Maximum: all protections active")
                     : privacy.threatModel === "enhanced" ? qsTr("Enhanced: MAC + Tor routing + DNS-over-Tor")
                     : qsTr("Basic: MAC randomization + hostname rotation")
            NxSegmented {
                anchors.verticalCenter: parent.verticalCenter
                model: [qsTr("Basic"), qsTr("Enhanced"), qsTr("Maximum")]
                currentIndex: privacy.threatModel === "maximum" ? 2 : privacy.threatModel === "enhanced" ? 1 : 0
                enabled: !privacy.anonBusy
                onActivated: (i) => privacy.setThreatModel(["basic","enhanced","maximum"][i])
            }
        }

        // Rotate now
        NxRow {
            icon: "rotate-cw"
            title: qsTr("Rotate Identity")
            subtitle: qsTr("New MAC address · new hostname · new Tor exit (uses current threat level)")
            value: privacy.torRunning ? qsTr("Tor: running") : qsTr("Tor: stopped")
            NxButton {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Rotate Now")
                busy: privacy.anonBusy
                onClicked: privacy.rotateIdentity()
            }
        }

        // Transparent Tor proxy
        NxRow {
            icon: "shield"
            title: qsTr("Transparent Tor Proxy")
            subtitle: qsTr("Routes ALL TCP connections and DNS queries through Tor — not just your browser. UDP is blocked while active.")
            value: privacy.transparentProxy ? qsTr("ACTIVE") : qsTr("off")
            Switch {
                anchors.verticalCenter: parent.verticalCenter
                checked: privacy.transparentProxy
                enabled: !privacy.anonBusy
                Accessible.name: qsTr("Transparent Tor Proxy")
                onToggled: {
                    const v = checked
                    checked = Qt.binding(() => privacy.transparentProxy)
                    privacy.enableTransparentProxy(v)
                }
            }
        }

        // DNS-over-Tor
        NxRow {
            icon: "lock"
            title: qsTr("DNS-over-Tor")
            subtitle: qsTr("Routes all DNS queries through Tor. Prevents DNS leaks that expose which hostnames you visit.")
            value: privacy.dnsProtected ? qsTr("ACTIVE") : qsTr("off")
            Switch {
                anchors.verticalCenter: parent.verticalCenter
                checked: privacy.dnsProtected
                enabled: !privacy.anonBusy
                Accessible.name: qsTr("DNS-over-Tor")
                onToggled: {
                    const v = checked
                    checked = Qt.binding(() => privacy.dnsProtected)
                    privacy.enableDnsProtect(v)
                }
            }
        }

        // Bluetooth kill
        NxRow {
            icon: "bluetooth-off"
            title: qsTr("Bluetooth Kill Switch")
            subtitle: qsTr("Blocks Bluetooth via rfkill. Nearby devices cannot detect or track this machine while killed.")
            value: privacy.bluetoothKilled ? qsTr("BLOCKED") : qsTr("active")
            NxButton {
                anchors.verticalCenter: parent.verticalCenter
                text: privacy.bluetoothKilled ? qsTr("Restore") : qsTr("Kill")
                busy: privacy.anonBusy
                onClicked: privacy.killBluetooth(!privacy.bluetoothKilled)
            }
        }

        // USB storage kill
        NxRow {
            icon: "hard-drive"
            title: qsTr("USB Storage Kill Switch")
            subtitle: qsTr("Blocks USB storage devices via udev. Prevents data exfiltration and BadUSB attacks.")
            value: privacy.usbStorageKilled ? qsTr("BLOCKED") : qsTr("allowed")
            NxButton {
                anchors.verticalCenter: parent.verticalCenter
                text: privacy.usbStorageKilled ? qsTr("Restore") : qsTr("Block")
                busy: privacy.anonBusy
                onClicked: privacy.killUsbStorage(!privacy.usbStorageKilled)
            }
        }

        // Kernel hardening
        NxRow {
            icon: "cpu"
            title: qsTr("Kernel Hardening")
            subtitle: qsTr("Applies sysctl: disables TCP timestamps, hides kernel pointers, hardens BPF JIT, enables reverse-path filtering.")
            value: privacy.kernelHardened ? qsTr("applied") : qsTr("system default")
            NxButton {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Apply")
                busy: privacy.anonBusy
                enabled: !privacy.kernelHardened && !privacy.anonBusy
                onClicked: privacy.hardenKernelNow()
            }
        }

        // Auto-rotate
        NxRow {
            icon: "clock"
            title: qsTr("Auto-rotate")
            subtitle: privacy.autoRotateActive
                ? qsTr("Identity rotates automatically. Each cycle briefly drops network connectivity.")
                : qsTr("Rotate MAC address, hostname and Tor exit on a timer.")
            value: privacy.autoRotateActive ? qsTr("active") : qsTr("off")
            NxButton {
                anchors.verticalCenter: parent.verticalCenter
                text: privacy.autoRotateActive ? qsTr("Stop") : qsTr("Every 5 min")
                busy: privacy.anonBusy
                onClicked: privacy.autoRotateActive ? privacy.stopAutoRotate() : privacy.startAutoRotate(300)
            }
        }

        // Log wipe
        NxRow {
            icon: "trash-2"
            title: qsTr("Wipe Logs")
            subtitle: qsTr("Clears systemd journal, all shell histories, thumbnail cache and old /tmp files.")
            NxButton {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Wipe…")
                busy: privacy.anonBusy
                onClicked: wipeLogsDialog.open()
            }
        }

        // Leak test
        NxRow {
            icon: "search"
            title: qsTr("Identity Leak Test")
            subtitle: qsTr("Checks direct IP, Tor exit IP, DNS resolver and IPv6 exposure. Results appear below.")
            NxButton {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Run Test")
                busy: privacy.anonBusy
                onClicked: privacy.runLeakTest()
            }
        }

        // Output log
        Text {
            Layout.fillWidth: true
            visible: privacy.anonOutput !== ""
            text: privacy.anonOutput
            font.family: "monospace"
            font.pixelSize: Theme.fontSizeSmall - 1
            color: Theme.textMuted
            wrapMode: Text.Wrap
            topPadding: Theme.spaceSm
        }

        // NUKE — the big red button
        NxRow {
            icon: "zap"
            title: qsTr("NUKE Identity")
            subtitle: qsTr("Ghost Mode + wipe all logs instantly. The most aggressive single action.")
            NxButton {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("NUKE…")
                busy: privacy.anonBusy
                onClicked: nukeDialog.open()
            }
        }
    }

    // ---- screen lock --------------------------------------------------------------------
    NxSection {
        title: qsTr("Screen lock")
        description: !privacy.lockConfigurable ? qsTr("This build of the Control Center cannot read the screen locker's settings (no KConfig support).")
                   : privacy.lockerRunning ? qsTr("Applied to the running screen locker immediately.")
                   : qsTr("The screen locker (org.kde.screensaver) is not running in this session. Changes are saved and used when it starts.")

        NxRow {
            icon: "lock"
            title: qsTr("Lock the screen automatically")
            subtitle: qsTr("After the time below without keyboard or pointer input.")
            enabled: privacy.lockConfigurable
            Switch {
                anchors.verticalCenter: parent.verticalCenter
                checked: page.live(page.revision, privacy.autolock)
                Accessible.name: qsTr("Lock the screen automatically")
                onToggled: {
                    const wanted = checked
                    checked = Qt.binding(() => page.live(page.revision, privacy.autolock))
                    privacy.setAutolock(wanted)
                }
            }
        }
        NxRow {
            icon: "timer"
            title: qsTr("Lock after")
            enabled: privacy.lockConfigurable && privacy.autolock
            ComboBox {
                id: timeoutBox
                anchors.verticalCenter: parent.verticalCenter
                width: 190
                readonly property var items: page.choices([1, 2, 5, 10, 15, 30, 60], privacy.lockTimeoutMinutes, page.minutesText)
                model: items
                textRole: "text"
                valueRole: "value"
                currentIndex: page.live(page.revision, page.indexOfValue(items, privacy.lockTimeoutMinutes))
                Accessible.name: qsTr("Lock after")
                onActivated: privacy.setLockTimeoutMinutes(currentValue)
            }
        }
        NxRow {
            icon: "moon"
            title: qsTr("Lock when the computer wakes from sleep")
            enabled: privacy.lockConfigurable
            Switch {
                anchors.verticalCenter: parent.verticalCenter
                checked: page.live(page.revision, privacy.lockOnResume)
                Accessible.name: qsTr("Lock when the computer wakes from sleep")
                onToggled: {
                    const wanted = checked
                    checked = Qt.binding(() => page.live(page.revision, privacy.lockOnResume))
                    privacy.setLockOnResume(wanted)
                }
            }
        }
        NxRow {
            icon: "key"
            title: qsTr("Ask for the password")
            subtitle: qsTr("How long after the screen locked itself it can still be dismissed without the password.")
            enabled: privacy.lockConfigurable
            ComboBox {
                id: graceBox
                anchors.verticalCenter: parent.verticalCenter
                width: 190
                readonly property var items: page.choices([0, 5, 15, 30, 60, 300], privacy.lockGraceSeconds, page.graceText)
                model: items
                textRole: "text"
                valueRole: "value"
                currentIndex: page.live(page.revision, page.indexOfValue(items, privacy.lockGraceSeconds))
                Accessible.name: qsTr("Ask for the password")
                onActivated: privacy.setLockGraceSeconds(currentValue)
            }
        }
    }

    // ---- history ------------------------------------------------------------------------
    NxSection {
        title: qsTr("History")

        NxRow {
            icon: "clock"
            title: qsTr("Recently used files")
            subtitle: privacy.recentFilesPresent ? qsTr("The list applications and file dialogs offer under “Recent” (%1)").arg(privacy.recentFilesPath)
                                                 : qsTr("No list exists yet (%1)").arg(privacy.recentFilesPath)
            value: privacy.recentFilesCount === 1 ? qsTr("1 entry") : qsTr("%1 entries").arg(privacy.recentFilesCount)
            NxButton {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Clear…")
                enabled: privacy.recentFilesPresent
                onClicked: clearRecentDialog.open()
            }
        }
        NxRow {
            icon: "apps"
            title: qsTr("Launcher usage")
            subtitle: qsTr("How often each application was started; the launcher sorts by it (%1)").arg(privacy.launcherUsagePath)
            value: privacy.launcherUsageCount === 1 ? qsTr("1 application") : qsTr("%1 applications").arg(privacy.launcherUsageCount)
            NxButton {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Clear…")
                enabled: privacy.launcherUsageCount > 0
                onClicked: clearUsageDialog.open()
            }
        }
    }

    // ---- clean-up -----------------------------------------------------------------------
    NxSection {
        title: qsTr("Clean up")
        description: qsTr("Sizes are measured on disk.")
        headerActions: [
            NxButton {
                text: qsTr("Measure again")
                iconName: "refresh"
                busy: privacy.scanning
                onClicked: { privacy.refresh(); privacy.scan() }
            }
        ]

        Repeater {
            model: privacy.cleanup
            NxRow {
                id: cleanupRow
                required property var modelData
                icon: page.cleanupIcon(modelData.id)
                title: page.cleanupTitle(modelData.id)
                subtitle: page.cleanupDescription(modelData)
                value: page.cleanupValue(modelData)
                NxButton {
                    anchors.verticalCenter: parent.verticalCenter
                    text: cleanupRow.modelData.id === "trash" ? qsTr("Empty…") : qsTr("Delete…")
                    busy: privacy.cleaning === cleanupRow.modelData.id
                    enabled: !privacy.scanning && cleanupRow.modelData.files > 0
                    onClicked: {
                        page.cleanId = cleanupRow.modelData.id
                        cleanDialog.open()
                    }
                }
            }
        }
    }

    // ---- network identity ---------------------------------------------------------------
    NxSection {
        title: qsTr("Network identity")
        description: !privacy.networkManagerInstalled ? qsTr("NetworkManager is not installed; these are the values its configuration files contain.")
                   : privacy.shadowPrivacyFile !== "" ? qsTr("System-wide defaults of NetworkManager; the SHADOW privacy defaults are installed (%1). A network can override them in its own settings.").arg(privacy.shadowPrivacyFile)
                   : qsTr("System-wide defaults of NetworkManager. The SHADOW privacy defaults (80-shadow-privacy.conf) are not installed. A network can override the defaults in its own settings.")

        Repeater {
            model: privacy.macSettings
            NxRow {
                required property var modelData
                icon: modelData.key === "ipv6.ip6-privacy" ? "globe" : modelData.key.indexOf("ethernet") === 0 ? "ethernet" : "wifi"
                title: page.macTitle(modelData.key)
                subtitle: page.macSubtitle(modelData)
                value: page.macValue(modelData)
            }
        }
    }

    NxConfirmDialog {
        id: clearRecentDialog
        title: qsTr("Clear the list of recently used files?")
        message: qsTr("Applications will no longer offer these files under “Recent”. The files themselves are not touched.")
        confirmText: qsTr("Clear")
        destructive: true
        onConfirmed: privacy.clearRecentFiles()
    }
    NxConfirmDialog {
        id: clearUsageDialog
        title: qsTr("Clear the launcher's usage history?")
        message: qsTr("The launcher forgets how often each application was started and sorts applications by name until new counts build up.")
        confirmText: qsTr("Clear")
        destructive: true
        onConfirmed: privacy.clearLauncherUsage()
    }
    NxConfirmDialog {
        id: cleanDialog
        title: page.cleanId === "trash" ? qsTr("Empty the trash?")
             : page.cleanId === "thumbnails" ? qsTr("Delete the thumbnail cache?") : qsTr("Delete old temporary files?")
        message: page.cleanupConfirm(page.cleanId)
        confirmText: page.cleanId === "trash" ? qsTr("Empty trash") : qsTr("Delete")
        destructive: true
        onConfirmed: privacy.clean(page.cleanId)
    }
    NxConfirmDialog {
        id: wipeLogsDialog
        title: qsTr("Wipe logs?")
        message: qsTr("This permanently deletes the systemd journal, all shell histories (.bash_history, .zsh_history), thumbnail caches and /tmp files older than 1 hour. The files themselves are not affected.")
        confirmText: qsTr("Wipe logs")
        destructive: true
        onConfirmed: privacy.wipeLogsNow()
    }
    NxConfirmDialog {
        id: nukeDialog
        title: qsTr("NUKE identity?")
        message: qsTr("This runs Ghost Mode (new MAC, new hostname, transparent Tor proxy, DNS-over-Tor, Bluetooth kill, kernel hardening) and immediately wipes all logs. This is the most aggressive action available. Use only when you need instant, complete identity obliteration.")
        confirmText: qsTr("NUKE")
        destructive: true
        onConfirmed: privacy.nukeIdentity()
    }
}
