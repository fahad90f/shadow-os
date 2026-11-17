// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import org.shadow.ui
import org.shadow.controlcenter

// Wi-Fi Security Audit: capture a WPA/WPA2 handshake and test it against a
// dictionary.  Requires aircrack-ng (airmon-ng / airodump-ng / aireplay-ng /
// aircrack-ng) and a monitor-capable wireless adapter.
//
// This page is for authorised security testing on networks you own or have
// written permission to test.
NxPage {
    id: page
    title: qsTr("Wi-Fi Security Audit")
    subtitle: qsTr("Test whether a WPA / WPA2 network's password is in a common dictionary. Use only on networks you own or have permission to test.")

    // ---- backend -----------------------------------------------------------
    WifiAuditBackend {
        id: audit
        Component.onCompleted: scanNetworks()
    }

    // ---- helpers -----------------------------------------------------------
    property string selectedBssid:   ""
    property string selectedSsid:    ""
    property string selectedChannel: "0"
    property bool   useCustomList:   false
    property string customListPath:  ""
    property bool   advancedOpen:    false

    function networkIndex() {
        for (let i = 0; i < audit.networks.length; i++)
            if (audit.networks[i].bssid === page.selectedBssid) return i
        return -1
    }

    function canStart() {
        if (audit.running) return false
        if (page.selectedBssid === "") return false
        if (page.useCustomList && page.customListPath.trim() === "") return false
        return true
    }

    function doStart() {
        const wl = page.useCustomList ? page.customListPath.trim() : ""
        audit.start(page.selectedBssid, page.selectedSsid, page.selectedChannel, wl)
    }

    // Show a toast on result
    Connections {
        target: audit
        function onChanged() {
            if (audit.phase === "found")
                page.ApplicationWindow.window.toaster.show(
                    qsTr("Password found: %1").arg(audit.result), "success")
            else if (audit.phase === "not_found")
                page.ApplicationWindow.window.toaster.show(
                    qsTr("Password not in wordlist — try a larger list"), "warning")
            else if (audit.phase === "error")
                page.ApplicationWindow.window.toaster.show(audit.errorMessage, "danger")
        }
    }

    // ---- UI ----------------------------------------------------------------

    // Disclaimer card
    NxCard {
        Layout.fillWidth: true
        implicitHeight: disclaimerRow.implicitHeight + Theme.spaceMd * 2
        color: Qt.rgba(Theme.warning.r, Theme.warning.g, Theme.warning.b, 0.08)
        border.color: Qt.rgba(Theme.warning.r, Theme.warning.g, Theme.warning.b, 0.25)
        RowLayout {
            id: disclaimerRow
            anchors { left: parent.left; right: parent.right; top: parent.top
                      margins: Theme.spaceMd }
            spacing: Theme.spaceSm
            NxIcon { name: "shield-alert"; size: 18; color: Theme.warning }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.pixelSize: Theme.fontSizeSmall
                color: Theme.warning
                text: qsTr("Authorised use only — only test networks you own or have written permission to test.  Unauthorised access to computer networks is illegal.")
            }
        }
    }

    // ---- Target network ----------------------------------------------------
    NxSection {
        title: qsTr("Target Network")
        headerActions: [
            NxButton {
                text: qsTr("Scan")
                iconName: "refresh"
                busy: audit.scanBusy
                enabled: !audit.running
                onClicked: audit.scanNetworks()
            }
        ]

        NxRow {
            icon: "wifi"
            title: qsTr("Network")
            subtitle: page.selectedBssid !== ""
                      ? qsTr("BSSID %1 · Channel %2")
                        .arg(page.selectedBssid).arg(page.selectedChannel)
                      : qsTr("Select a network from the list below")

            ComboBox {
                id: networkCombo
                width: 280
                enabled: !audit.running
                model: audit.networks
                textRole: "ssid"
                displayText: currentIndex >= 0
                             ? audit.networks[currentIndex].ssid
                             : (audit.scanBusy ? qsTr("Scanning…") : qsTr("No WPA networks found"))
                Accessible.name: qsTr("Target network")

                delegate: ItemDelegate {
                    required property var modelData
                    required property int index
                    width: networkCombo.width
                    contentItem: ColumnLayout {
                        spacing: 2
                        Label {
                            text: modelData.ssid || qsTr("(hidden)")
                            font.pixelSize: Theme.fontSizeBody
                            color: Theme.textPrimary
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                        Label {
                            text: qsTr("%1 · ch %2 · %3").arg(modelData.bssid)
                                                         .arg(modelData.channel)
                                                         .arg(modelData.security)
                            font.pixelSize: Theme.fontSizeSmall
                            color: Theme.textMuted
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                    }
                    highlighted: networkCombo.highlightedIndex === index
                    onClicked: {
                        networkCombo.currentIndex = index
                        page.selectedBssid   = modelData.bssid
                        page.selectedSsid    = modelData.ssid || ""
                        page.selectedChannel = modelData.channel || "0"
                    }
                }
            }
        }

        // Manual BSSID entry
        NxRow {
            icon: "edit"
            title: qsTr("Manual BSSID")
            subtitle: qsTr("Enter XX:XX:XX:XX:XX:XX directly if the network is not in the list above")
            TextField {
                width: 240
                enabled: !audit.running
                placeholderText: "AA:BB:CC:DD:EE:FF"
                text: page.selectedBssid
                font.family: Theme.monoFamily
                Accessible.name: qsTr("Manual BSSID")
                onTextEdited: {
                    page.selectedBssid = text.toUpperCase()
                    page.selectedSsid = ""
                    networkCombo.currentIndex = -1
                }
            }
            TextField {
                width: 80
                enabled: !audit.running
                placeholderText: qsTr("Chan")
                text: page.selectedChannel
                Accessible.name: qsTr("Channel")
                onTextEdited: page.selectedChannel = text
            }
        }
    }

    // ---- Wordlist ----------------------------------------------------------
    NxSection {
        title: qsTr("Wordlist")

        NxRow {
            icon: "list"
            title: qsTr("Built-in common passwords")
            subtitle: qsTr("~400 of the most commonly used Wi-Fi passwords — a quick first check")
            RadioButton {
                anchors.verticalCenter: parent.verticalCenter
                checked: !page.useCustomList
                enabled: !audit.running
                Accessible.name: qsTr("Built-in wordlist")
                onClicked: page.useCustomList = false
            }
        }

        NxRow {
            icon: "file-text"
            title: qsTr("Custom wordlist file")
            subtitle: qsTr("Use your own text file — one password per line (e.g. rockyou.txt)")
            RadioButton {
                anchors.verticalCenter: parent.verticalCenter
                checked: page.useCustomList
                enabled: !audit.running
                Accessible.name: qsTr("Custom wordlist")
                onClicked: page.useCustomList = true
            }
        }

        Item {
            visible: page.useCustomList
            Layout.fillWidth: true
            implicitHeight: customRow.implicitHeight + Theme.spaceSm
            RowLayout {
                id: customRow
                anchors { left: parent.left; right: parent.right
                          leftMargin: Theme.spaceLg; rightMargin: Theme.spaceLg
                          verticalCenter: parent.verticalCenter }
                spacing: Theme.spaceSm
                TextField {
                    id: customPathField
                    Layout.fillWidth: true
                    enabled: !audit.running
                    placeholderText: qsTr("/path/to/wordlist.txt")
                    text: page.customListPath
                    Accessible.name: qsTr("Wordlist file path")
                    onTextEdited: page.customListPath = text
                }
                NxButton {
                    text: qsTr("Browse…")
                    enabled: !audit.running
                    onClicked: fileDialog.open()
                }
            }
        }
    }

    // ---- Advanced ----------------------------------------------------------
    NxSection {
        NxRow {
            icon: "settings"
            title: qsTr("Advanced options")
            subtitle: page.advancedOpen ? qsTr("Interface auto-detect, timeout 120 s") : ""
            NxIconButton {
                anchors.verticalCenter: parent.verticalCenter
                iconName: page.advancedOpen ? "chevron-up" : "chevron-down"
                tooltip: page.advancedOpen ? qsTr("Collapse") : qsTr("Expand")
                onClicked: page.advancedOpen = !page.advancedOpen
            }
        }

        Item {
            visible: page.advancedOpen
            Layout.fillWidth: true
            implicitHeight: advancedColumn.implicitHeight + Theme.spaceMd
            ColumnLayout {
                id: advancedColumn
                anchors { left: parent.left; right: parent.right
                          leftMargin: Theme.spaceLg; rightMargin: Theme.spaceLg
                          top: parent.top; topMargin: Theme.spaceSm }
                spacing: Theme.spaceXs

                Label {
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSizeSmall
                    text: qsTr("The first monitor-capable adapter is used automatically.\n"
                              + "The adapter and NetworkManager are restored when the audit finishes or is stopped.\n"
                              + "Handshake capture times out after 120 seconds.")
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
                Label {
                    visible: !toolsOkLabel.visible
                    id: toolsMissingLabel
                    color: Theme.danger
                    font.pixelSize: Theme.fontSizeSmall
                    text: qsTr("aircrack-ng is not installed. Install it with:\n"
                              + "sudo apt install aircrack-ng")
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
                Label {
                    id: toolsOkLabel
                    visible: false // set below after phase changes
                    color: Theme.success
                    font.pixelSize: Theme.fontSizeSmall
                    text: qsTr("aircrack-ng tools found ✓")
                    Layout.fillWidth: true
                }
            }
        }
    }

    // ---- Start / Stop button -----------------------------------------------
    Item {
        Layout.fillWidth: true
        implicitHeight: startBtn.implicitHeight + Theme.spaceLg
        RowLayout {
            anchors.centerIn: parent
            spacing: Theme.spaceMd

            NxButton {
                id: startBtn
                text: qsTr("Start Audit")
                kind: "primary"
                iconName: "shield-alert"
                enabled: page.canStart()
                visible: !audit.running
                onClicked: page.doStart()
            }

            NxButton {
                text: qsTr("Stop")
                kind: "danger"
                iconName: "x"
                visible: audit.running
                onClicked: audit.stop()
            }
        }
    }

    // ---- Status / progress -------------------------------------------------
    NxSection {
        visible: audit.phase !== "idle"
        title: qsTr("Status")

        NxRow {
            icon: {
                switch (audit.phase) {
                    case "monitor":   return "wifi"
                    case "scan":      return "search"
                    case "capture":   return "download"
                    case "deauth":    return "zap"
                    case "cracking":  return "key"
                    case "found":     return "unlock"
                    case "not_found": return "lock"
                    case "error":     return "alert-triangle"
                    case "cleanup":   return "refresh-cw"
                    default:          return "info"
                }
            }
            iconColor: {
                switch (audit.phase) {
                    case "found":     return Theme.success
                    case "not_found": return Theme.textMuted
                    case "error":     return Theme.danger
                    default:          return Theme.accent
                }
            }
            title: {
                switch (audit.phase) {
                    case "monitor":   return qsTr("Enabling monitor mode…")
                    case "scan":      return qsTr("Finding target channel…")
                    case "capture":   return qsTr("Capturing WPA handshake…")
                    case "deauth":    return qsTr("Sending deauthentication packets…")
                    case "cracking":  return qsTr("Testing passwords…")
                    case "found":     return qsTr("Password found!")
                    case "not_found": return qsTr("Password not in wordlist")
                    case "error":     return qsTr("Error")
                    case "cleanup":   return qsTr("Restoring adapter…")
                    default:          return audit.phase
                }
            }
            subtitle: {
                if (audit.phase === "found")     return qsTr("The password is: %1").arg(audit.result)
                if (audit.phase === "not_found") return qsTr("Try a larger custom wordlist such as rockyou.txt")
                if (audit.phase === "error")     return audit.errorMessage
                if (audit.phase === "cracking" && audit.total > 0)
                    return qsTr("%1 / %2 tested").arg(audit.progress.toLocaleString())
                                                 .arg(audit.total.toLocaleString())
                return ""
            }

            // Animated orb for active phases
            Item {
                anchors.verticalCenter: parent.verticalCenter
                width: 32; height: 32
                visible: audit.running

                Rectangle {
                    id: outerRing
                    anchors.centerIn: parent
                    width: 28; height: 28; radius: 14
                    color: "transparent"
                    border.width: 2
                    border.color: Theme.accent
                    opacity: 0.5
                    SequentialAnimation on opacity {
                        running: audit.running
                        loops: Animation.Infinite
                        NumberAnimation { to: 1.0; duration: 600; easing.type: Easing.InOutSine }
                        NumberAnimation { to: 0.3; duration: 600; easing.type: Easing.InOutSine }
                    }
                }
                Rectangle {
                    anchors.centerIn: parent
                    width: 10; height: 10; radius: 5
                    color: Theme.accent
                }
            }
        }

        // Progress bar (cracking phase)
        Item {
            visible: audit.phase === "cracking" && audit.total > 0
            Layout.fillWidth: true
            implicitHeight: progressBar.implicitHeight + Theme.spaceSm * 2
            ProgressBar {
                id: progressBar
                anchors {
                    left: parent.left; right: parent.right
                    leftMargin: Theme.spaceLg; rightMargin: Theme.spaceLg
                    verticalCenter: parent.verticalCenter
                }
                from: 0
                to: audit.total
                value: audit.progress
            }
        }

        // Result highlight
        Item {
            visible: audit.phase === "found"
            Layout.fillWidth: true
            implicitHeight: resultBox.implicitHeight + Theme.spaceSm * 2
            Rectangle {
                id: resultBox
                anchors {
                    left: parent.left; right: parent.right
                    leftMargin: Theme.spaceLg; rightMargin: Theme.spaceLg
                    verticalCenter: parent.verticalCenter
                }
                height: resultLabel.implicitHeight + Theme.spaceMd * 2
                radius: Theme.radius
                color: Qt.rgba(Theme.success.r, Theme.success.g, Theme.success.b, 0.12)
                border.color: Qt.rgba(Theme.success.r, Theme.success.g, Theme.success.b, 0.35)

                RowLayout {
                    id: resultLabel
                    anchors {
                        left: parent.left; right: parent.right
                        verticalCenter: parent.verticalCenter
                        leftMargin: Theme.spaceMd; rightMargin: Theme.spaceSm
                    }
                    spacing: Theme.spaceSm
                    NxIcon { name: "key"; size: 16; color: Theme.success }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Password: ")
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSizeBody
                    }
                    Label {
                        text: audit.result
                        color: Theme.success
                        font.pixelSize: Theme.fontSizeBody + 2
                        font.bold: true
                        font.family: Theme.monoFamily
                    }
                    NxIconButton {
                        iconName: "copy"
                        tooltip: qsTr("Copy to clipboard")
                        onClicked: {
                            audit.result.trim()
                            // Copy via clipboard
                            let dummy = Qt.createQmlObject(
                                'import QtQuick; TextEdit { visible: false }',
                                page, "clipboard_helper")
                            dummy.text = audit.result
                            dummy.selectAll()
                            dummy.copy()
                            dummy.destroy()
                            page.ApplicationWindow.window.toaster.show(
                                qsTr("Copied to clipboard"), "info")
                        }
                    }
                }
            }
        }
    }

    // ---- Live log ----------------------------------------------------------
    NxSection {
        visible: audit.log.length > 0
        title: qsTr("Log")

        NxCard {
            Layout.fillWidth: true
            implicitHeight: Math.min(logView.contentHeight + Theme.spaceMd * 2, 220)
            clip: true

            ListView {
                id: logView
                anchors.fill: parent
                anchors.margins: Theme.spaceMd
                model: audit.log
                spacing: 2
                delegate: Label {
                    required property string modelData
                    width: logView.width
                    text: modelData
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSizeSmall
                    font.family: Theme.monoFamily
                    wrapMode: Text.WrapAnywhere
                    elide: Text.ElideNone
                }
                onCountChanged: positionViewAtEnd()
            }
        }
    }

    // ---- File picker dialog ------------------------------------------------
    FileDialog {
        id: fileDialog
        title: qsTr("Choose Wordlist File")
        nameFilters: [qsTr("Text files (*.txt *.lst)"), qsTr("All files (*)")]
        onAccepted: {
            const path = selectedFile.toString().replace("file://", "")
            page.customListPath = path
            customPathField.text = path
        }
    }
}
