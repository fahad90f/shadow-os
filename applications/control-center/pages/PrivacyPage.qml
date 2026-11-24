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
        return key === "connection.stable-id" ? qsTr("When private addresses change")
             : key === "wifi.scan-rand-mac-address" ? qsTr("Hardware address while searching for Wi-Fi networks")
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
        if (item.key === "connection.stable-id") {
            if (!item.set) return qsTr("Not set (NetworkManager default: fixed per network)")
            if (v === "") return qsTr("Fixed per network")
            return v.indexOf("${BOOT}") >= 0 ? qsTr("At every boot") : v === "${CONNECTION}" ? qsTr("Fixed per network") : v
        }
        if (!item.set) return qsTr("Unchanged (NetworkManager default)")
        return v === "stable" ? qsTr("Private, different per network")
             : v === "random" ? qsTr("Random for every connection")
             : v === "permanent" ? qsTr("Real address")
             : v === "preserve" ? qsTr("Unchanged")
             : v === "stable-ssid" ? qsTr("Different per network name, stable") : v
    }
    function macSubtitle(item) {
        if (!item.set)
            return qsTr("%1 is not set in NetworkManager's configuration").arg(item.key)
        // A named section ([connection-…] without match-device) applies to
        // every device and comes before the plain [connection] section.
        const key = item.group && item.group !== item.section ? "[" + item.group + "] " + item.key : item.key
        return qsTr("%1 = %2  ·  %3").arg(key).arg(item.value).arg(item.source)
    }

    PrivacyBackend {
        id: privacy
        Component.onCompleted: { scan(); refreshAnon(); refreshHarden() }
        // Hide Me changes the kill switch too: read its state file again at
        // once rather than when the watcher notices.
        onFailed: (message) => {
            page.revision++
            guard.refresh()
            page.ApplicationWindow.window.toaster.show(message, "danger")
        }
        onSucceeded: (message) => {
            guard.refresh()
            page.ApplicationWindow.window.toaster.show(message, "success")
        }
    }

    // ---- kill switch and panic (backends/privacykillswitchbackend.h) ------------------
    KillSwitchBackend {
        id: guard
        onFailed: (message) => {
            page.revision++
            page.ApplicationWindow.window.toaster.show(message, "danger")
        }
        onSucceeded: (message) => page.ApplicationWindow.window.toaster.show(message, "success")
    }
    readonly property var ks: guard.killSwitch
    readonly property var pn: guard.panic
    readonly property var liveCheck: guard.liveCheck
    readonly property var ksModes: ["off", "tor", "vpn", "tor-or-vpn"]
    property string pendingMode: ""

    // Hide Me's view of the kill switch, from the same document as above.
    // Tor only with the local network allowed is not Hide Me's Tor only: the
    // private ranges and multicast leave directly (Hide Me sets --no-lan).
    readonly property bool ksTorOn: ks.state === "on" && ks.mode === "tor" && ks.lan !== true
    readonly property string ksModeInForce: ks.state !== "on" ? qsTr("off")
        : ks.lan === true ? qsTr("%1, local network allowed").arg(ks.modeName) : ks.modeName
    readonly property bool hideMeHoldsKs: (ks.hideMe || []).length > 0
    readonly property bool ghostOn: privacy.ghostMode && ksTorOn
    readonly property bool ghostBroken: privacy.ghostMode && !ksTorOn && ks.state !== "unknown"
    property string ghostKind: "ghost"          // what ghostDialog confirms: "ghost" | "maximum"

    function durationText(seconds) {
        if (seconds % 60 === 0) return seconds === 60 ? qsTr("1 minute") : qsTr("%1 minutes").arg(seconds / 60)
        return qsTr("%1 seconds").arg(seconds)
    }
    function hideMeKillSwitchText() {
        if (!guard.toolInstalled)
            return qsTr("Needs the network kill switch (shadow-killswitch, package shadow-hardening), which is not installed.")
        if (ks.state === "error")
            return qsTr("The kill switch reports a problem (see Network kill switch above), so traffic may not be limited to Tor. Turning this on sets Tor only again.")
        if (ksTorOn && !hideMeHoldsKs)
            return qsTr("On: Tor only was chosen in Network kill switch above; change it there.")
        if (ksTorOn) {
            const holders = ks.hideMe || []
            return qsTr("On: only Tor can send, IPv6 and forwarded traffic included. Turning it off puts back the mode in force before Hide Me%1.")
                       .arg(holders.indexOf("proxy") >= 0 || holders.indexOf("ghost") >= 0 ? qsTr(" and also turns off the transparent proxy") : "")
                   + (ks.runtimeOverride ? " " + qsTr("Ends with a restart.") : "")
        }
        if (ks.state === "on" && ks.mode === "tor")
            return qsTr("The network kill switch is in mode Tor only with the local network allowed (set above): the local network is reached directly, not through Tor. Turning this on blocks the local network too until you turn it off here; then it is allowed again.")
        if (ks.state === "on")
            return qsTr("The network kill switch is in mode “%1” (set above). Turning this on changes it to Tor only until you turn it off here; then “%1” is put back.").arg(ks.modeName)
        if (ks.state === "bypassed")
            return qsTr("Off for this boot (started with shadow.killswitch=off). Turning it on here loads Tor only at once.")
        return qsTr("Nothing leaves except through Tor, IPv6 and forwarded traffic (virtual machines, containers) included; programs that do not use Tor get no connection. Lasts until you turn it off or restart. It is the network kill switch above in mode Tor only.")
    }

    function ksModeName(mode) {
        return mode === "tor" ? qsTr("Tor only") : mode === "vpn" ? qsTr("VPN only")
             : mode === "tor-or-vpn" ? qsTr("Tor or VPN") : mode === "off" ? qsTr("Off") : qsTr("Unknown")
    }
    function ksModeText(mode) {
        return mode === "tor" ? qsTr("Only Tor can reach the network. Programs that do not use Tor (or Hide Me's transparent proxy) get no connection.")
             : mode === "vpn" ? qsTr("Only traffic through a VPN tunnel, and to your VPN servers, can leave. When the VPN is down, nothing leaves.")
             : mode === "tor-or-vpn" ? qsTr("Traffic through Tor or through a VPN tunnel can leave; nothing else.")
             : mode === "off" ? qsTr("Traffic is not restricted. Programs use your normal connection when Tor or a VPN stops.")
             : qsTr("The mode in force is not known.")
    }
    function when(iso) {
        if (!iso) return ""
        const d = new Date(iso)
        return isNaN(d.getTime()) ? iso : d.toLocaleString(Qt.locale(), Locale.ShortFormat)
    }
    function hideMeText(holders) {
        const names = holders.map(h => h === "ghost" ? qsTr("Ghost Mode") : h === "proxy" ? qsTr("the transparent Tor proxy")
                                                                        : qsTr("its “Kill switch: Tor only” switch"))
        return qsTr("Hide Me turned the kill switch on for %1. Turning that off in Hide Me below puts back the mode that was in force before.")
            .arg(names.join(qsTr(" and ")))
    }
    function ksStateTitle() {
        const s = ks.state
        if (s === "on") return qsTr("On: %1").arg(ks.modeName)
        if (s === "off") return qsTr("Off")
        if (s === "bypassed") return qsTr("Off for this boot")
        if (s === "error") return ks.active ? qsTr("%1 — not working").arg(ks.modeName || qsTr("Problem")) : qsTr("Problem")
        return guard.toolInstalled ? qsTr("No status recorded") : qsTr("Not installed")
    }
    function ksStateSubtitle() {
        const s = ks.state
        if (s === "on") {
            let parts = []
            if (ks.since) parts.push(qsTr("since %1").arg(page.when(ks.since)))
            parts.push(ks.lan ? qsTr("local network allowed") : qsTr("local network blocked"))
            if (ks.droppedPackets >= 0) parts.push(qsTr("%1 packets blocked").arg(Number(ks.droppedPackets).toLocaleString(Qt.locale(), "f", 0)))
            return parts.join("  ·  ")
        }
        if (s === "off") return qsTr("Nothing is blocked. Programs use your normal connection.")
        if (s === "bypassed") return qsTr("Started with shadow.killswitch=off on the kernel command line. At the next normal start: %1.").arg(ks.persistentModeName)
        if (s === "error") return ks.updated ? qsTr("Recorded by shadow-killswitch at %1; details below.").arg(page.when(ks.updated))
                                             : qsTr("Details below.")
        return guard.toolInstalled ? qsTr("shadow-killswitch has not recorded a state in this boot (%1), so whether anything is blocked is unknown.").arg(guard.statusFile)
                                   : qsTr("The kill switch tool (%1, package shadow-hardening) is not installed.").arg(guard.toolPath)
    }
    function liveText() {
        const c = liveCheck
        if (c.state === "checking") return qsTr("Checking…")
        if (c.state === "failed") return qsTr("Could not check")
        if (c.state !== "done") return qsTr("Not checked")
        if (c.outdated) return qsTr("Checked before the last change")
        const t = c.killSwitchTable
        return t === true ? qsTr("Table in the kernel") : t === false ? qsTr("No table in the kernel") : qsTr("Could not check")
    }
    function liveSubtitle() {
        const c = liveCheck
        if (c.state === "failed" || (c.state === "done" && c.killSwitchTable !== true && c.killSwitchTable !== false))
            return c.reason || ""
        if (c.state === "done")
            return qsTr("Asked shadow-security-status at %1. The status above is what shadow-killswitch recorded%2.")
                .arg(page.when(c.checked)).arg(ks.updated ? qsTr(" at %1").arg(page.when(ks.updated)) : "")
        return qsTr("Asks the kernel whether the kill switch table is loaded right now.")
    }
    function panicTitle() {
        const s = pn.state
        if (s === "engaged") return qsTr("Engaged: cut off from every network")
        if (s === "error") return qsTr("Problem")
        if (s === "released") return qsTr("Not engaged")
        return qsTr("Not engaged")
    }
    function panicSubtitle() {
        const s = pn.state
        if (s === "engaged") return pn.since ? qsTr("Since %1. Only an administrator can reconnect; a restart also ends it.").arg(page.when(pn.since))
                                             : qsTr("Only an administrator can reconnect; a restart also ends it.")
        if (s === "error") return qsTr("Details below.")
        if (s === "released") return qsTr("The last panic was released at %1.").arg(page.when(pn.released))
        return guard.panicToolInstalled ? qsTr("Press the shortcut below to cut every connection at once.")
                                        : qsTr("The panic tool (shadow-panic, package shadow-hardening) is not installed.")
    }

    NxConfirmDialog {
        id: engageDialog
        title: qsTr("Turn on the kill switch: %1?").arg(page.ksModeName(page.pendingMode))
        message: qsTr("%1\n\nConnections that this mode does not allow are closed now (downloads, calls and other open connections stop). %2")
            .arg(page.ksModeText(page.pendingMode))
            .arg(guard.keepAfterRestart
                 ? qsTr("The mode is also loaded at every start, before any network connects: if %1 then, there is no network until you turn it off here (or start once with shadow.killswitch=off).")
                     .arg(page.pendingMode === "tor" ? qsTr("Tor does not work") : page.pendingMode === "vpn" ? qsTr("the VPN does not connect")
                          : qsTr("neither Tor nor a VPN works"))
                 : qsTr("It lasts until the next restart."))
        confirmText: qsTr("Turn on")
        onConfirmed: guard.setMode(page.pendingMode)
    }
    NxConfirmDialog {
        id: releaseDialog
        title: qsTr("Reconnect this computer?")
        message: qsTr("This releases the panic: its firewall table is removed, NetworkManager networking is turned back on if the panic turned it off, and the radios the panic blocked are unblocked. The kill switch is not changed. An administrator password is needed.")
        confirmText: qsTr("Reconnect")
        onConfirmed: guard.releasePanic()
    }

    // ---- network kill switch -----------------------------------------------------------
    NxSection {
        title: qsTr("Network kill switch")
        description: qsTr("While it is on, nothing leaves this computer except through Tor or a VPN. If Tor or the VPN stops, the connection stops too, instead of falling back to your normal connection.")
        headerActions: [
            NxIconButton {
                iconName: "refresh"
                tooltip: qsTr("Read the state again")
                onClicked: guard.refresh()
            }
        ]

        NxRow {
            icon: page.ks.state === "on" ? "shield-check" : page.ks.state === "error" ? "shield-alert" : "shield-off"
            iconColor: page.ks.state === "on" ? Theme.success : page.ks.state === "error" ? Theme.danger
                     : page.ks.state === "bypassed" ? Theme.warning : Theme.textMuted
            title: page.ksStateTitle()
            subtitle: page.ksStateSubtitle()
            NxBadge {
                anchors.verticalCenter: parent.verticalCenter
                text: page.ks.protected ? qsTr("Protected") : page.ks.state === "error" ? qsTr("Not protected")
                    : page.ks.state === "off" || page.ks.state === "bypassed" ? qsTr("Off") : qsTr("Unknown")
                kind: page.ks.protected ? "success" : page.ks.state === "error" ? "danger"
                    : page.ks.state === "bypassed" ? "warning" : "neutral"
            }
        }

        NxBanner {
            visible: page.ks.state === "error"
            Layout.leftMargin: Theme.spaceLg
            Layout.rightMargin: Theme.spaceLg
            Layout.bottomMargin: Theme.spaceMd
            kind: "danger"
            // "Traffic may leave" only when the mode's table is not in force; a problem
            // with the table loaded (a broken configuration while it fails closed) is not a leak.
            title: page.ks.trafficMayLeave ? qsTr("Traffic may leave outside Tor or the VPN")
                 : page.ks.active ? qsTr("Kill switch problem") : qsTr("The kill switch is not in the state it reports")
            message: page.ks.errorText + "\n" + (guard.toolInstalled && page.ks.active
                     ? qsTr("Load it again, or check from a terminal: sudo shadow-killswitch status")
                     : qsTr("Check from a terminal: sudo shadow-killswitch status"))
            actionText: guard.toolInstalled && page.ks.active ? qsTr("Load again") : ""
            onActionTriggered: guard.reassert()
        }
        NxBanner {
            visible: page.ks.state === "bypassed"
            Layout.leftMargin: Theme.spaceLg
            Layout.rightMargin: Theme.spaceLg
            Layout.bottomMargin: Theme.spaceMd
            kind: "warning"
            title: qsTr("The kill switch is off for this boot")
            message: qsTr("The computer was started with shadow.killswitch=off (the recovery escape hatch), so the saved mode “%1” is not loaded. Nothing is blocked now. Choosing a mode below loads it at once.").arg(page.ks.persistentModeName)
        }
        NxBanner {
            visible: page.ks.config !== undefined && page.ks.config.problem !== ""
            Layout.leftMargin: Theme.spaceLg
            Layout.rightMargin: Theme.spaceLg
            Layout.bottomMargin: Theme.spaceMd
            kind: "warning"
            title: qsTr("The saved setting cannot be used")
            message: qsTr("%1 %2. At start-up the kill switch therefore fails closed (Tor or VPN). Choosing a mode below writes a new file.")
                .arg(guard.configFile).arg(page.ks.config ? page.ks.config.problem : "")
        }

        NxRow {
            icon: "shield"
            title: qsTr("Mode")
            subtitle: page.ksModeText(page.ks.mode)
            enabled: guard.toolInstalled && !guard.busy && !privacy.anonBusy
            NxSegmented {
                anchors.verticalCenter: parent.verticalCenter
                model: [qsTr("Off"), qsTr("Tor only"), qsTr("VPN only"), qsTr("Tor or VPN")]
                currentIndex: page.live(page.revision, page.ksModes.indexOf(page.ks.mode))
                Accessible.name: qsTr("Kill switch mode")
                onActivated: (index) => {
                    const mode = page.ksModes[index]
                    if (guard.closesConnections(mode, page.ks.lan)) {
                        page.pendingMode = mode
                        engageDialog.open()
                    } else {
                        guard.setMode(mode)
                    }
                }
            }
        }
        NxRow {
            icon: "network"
            title: qsTr("Allow the local network")
            subtitle: page.ks.active
                ? qsTr("Printers, file shares and other devices on your own network (private and link-local addresses). Off by default: on a large company network that is a lot of the world.")
                : qsTr("Can be changed while the kill switch is on.")
            enabled: guard.toolInstalled && !guard.busy && page.ks.active && page.ks.state !== "unknown"
            Switch {
                anchors.verticalCenter: parent.verticalCenter
                checked: page.live(page.revision, page.ks.lan === true)
                Accessible.name: qsTr("Allow the local network")
                onToggled: {
                    const wanted = checked
                    checked = Qt.binding(() => page.live(page.revision, page.ks.lan === true))
                    guard.setLan(wanted)
                }
            }
        }
        NxRow {
            icon: "power"
            title: qsTr("Keep after a restart")
            subtitle: (guard.keepAfterRestart
                       ? qsTr("Changes made here are saved and loaded at start-up, before any network connects.")
                       : qsTr("Changes made here last until the next restart."))
                      + " " + qsTr("At start-up: %1.").arg(page.ks.persistentModeName || qsTr("unknown"))
            enabled: guard.toolInstalled && !guard.busy
            Switch {
                anchors.verticalCenter: parent.verticalCenter
                checked: page.live(page.revision, guard.keepAfterRestart)
                Accessible.name: qsTr("Keep after a restart")
                onToggled: {
                    const wanted = checked
                    checked = Qt.binding(() => page.live(page.revision, guard.keepAfterRestart))
                    guard.keepAfterRestart = wanted
                }
            }
        }
        NxRow {
            visible: page.ks.mode === "vpn" || page.ks.mode === "tor-or-vpn"
            icon: "server"
            title: qsTr("VPN servers allowed")
            subtitle: page.ks.skippedHostnames > 0
                ? qsTr("%1 VPN profile(s) name the server by host name, which is never looked up: add its address with: sudo shadow-killswitch vpn-endpoint add <ip>:<port>/udp").arg(page.ks.skippedHostnames)
                : qsTr("Servers of saved OpenVPN profiles and active WireGuard connections, and servers added with shadow-killswitch vpn-endpoint add.")
            value: String(page.ks.endpoints)
        }
        NxRow {
            visible: page.hideMeHoldsKs && page.ks.mode === "tor"
            icon: "eye-off"
            title: qsTr("Turned on by Hide Me")
            subtitle: page.hideMeText(page.ks.hideMe || [])
        }
        NxRow {
            icon: "cpu"
            iconColor: page.liveCheck.state === "done" && !page.liveCheck.outdated && page.ks.state === "error" ? Theme.danger : Theme.textSecondary
            title: qsTr("Checked against the kernel")
            subtitle: page.liveSubtitle()
            value: page.liveText()
            NxButton {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Check now")
                busy: page.liveCheck.state === "checking"
                onClicked: guard.checkLive()
            }
        }
        NxRow {
            icon: "alert-triangle"
            title: qsTr("What it blocks besides leaks")
            subtitle: qsTr("Hotel and train log-in pages, printers and file shares (unless the local network is allowed), virtual machines and containers, and time synchronisation. If you cannot get online, choose Off — or at the boot menu add shadow.killswitch=off for one start.")
        }
        Text {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spaceLg
            Layout.rightMargin: Theme.spaceLg
            Layout.bottomMargin: Theme.spaceMd
            visible: (page.ks.warnings || []).length > 0
            text: qsTr("Last change: %1").arg((page.ks.warnings || []).join("; "))
            color: Theme.textMuted
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
            wrapMode: Text.Wrap
        }
    }

    // ---- panic ------------------------------------------------------------------------
    NxSection {
        title: qsTr("Panic")
        description: qsTr("Cuts this computer off every network at once — wired, Wi-Fi and VPN — and, as set below, turns the radios off, locks the screen and clears the clipboard. Starting it needs no password; reconnecting needs an administrator. A restart also ends it.")

        NxRow {
            icon: "zap"
            iconColor: page.pn.state === "engaged" ? Theme.warning : page.pn.state === "error" ? Theme.danger : Theme.textSecondary
            title: page.panicTitle()
            subtitle: page.panicSubtitle()
            NxButton {
                anchors.verticalCenter: parent.verticalCenter
                visible: page.pn.engaged === true || page.pn.loaded === true
                         || (page.liveCheck.state === "done" && page.liveCheck.panicTable === true)
                text: qsTr("Reconnect…")
                busy: guard.busyAction === "release"
                enabled: guard.panicToolInstalled && !guard.busy
                onClicked: releaseDialog.open()
            }
        }
        NxRow {
            icon: "keyboard"
            title: qsTr("Keyboard shortcut")
            subtitle: guard.panicShortcut.note || qsTr("Works anywhere in the desktop session, also when the Control Center is closed.")
            value: guard.panicShortcut.keys || qsTr("None")
            monoValue: true
        }
        NxRow {
            icon: "airplane"
            title: qsTr("Turn off Wi-Fi, Bluetooth and mobile radios")
            subtitle: qsTr("Only radios that were on; reconnecting turns those back on.")
            enabled: false
            Switch { anchors.verticalCenter: parent.verticalCenter; checked: page.pn.config ? page.pn.config.rfkill === true : true; Accessible.name: qsTr("Turn off radios during a panic") }
        }
        NxRow {
            icon: "lock"
            title: qsTr("Lock the screen")
            enabled: false
            Switch { anchors.verticalCenter: parent.verticalCenter; checked: page.pn.config ? page.pn.config.lockSession === true : true; Accessible.name: qsTr("Lock the screen during a panic") }
        }
        NxRow {
            icon: "paste"
            title: qsTr("Clear the clipboard")
            enabled: false
            Switch { anchors.verticalCenter: parent.verticalCenter; checked: page.pn.config ? page.pn.config.clearClipboard === true : true; Accessible.name: qsTr("Clear the clipboard during a panic") }
        }
        NxRow {
            icon: "info"
            title: qsTr("These options are read-only here")
            subtitle: (page.pn.config && page.pn.config.problem !== ""
                       ? qsTr("%1 %2, so the defaults (all on) apply. ").arg(guard.panicConfigFile).arg(page.pn.config.problem)
                       : page.pn.config && page.pn.config.source === "file" ? qsTr("Read from %1. ").arg(guard.panicConfigFile)
                       : qsTr("%1 does not exist, so the defaults (all on) apply. ").arg(guard.panicConfigFile))
                      + qsTr("shadow-panic has no command to change them yet; an administrator can set rfkill, lock_session and clear_clipboard to true or false in that file.")
        }
        NxBanner {
            visible: page.pn.state === "error"
            Layout.leftMargin: Theme.spaceLg
            Layout.rightMargin: Theme.spaceLg
            Layout.bottomMargin: Theme.spaceMd
            kind: "danger"
            title: qsTr("The panic is not in the state it reports")
            message: page.pn.errorText
        }
        Text {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spaceLg
            Layout.rightMargin: Theme.spaceLg
            Layout.bottomMargin: Theme.spaceMd
            visible: page.pn.state === "engaged" && (page.pn.warnings || []).length > 0
            text: qsTr("Not every step worked: %1").arg((page.pn.warnings || []).join("; "))
            color: Theme.textMuted
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
            wrapMode: Text.Wrap
        }
    }

    // ---- Hide Me -----------------------------------------------------------------------
    // shadow-anon (backends/privacybackend.h). Whether the kill switch is in
    // force is read from 'guard' (shadow-killswitch's status file), exactly as
    // in the Network kill switch section above, never from Hide Me's own
    // record: the two sections cannot disagree.
    NxSection {
        title: qsTr("Hide Me")
        description: page.ghostOn
            ? qsTr("Ghost Mode is on: the kill switch lets only Tor out, and all TCP and DNS of every program go through Tor. It ends with a restart.")
            : qsTr("How this computer identifies itself on networks, and Tor for all its traffic (shadow-anon). Ghost Mode turns every protection on at once; it ends with a restart.")

        NxBanner {
            visible: page.ghostBroken
            Layout.leftMargin: Theme.spaceLg
            Layout.rightMargin: Theme.spaceLg
            Layout.bottomMargin: Theme.spaceMd
            kind: "danger"
            title: qsTr("Ghost Mode is on, but its kill switch is not")
            message: qsTr("The network kill switch above is not in “Tor only” (%1), so traffic is not limited to Tor. Turn Ghost Mode off and on again.")
                .arg(page.ksStateTitle())
        }
        NxBanner {
            visible: privacy.anonError !== "" && !privacy.anonBusy
            Layout.leftMargin: Theme.spaceLg
            Layout.rightMargin: Theme.spaceLg
            Layout.bottomMargin: Theme.spaceMd
            kind: "warning"
            title: qsTr("The last Hide Me change did not complete")
            message: privacy.anonError
        }

        // Ghost Mode master control
        NxRow {
            icon: "eye-off"
            iconColor: page.ghostOn ? Theme.success : page.ghostBroken ? Theme.danger : Theme.textSecondary
            title: qsTr("Ghost Mode")
            subtitle: qsTr("The kill switch first (only Tor can send); if it cannot be engaged, nothing else is changed. Then a new network address, IPv6 privacy, a new host name, UTC time zone, kernel hardening, Bluetooth off, the transparent Tor proxy, DNS-over-Tor and a log wipe.")
            NxButton {
                anchors.verticalCenter: parent.verticalCenter
                text: privacy.ghostMode ? qsTr("Turn off") : qsTr("Turn on…")
                busy: privacy.anonAction === "ghost-on" || privacy.anonAction === "ghost-off"
                enabled: privacy.anonToolInstalled && !privacy.anonBusy
                onClicked: {
                    if (privacy.ghostMode) {
                        privacy.disableGhostMode()
                    } else {
                        page.ghostKind = "ghost"
                        ghostDialog.open()
                    }
                }
            }
        }

        // Threat Level
        NxRow {
            icon: "shield"
            title: qsTr("Threat level")
            subtitle: privacy.threatModel === "maximum"
                      ? qsTr("Maximum: Ghost Mode without a new network address — the kill switch first, the transparent Tor proxy, DNS-over-Tor, Bluetooth off, kernel hardening and a log wipe.")
                      : privacy.threatModel === "enhanced"
                        ? qsTr("Enhanced: IPv6 privacy, a new host name, new Tor circuits and DNS-over-Tor. The transparent proxy and a kill switch Hide Me turned on are turned off.")
                        : qsTr("Basic: IPv6 privacy addresses and a new host name. Hide Me's transparent proxy, DNS-over-Tor and kill switch are turned off. No level changes the network address.")
            NxSegmented {
                anchors.verticalCenter: parent.verticalCenter
                model: [qsTr("Basic"), qsTr("Enhanced"), qsTr("Maximum")]
                currentIndex: page.live(page.revision, privacy.threatModel === "maximum" ? 2 : privacy.threatModel === "enhanced" ? 1 : 0)
                enabled: privacy.anonToolInstalled && !privacy.anonBusy
                Accessible.name: qsTr("Threat level")
                onActivated: (i) => {
                    if (i === 2) {
                        page.ghostKind = "maximum"
                        ghostDialog.open()
                    } else {
                        privacy.setThreatModel(i === 1 ? "enhanced" : "basic")
                    }
                }
            }
        }

        // Hide Me's kill switch: shadow-anon hands it to shadow-killswitch (mode tor)
        NxRow {
            icon: page.ksTorOn ? "shield-check" : page.ks.state === "error" ? "shield-alert" : "shield-off"
            iconColor: page.ksTorOn ? Theme.success : page.ks.state === "error" ? Theme.danger : Theme.textSecondary
            title: qsTr("Kill switch: Tor only")
            subtitle: page.hideMeKillSwitchText()
            // Another mode in force is named, so this row never reads "off"
            // while the section above reads "on".
            value: page.ksTorOn ? qsTr("on") : page.ks.state === "error" ? qsTr("problem")
                 : page.ksModeInForce
            Switch {
                anchors.verticalCenter: parent.verticalCenter
                checked: page.live(page.revision, page.ksTorOn)
                // Tor only set in the Network kill switch section is changed there.
                enabled: guard.toolInstalled && privacy.anonToolInstalled && !privacy.anonBusy && !guard.busy
                         && !(page.ksTorOn && !page.hideMeHoldsKs)
                Accessible.name: qsTr("Kill switch, Tor only")
                onToggled: {
                    const wanted = checked
                    checked = Qt.binding(() => page.live(page.revision, page.ksTorOn))
                    if (wanted)
                        hideMeKillSwitchDialog.open()
                    else
                        privacy.enableKillSwitch(false)
                }
            }
        }

        // Rotate now
        NxRow {
            icon: "rotate-cw"
            title: qsTr("Rotate identity")
            subtitle: qsTr("A new network address on every connected network (still one per network; at most once in 5 minutes; a short reconnection; networks with an address setting of their own keep it), a new host name and new Tor circuits.")
            value: privacy.torRunning ? qsTr("Tor: running") : qsTr("Tor: stopped")
            NxButton {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Rotate now")
                busy: privacy.anonAction === "rotate"
                enabled: privacy.anonToolInstalled && !privacy.anonBusy
                onClicked: privacy.rotateIdentity()
            }
        }

        // Transparent Tor proxy
        NxRow {
            icon: "shield"
            iconColor: privacy.transparentProxy && !page.ksTorOn ? Theme.danger : Theme.textSecondary
            title: qsTr("Transparent Tor proxy")
            subtitle: qsTr("All TCP connections and DNS lookups of every program go through Tor, .onion addresses included. The kill switch (Tor only) is turned on first, so UDP and IPv6 are blocked instead of leaking; turning the proxy off releases it unless something else in Hide Me still needs it.")
            value: !privacy.transparentProxy ? qsTr("off") : page.ksTorOn ? qsTr("ACTIVE") : qsTr("ACTIVE, no kill switch")
            Switch {
                anchors.verticalCenter: parent.verticalCenter
                checked: page.live(page.revision, privacy.transparentProxy)
                enabled: privacy.anonToolInstalled && !privacy.anonBusy && !guard.busy
                Accessible.name: qsTr("Transparent Tor proxy")
                onToggled: {
                    const v = checked
                    checked = Qt.binding(() => page.live(page.revision, privacy.transparentProxy))
                    privacy.enableTransparentProxy(v)
                }
            }
        }

        // DNS-over-Tor
        NxRow {
            icon: "lock"
            title: qsTr("DNS-over-Tor")
            subtitle: qsTr("Every DNS lookup goes to Tor (through systemd-resolved where it runs). Kept after a restart; start once with shadow.hideme=off to skip it. Names in a network's own domain (such as a router's “.lan” names) go to Tor as well, so they do not resolve while it is on.")
            value: privacy.dnsProtected ? qsTr("ACTIVE") : qsTr("off")
            Switch {
                anchors.verticalCenter: parent.verticalCenter
                checked: page.live(page.revision, privacy.dnsProtected)
                enabled: privacy.anonToolInstalled && !privacy.anonBusy
                Accessible.name: qsTr("DNS-over-Tor")
                onToggled: {
                    const v = checked
                    checked = Qt.binding(() => page.live(page.revision, privacy.dnsProtected))
                    privacy.enableDnsProtect(v)
                }
            }
        }

        // Bluetooth kill
        NxRow {
            icon: "bluetooth-off"
            title: qsTr("Bluetooth kill switch")
            subtitle: qsTr("Blocks Bluetooth with rfkill until you restore it; nearby devices cannot see or track this computer meanwhile.")
            value: privacy.bluetoothKilled ? qsTr("BLOCKED") : qsTr("allowed")
            NxButton {
                anchors.verticalCenter: parent.verticalCenter
                text: privacy.bluetoothKilled ? qsTr("Restore") : qsTr("Block")
                busy: privacy.anonAction === "bluetooth"
                enabled: privacy.anonToolInstalled && !privacy.anonBusy
                onClicked: privacy.killBluetooth(!privacy.bluetoothKilled)
            }
        }

        // USB storage kill
        NxRow {
            icon: "usb"
            title: qsTr("USB storage kill switch")
            subtitle: qsTr("USB storage devices plugged in from now on are refused (a udev rule, kept after a restart). Other USB devices, such as keyboards and network adapters, are not blocked.")
            value: privacy.usbStorageKilled ? qsTr("BLOCKED") : qsTr("allowed")
            NxButton {
                anchors.verticalCenter: parent.verticalCenter
                text: privacy.usbStorageKilled ? qsTr("Restore") : qsTr("Block")
                busy: privacy.anonAction === "usb"
                enabled: privacy.anonToolInstalled && !privacy.anonBusy
                onClicked: privacy.killUsbStorage(!privacy.usbStorageKilled)
            }
        }

        // Kernel hardening
        NxRow {
            icon: "cpu"
            title: qsTr("Kernel hardening")
            subtitle: qsTr("sysctl settings, kept after a restart: no TCP timestamps, kernel pointers and dmesg hidden, BPF JIT hardened, loose reverse-path filtering, no ICMP redirects or source routing, ptrace limited to parent processes.")
            value: privacy.kernelHardened ? qsTr("applied") : qsTr("system default")
            NxButton {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Apply")
                busy: privacy.anonAction === "harden-kernel"
                enabled: privacy.anonToolInstalled && !privacy.kernelHardened && !privacy.anonBusy
                onClicked: privacy.hardenKernelNow()
            }
        }

        // Auto-rotate: new Tor circuits on a timer (shadow-anon-rotate.timer)
        NxRow {
            icon: "clock"
            title: qsTr("Auto-rotate Tor circuits")
            subtitle: privacy.autoRotateActive
                ? qsTr("New Tor circuits (a new exit for new connections) every %1. Only Tor circuits: the network address and host name change only with Rotate now, and the connection is never dropped. Does nothing while Tor is not running.")
                    .arg(page.durationText(privacy.autoRotateIntervalSeconds))
                : qsTr("New Tor circuits on a timer, at least %1 apart. It never changes the network address or host name and never drops the connection.")
                    .arg(page.durationText(privacy.autoRotateMinimumSeconds))
            value: privacy.autoRotateActive ? qsTr("every %1").arg(page.durationText(privacy.autoRotateIntervalSeconds)) : qsTr("off")
            NxButton {
                anchors.verticalCenter: parent.verticalCenter
                text: privacy.autoRotateActive ? qsTr("Stop") : qsTr("Every 5 min")
                busy: privacy.anonAction === "start" || privacy.anonAction === "stop"
                enabled: privacy.anonToolInstalled && !privacy.anonBusy
                onClicked: privacy.autoRotateActive ? privacy.stopAutoRotate() : privacy.startAutoRotate(300)
            }
        }

        // Log wipe
        NxRow {
            icon: "trash-2"
            title: qsTr("Wipe logs")
            subtitle: qsTr("Empties the systemd journal and every user's shell history, and deletes thumbnail caches and files in /tmp older than an hour.")
            NxButton {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Wipe…")
                busy: privacy.anonAction === "wipe-logs"
                enabled: privacy.anonToolInstalled && !privacy.anonBusy
                onClicked: wipeLogsDialog.open()
            }
        }

        // Leak test
        NxRow {
            icon: "search"
            title: qsTr("Identity leak test")
            subtitle: qsTr("Asks api.ipify.org for the address it sees, directly and through Tor (the direct request fails while the kill switch is on), and shows the DNS resolver in use and whether IPv6 reaches the internet. Results appear below.")
            NxButton {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Run test")
                busy: privacy.anonAction === "leak-test"
                enabled: privacy.anonToolInstalled && !privacy.anonBusy
                onClicked: privacy.runLeakTest()
            }
        }

        // Output log
        Text {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spaceLg
            Layout.rightMargin: Theme.spaceLg
            visible: privacy.anonOutput !== ""
            text: privacy.anonOutput
            font.family: Theme.fontMono
            font.pixelSize: Theme.fontSizeSmall - 1
            color: Theme.textMuted
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            topPadding: Theme.spaceSm
        }

        // NUKE — the big red button
        NxRow {
            icon: "zap"
            title: qsTr("NUKE identity")
            subtitle: qsTr("Ghost Mode with a new network address, new Tor circuits and the logs wiped, in one step.")
            NxButton {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("NUKE…")
                busy: privacy.anonAction === "nuke"
                enabled: privacy.anonToolInstalled && !privacy.anonBusy
                onClicked: nukeDialog.open()
            }
        }
    }

    // ---- OS Integrity & Kernel Lockdown -------------------------------------------------
    NxSection {
        title: qsTr("OS Integrity & Kernel Lockdown")
        description: qsTr("AIDE file integrity monitoring, AppArmor MAC enforcement, kernel audit daemon, and boot-level lockdown with 24 hardening parameters.")

        // Hardening level
        NxRow {
            icon: "shield"
            iconColor: privacy.hardeningLevel === "paranoid" ? Theme.danger
                     : privacy.hardeningLevel === "standard" ? Theme.textMuted : Theme.success
            title: qsTr("Hardening level")
            subtitle: privacy.hardeningLevel === "paranoid"
                ? qsTr("Paranoid: maximum lockdown. Web-browser and Flatpak sandboxes will NOT work.")
                : privacy.hardeningLevel === "standard"
                  ? qsTr("Standard: core protections; nothing that commonly breaks apps.")
                  : qsTr("Hardened (default): strong limits, browser and Flatpak sandboxes still work.")
            ColumnLayout {
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.spaceXs
                NxSegmented {
                    model: [qsTr("Standard"), qsTr("Hardened"), qsTr("Paranoid")]
                    enabled: !privacy.hardenBusy
                    currentIndex: privacy.hardeningLevel === "paranoid" ? 2
                                : privacy.hardeningLevel === "standard" ? 0 : 1
                    onActivated: (index) => privacy.setHardeningLevel(
                        index === 2 ? "paranoid" : index === 0 ? "standard" : "hardened")
                }
            }
        }

        // AIDE file integrity monitor
        NxRow {
            icon: privacy.aideChanges > 0 ? "alert-triangle" : privacy.aideInitialized ? "shield-check" : "shield-off"
            iconColor: privacy.aideChanges > 0 ? Theme.danger
                     : privacy.aideInitialized ? Theme.success : Theme.textMuted
            title: qsTr("File Integrity Monitor (AIDE)")
            subtitle: privacy.aideInitialized
                ? (privacy.aideChanges > 0
                   ? qsTr("⚠ %1 change(s) detected since last check — run a check for details.").arg(privacy.aideChanges)
                   : privacy.aideLastCheck !== ""
                     ? qsTr("Clean · Last checked: %1").arg(privacy.aideLastCheck)
                     : qsTr("Database ready. Run a check to verify system integrity."))
                : qsTr("Not initialized. Build a baseline database to detect future tampering (~5 min).")
            NxButton {
                anchors.verticalCenter: parent.verticalCenter
                text: privacy.aideInitialized ? qsTr("Run Check") : qsTr("Init Database")
                busy: privacy.hardenBusy
                onClicked: privacy.aideInitialized ? privacy.aideCheck() : privacy.aideInit()
            }
        }

        // AppArmor MAC enforcement
        NxRow {
            icon: "shield"
            title: qsTr("AppArmor MAC")
            subtitle: qsTr("Mandatory Access Control. Confines every process to its declared capabilities — an exploit that lands cannot escape its profile.")
            NxBadge {
                anchors.verticalCenter: parent.verticalCenter
                text: privacy.appArmorEnforcing ? qsTr("Enforcing") : qsTr("Permissive / Off")
                kind: privacy.appArmorEnforcing ? "success" : "warning"
            }
            NxButton {
                anchors.verticalCenter: parent.verticalCenter
                visible: !privacy.appArmorEnforcing
                text: qsTr("Enforce All")
                busy: privacy.hardenBusy
                onClicked: privacy.enforceAppArmor()
            }
        }

        // Audit daemon
        NxRow {
            icon: "activity"
            title: qsTr("Audit Daemon")
            subtitle: qsTr("Logs all setuid executions, /etc/shadow access, kernel module loads and privilege escalations. Output in /var/log/audit/.")
            NxBadge {
                anchors.verticalCenter: parent.verticalCenter
                text: privacy.auditdRunning ? qsTr("Running") : qsTr("Stopped")
                kind: privacy.auditdRunning ? "success" : "neutral"
            }
        }

        // Boot-level kernel lockdown
        NxRow {
            icon: "lock"
            title: qsTr("Boot-Level Kernel Lockdown")
            subtitle: privacy.bootHardened
                ? qsTr("Active — 24 params: lockdown=confidentiality, IOMMU, slab_nomerge, PTI, Spectre/MDS/TSX mitigations, vsyscall=none, oops=panic.")
                : qsTr("Write 24 kernel boot params: lockdown mode, IOMMU, memory zeroing, slab protection, all CPU side-channel mitigations. Takes effect on next reboot.")
            NxButton {
                anchors.verticalCenter: parent.verticalCenter
                visible: !privacy.bootHardened
                text: qsTr("Apply…")
                busy: privacy.hardenBusy
                onClicked: bootHardenDialog.open()
            }
            NxBadge {
                anchors.verticalCenter: parent.verticalCenter
                visible: privacy.bootHardened
                text: qsTr("Active")
                kind: "success"
            }
        }

        // Filesystem hardening
        NxRow {
            icon: "hard-drive"
            title: qsTr("Filesystem Hardening")
            subtitle: qsTr("Mounts /proc with hidepid=2,gid=proc: other users' processes are hidden, except from the group “proc” it creates and adds polkitd to, so polkit and pkexec keep working. /tmp and /dev/shm get noexec, nosuid and nodev. Kept after a restart; start once with shadow.hidepid=off to skip the /proc part.")
            NxButton {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Apply")
                busy: privacy.hardenBusy
                onClicked: privacy.hardenFilesystems()
            }
        }

        // Hardening output log
        Text {
            Layout.fillWidth: true
            visible: privacy.hardenOutput !== ""
            text: privacy.hardenOutput
            font.family: "monospace"
            font.pixelSize: Theme.fontSizeSmall - 1
            color: Theme.textMuted
            wrapMode: Text.Wrap
            topPadding: Theme.spaceSm
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
        description: (!privacy.networkManagerInstalled ? qsTr("NetworkManager is not installed; these are the values its configuration files contain.")
                   : privacy.netPrivacyEscapeFile !== "" ? qsTr("Real addresses for this boot: the computer was started with shadow.netprivacy=off (the recovery escape hatch), so every device keeps its own hardware address (%1). The SHADOW privacy defaults apply again at the next normal start.").arg(privacy.netPrivacyEscapeFile)
                   : privacy.shadowPrivacyFile !== "" ? qsTr("System-wide defaults of NetworkManager; the SHADOW privacy defaults are installed (%1). A network can override them in its own settings.").arg(privacy.shadowPrivacyFile)
                   : qsTr("System-wide defaults of NetworkManager. The SHADOW privacy defaults (80-shadow-privacy.conf) are not installed. A network can override the defaults in its own settings."))
                   + " " + qsTr("Network Center → Privacy changes the address policy and encrypted DNS.")

        Repeater {
            model: privacy.macSettings
            NxRow {
                required property var modelData
                icon: modelData.key === "ipv6.ip6-privacy" ? "globe" : modelData.key === "connection.stable-id" ? "refresh"
                    : modelData.key.indexOf("ethernet") === 0 ? "ethernet" : "wifi"
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
        message: qsTr("This permanently deletes the systemd journal, every user's shell history (bash, zsh, fish), thumbnail caches and files in /tmp not changed for an hour. It cannot be undone.")
        confirmText: qsTr("Wipe logs")
        destructive: true
        onConfirmed: privacy.wipeLogsNow()
    }
    NxConfirmDialog {
        id: nukeDialog
        title: qsTr("NUKE identity?")
        message: qsTr("This runs Ghost Mode — the kill switch first (only Tor can reach the network; if it cannot be engaged, nothing else is changed), then a new network address, a new host name, UTC time zone, kernel hardening, Bluetooth off, the transparent Tor proxy and DNS-over-Tor — then new Tor circuits, and wipes the systemd journal, shell histories, thumbnail caches and old /tmp files. The wipe cannot be undone. Ghost Mode lasts until you turn it off or restart.")
        confirmText: qsTr("NUKE")
        destructive: true
        onConfirmed: privacy.nukeIdentity()
    }
    NxConfirmDialog {
        id: ghostDialog
        title: page.ghostKind === "maximum" ? qsTr("Set the threat level to Maximum?") : qsTr("Turn on Ghost Mode?")
        message: qsTr("The kill switch is engaged first: only Tor can reach the network, and connections that do not go through Tor are closed. If it cannot be engaged, nothing else is changed.\n\nThen: %1IPv6 privacy, a new host name, the time zone set to UTC, kernel hardening, Bluetooth off, the transparent Tor proxy and DNS-over-Tor. The systemd journal, shell histories, thumbnail caches and old /tmp files are wiped; that cannot be undone.\n\nIt lasts until you turn it off or restart (DNS-over-Tor stays on after a restart until you turn it off).")
            .arg(page.ghostKind === "maximum" ? "" : qsTr("a new network address on every connected network (a short reconnection), "))
        confirmText: page.ghostKind === "maximum" ? qsTr("Set Maximum") : qsTr("Turn on")
        destructive: true
        onConfirmed: page.ghostKind === "maximum" ? privacy.setThreatModel("maximum") : privacy.enableGhostMode()
    }
    NxConfirmDialog {
        id: hideMeKillSwitchDialog
        title: qsTr("Turn on the kill switch (Tor only)?")
        message: qsTr("Only Tor can reach the network, IPv6 and forwarded traffic included. Connections that do not go through Tor are closed now, and programs that do not use Tor (or the transparent proxy) get no connection.\n\nIt lasts until you turn it off here or restart; then the mode in force now (%1) is put back.")
            .arg(page.ksModeInForce)
        confirmText: qsTr("Turn on")
        onConfirmed: privacy.enableKillSwitch(true)
    }
    NxConfirmDialog {
        id: bootHardenDialog
        title: qsTr("Apply boot-level kernel hardening?")
        message: qsTr("Writes 24 parameters to /etc/default/grub.d/90-shadow-hardening.cfg and runs update-grub. Requires a reboot to activate. Includes lockdown=confidentiality (may prevent loading unsigned kernel modules), oops=panic (crashes on kernel errors), IOMMU force-on, and all CPU side-channel mitigations (Spectre, Meltdown, MDS, TSX). Cannot be reversed without editing the GRUB config manually.")
        confirmText: qsTr("Apply (reboot to activate)")
        destructive: false
        onConfirmed: privacy.applyBootHardening()
    }
}
