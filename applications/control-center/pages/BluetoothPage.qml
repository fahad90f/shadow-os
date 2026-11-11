// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.controlcenter

NxPage {
    id: page
    title: qsTr("Bluetooth")
    subtitle: qsTr("Pair and connect wireless keyboards, mice, headphones and other devices.")

    // Bumped when BlueZ refuses a change, so switches fall back to the real state.
    property int revision: 0
    property string removeUbi: ""
    property string removeName: ""

    function deviceStatus(connected, busy, battery, kind) {
        const parts = [kind]
        if (busy === "pairing") parts.push(qsTr("Pairing…"))
        else if (busy === "connecting") parts.push(qsTr("Connecting…"))
        else if (busy === "disconnecting") parts.push(qsTr("Disconnecting…"))
        else if (busy === "removing") parts.push(qsTr("Removing…"))
        else if (connected) parts.push(qsTr("Connected"))
        else parts.push(qsTr("Not connected"))
        if (connected && battery >= 0) parts.push(qsTr("Battery %1 %").arg(battery))
        return parts.join("  ·  ")
    }

    BluetoothBackend {
        id: bt
        onFailed: (message) => {
            page.revision++
            page.ApplicationWindow.window.toaster.show(message, "danger")
        }
        onPromptChanged: {
            if (bt.prompt.kind !== undefined) {
                codeField.text = ""
                promptDialog.open()
            } else {
                promptDialog.close()
            }
        }
    }

    component EmptyRow: Item {
        property alias text: emptyLabel.text
        Layout.fillWidth: true
        implicitHeight: Theme.rowHeight
        Label {
            id: emptyLabel
            anchors.fill: parent
            anchors.leftMargin: Theme.spaceLg
            anchors.rightMargin: Theme.spaceLg
            verticalAlignment: Text.AlignVCenter
            color: Theme.textMuted
            elide: Text.ElideRight
        }
    }

    NxCard {
        visible: bt.ready && !bt.available
        Layout.fillWidth: true
        implicitHeight: 320
        NxEmptyState {
            anchors.fill: parent
            icon: "bluetooth"
            tone: "warning"
            title: bt.state === "unsupported-build" ? qsTr("Built without Bluetooth support")
                 : bt.state === "service-not-running" ? qsTr("The Bluetooth service is not running")
                 : bt.state === "no-system-bus" ? qsTr("The Bluetooth service cannot be reached")
                 : bt.state === "rfkill-hard" ? qsTr("Bluetooth is switched off by hardware")
                 : qsTr("No Bluetooth adapter")
            message: bt.statusMessage
            actionText: bt.state === "service-not-running" ? qsTr("Start the service") : ""
            onActionTriggered: bt.startService()
        }
    }

    NxSection {
        visible: bt.available
        title: qsTr("This computer")

        NxRow {
            icon: "bluetooth"
            iconColor: bt.powered ? Theme.accent : Theme.textSecondary
            title: qsTr("Bluetooth")
            subtitle: bt.state === "rfkill-soft" ? bt.statusMessage
                    : bt.adapterName !== "" ? (bt.adapterCount > 1 ? qsTr("%1  ·  %2  ·  one of %3 adapters").arg(bt.adapterName).arg(bt.adapterAddress).arg(bt.adapterCount)
                                                                    : qsTr("%1  ·  %2").arg(bt.adapterName).arg(bt.adapterAddress))
                    : ""
            Switch {
                anchors.verticalCenter: parent.verticalCenter
                checked: (page.revision, bt.powered)
                Accessible.name: qsTr("Bluetooth")
                onToggled: bt.setPowered(checked)
            }
        }
        NxRow {
            icon: "eye"
            title: qsTr("Visible to other devices")
            subtitle: !bt.powered ? qsTr("Switch Bluetooth on to make this computer visible.")
                    : bt.discoverableTimeout > 0 ? qsTr("Other devices can find “%1”. Visibility ends by itself after %2.").arg(bt.adapterName).arg(Theme.formatDuration(bt.discoverableTimeout))
                    : qsTr("Other devices can find “%1” for as long as this is on.").arg(bt.adapterName)
            enabled: bt.powered
            Switch {
                anchors.verticalCenter: parent.verticalCenter
                checked: (page.revision, bt.discoverable)
                Accessible.name: qsTr("Visible to other devices")
                onToggled: bt.setDiscoverable(checked)
            }
        }
    }

    NxSection {
        visible: bt.available && bt.powered
        title: qsTr("My devices")

        Repeater {
            model: bt.pairedDevices
            NxRow {
                id: mine
                required property string ubi
                required property string name
                required property string kind
                required property string deviceIcon
                required property bool paired
                required property bool trusted
                required property bool connected
                required property int battery
                required property string busy

                icon: deviceIcon
                iconColor: connected ? Theme.accent : Theme.textSecondary
                title: name
                subtitle: page.deviceStatus(connected, busy, battery, kind)

                NxButton {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: !mine.paired
                    text: qsTr("Pair")
                    busy: mine.busy === "pairing"
                    onClicked: bt.pair(mine.ubi)
                }
                NxButton {
                    anchors.verticalCenter: parent.verticalCenter
                    text: mine.connected ? qsTr("Disconnect") : qsTr("Connect")
                    busy: mine.busy === "connecting" || mine.busy === "disconnecting"
                    enabled: mine.busy === ""
                    onClicked: mine.connected ? bt.disconnectDevice(mine.ubi) : bt.connectDevice(mine.ubi)
                }
                NxIconButton {
                    id: moreButton
                    anchors.verticalCenter: parent.verticalCenter
                    iconName: "more-horizontal"
                    tooltip: qsTr("More actions for %1").arg(mine.name)
                    onClicked: deviceMenu.open()

                    Menu {
                        id: deviceMenu
                        width: 300
                        x: moreButton.width - width
                        y: moreButton.height
                        MenuItem {
                            text: qsTr("Trusted — may connect on its own")
                            checkable: true
                            checked: mine.trusted
                            onTriggered: bt.setTrusted(mine.ubi, !mine.trusted)
                        }
                        MenuSeparator { }
                        MenuItem {
                            text: qsTr("Remove…")
                            icon.source: "image://nxicon/trash"
                            onTriggered: {
                                page.removeUbi = mine.ubi
                                page.removeName = mine.name
                                removeDialog.open()
                            }
                        }
                    }
                }
            }
        }
        EmptyRow {
            visible: bt.pairedCount === 0
            text: qsTr("No device is paired with this computer yet.")
        }
    }

    NxSection {
        visible: bt.available && bt.powered
        title: qsTr("Nearby devices")
        description: bt.discovering ? qsTr("Searching… Put the device you want to add into pairing mode.")
                                    : qsTr("Search to list devices that are in pairing mode.")
        headerActions: [
            BusyIndicator {
                anchors.verticalCenter: parent.verticalCenter
                width: Theme.controlHeight - 8
                height: Theme.controlHeight - 8
                padding: 0
                visible: bt.discovering
                running: bt.discovering
            },
            NxButton {
                text: bt.discovering ? qsTr("Stop") : qsTr("Search")
                iconName: bt.discovering ? "stop" : "search"
                onClicked: bt.discovering ? bt.stopScan() : bt.startScan()
            }
        ]

        Repeater {
            model: bt.availableDevices
            NxRow {
                id: nearby
                required property string ubi
                required property string name
                required property string address
                required property string kind
                required property string deviceIcon
                required property string busy

                icon: deviceIcon
                title: name
                subtitle: nearby.busy === "pairing" ? kind + "  ·  " + qsTr("Pairing…") : kind + "  ·  " + address

                NxButton {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Pair")
                    kind: "primary"
                    busy: nearby.busy !== ""
                    onClicked: bt.pair(nearby.ubi)
                }
            }
        }
        EmptyRow {
            visible: bt.availableCount === 0
            text: bt.discovering ? qsTr("No new device found so far.") : qsTr("No nearby device is known. Start a search.")
        }
    }

    NxConfirmDialog {
        id: removeDialog
        title: qsTr("Remove %1?").arg(page.removeName)
        message: qsTr("The pairing is deleted. To use the device with this computer again it has to be paired anew.")
        confirmText: qsTr("Remove")
        destructive: true
        onConfirmed: bt.removeDevice(page.removeUbi)
    }

    // The question BlueZ asks while pairing.
    Dialog {
        id: promptDialog
        readonly property string kind: bt.prompt.kind !== undefined ? bt.prompt.kind : ""
        readonly property string deviceName: bt.prompt.device !== undefined && bt.prompt.device !== "" ? bt.prompt.device : qsTr("the device")
        readonly property bool wantsInput: kind === "pin" || kind === "passkey"
        readonly property bool informative: kind === "display-pin" || kind === "display-passkey"

        title: kind === "authorize" || kind === "authorize-service" ? qsTr("Allow %1?").arg(deviceName) : qsTr("Pair with %1").arg(deviceName)
        width: Math.min(440, page.width - Theme.spaceXxl)
        closePolicy: Popup.CloseOnEscape
        footer: null
        // Closing the dialog any other way than through its buttons means "no".
        onClosed: if (bt.prompt.kind !== undefined) bt.answerPrompt(false, "")

        contentItem: ColumnLayout {
            spacing: Theme.spaceLg

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textSecondary
                text: promptDialog.kind === "confirm" ? qsTr("Check that %1 shows the same code, then confirm on both devices.").arg(promptDialog.deviceName)
                    : promptDialog.kind === "pin" ? qsTr("Enter the PIN of %1. Simple devices often use 0000 or 1234; otherwise it is in the manual.").arg(promptDialog.deviceName)
                    : promptDialog.kind === "passkey" ? qsTr("Enter the six-digit code that %1 shows.").arg(promptDialog.deviceName)
                    : promptDialog.kind === "display-pin" ? qsTr("Type this PIN on %1, then press Enter there.").arg(promptDialog.deviceName)
                    : promptDialog.kind === "display-passkey" ? qsTr("Type this code on %1, then press Enter there.").arg(promptDialog.deviceName)
                    : promptDialog.kind === "authorize" ? qsTr("%1 wants to pair with this computer.").arg(promptDialog.deviceName)
                    : promptDialog.kind === "authorize-service" ? qsTr("%1 wants to use a service of this computer (%2).").arg(promptDialog.deviceName).arg(bt.prompt.uuid)
                    : ""
            }
            Label {
                Layout.fillWidth: true
                visible: promptDialog.kind === "confirm" || promptDialog.informative
                horizontalAlignment: Text.AlignHCenter
                text: bt.prompt.code !== undefined ? bt.prompt.code : ""
                font.family: Theme.fontMono
                font.pixelSize: Theme.fontSizeDisplay
                font.weight: Theme.weightSemibold
                font.letterSpacing: 4
                Accessible.name: qsTr("Pairing code %1").arg(text)
            }
            TextField {
                id: codeField
                Layout.fillWidth: true
                visible: promptDialog.wantsInput
                placeholderText: promptDialog.kind === "passkey" ? qsTr("Six digits") : qsTr("PIN")
                font.family: Theme.fontMono
                maximumLength: promptDialog.kind === "passkey" ? 6 : 16
                inputMethodHints: promptDialog.kind === "passkey" ? Qt.ImhDigitsOnly : Qt.ImhNoPredictiveText
                validator: RegularExpressionValidator {
                    regularExpression: promptDialog.kind === "passkey" ? /^[0-9]{1,6}$/ : /^.{1,16}$/
                }
                Accessible.name: placeholderText
                onAccepted: if (acceptableInput) bt.answerPrompt(true, text)
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceSm
                Item { Layout.fillWidth: true }
                NxButton {
                    text: promptDialog.informative || promptDialog.wantsInput ? qsTr("Cancel") : qsTr("Reject")
                    onClicked: bt.answerPrompt(false, "")
                }
                NxButton {
                    visible: !promptDialog.informative
                    text: promptDialog.kind === "confirm" ? qsTr("The codes match")
                        : promptDialog.wantsInput ? qsTr("Pair") : qsTr("Allow")
                    kind: "primary"
                    enabled: !promptDialog.wantsInput || codeField.acceptableInput
                    onClicked: bt.answerPrompt(true, codeField.text)
                }
            }
        }
        onOpened: if (wantsInput) codeField.forceActiveFocus()
    }
}
