// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui
import org.nexus.files

// "Connect to server": builds a KIO URL from protocol, host, port, folder and
// user name. A protocol whose KIO worker is not installed is listed, marked,
// and cannot be connected to — with the package that would provide it.
Dialog {
    id: dialog

    // function(url)
    property var handler: null

    readonly property var protocol: Launcher.protocols[protocolBox.currentIndex] ?? null
    readonly property bool available: protocol !== null && protocol.available
    readonly property url address: protocol === null ? ""
        : Launcher.serverUrl(protocol.scheme, hostField.text, portBox.port, pathField.text, userField.text)
    readonly property bool valid: address.toString() !== ""

    function show(handler) {
        dialog.handler = handler
        // Start on a protocol that can be used on this system.
        if (!dialog.available) {
            const list = Launcher.protocols
            for (let i = 0; i < list.length; i++) {
                if (list[i].available) { protocolBox.currentIndex = i; break }
            }
        }
        if (dialog.protocol) portBox.value = dialog.protocol.defaultPort
        open()
    }

    function connectNow() {
        if (!dialog.valid || !dialog.available) return
        const target = dialog.address
        if (remember.checked) Places.addServer(target, "")
        const fn = dialog.handler
        close()
        if (fn) fn(target)
    }

    title: qsTr("Connect to server")
    width: Math.min(500, parent ? parent.width - Theme.spaceXxl : 500)
    closePolicy: Popup.CloseOnEscape
    footer: null
    onOpened: hostField.forceActiveFocus()

    contentItem: ColumnLayout {
        spacing: Theme.spaceMd

        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: Theme.spaceMd
            rowSpacing: Theme.spaceSm

            Label { text: qsTr("Protocol"); color: Theme.textMuted }
            ComboBox {
                id: protocolBox
                Layout.fillWidth: true
                model: Launcher.protocols.map(p => p.available ? p.label : qsTr("%1 — not installed").arg(p.label))
                Accessible.name: qsTr("Protocol")
                onActivated: if (dialog.protocol) portBox.value = dialog.protocol.defaultPort
            }

            Label { text: qsTr("Server"); color: Theme.textMuted }
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceSm
                TextField {
                    id: hostField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Host name or IP address")
                    selectByMouse: true
                    Accessible.name: qsTr("Server")
                    onAccepted: dialog.connectNow()
                }
                Label { text: qsTr("Port"); color: Theme.textMuted }
                SpinBox {
                    id: portBox
                    from: 1
                    to: 65535
                    value: 22
                    editable: true
                    // Port numbers are not grouped ("8080", not "8,080").
                    textFromValue: (value) => String(value)
                    valueFromText: (text) => {
                        const n = parseInt(text)
                        return isNaN(n) ? portBox.value : n
                    }
                    Accessible.name: qsTr("Port")
                    // The port as typed: `value` only follows the text when
                    // the field loses focus or Enter is pressed.
                    property int port: value
                    onValueChanged: port = value
                    Connections {
                        target: portBox.contentItem
                        function onTextEdited() {
                            const n = parseInt(portBox.contentItem.text)
                            if (!isNaN(n) && n >= portBox.from && n <= portBox.to) portBox.port = n
                        }
                    }
                    Keys.onReturnPressed: dialog.connectNow()
                    Keys.onEnterPressed: dialog.connectNow()
                }
            }

            Label { text: qsTr("Folder"); color: Theme.textMuted }
            TextField {
                id: pathField
                Layout.fillWidth: true
                placeholderText: dialog.protocol && dialog.protocol.scheme === "smb" ? qsTr("/share/folder") : qsTr("/ (optional)")
                selectByMouse: true
                Accessible.name: qsTr("Folder")
                onAccepted: dialog.connectNow()
            }

            Label { text: qsTr("User name"); color: Theme.textMuted }
            TextField {
                id: userField
                Layout.fillWidth: true
                placeholderText: qsTr("Optional — the password is asked for when connecting")
                selectByMouse: true
                Accessible.name: qsTr("User name")
                onAccepted: dialog.connectNow()
            }
        }

        NxBanner {
            visible: dialog.protocol !== null && !dialog.available
            kind: "danger"
            title: dialog.protocol ? qsTr("%1 is not available on this system").arg(dialog.protocol.label) : ""
            message: dialog.protocol ? qsTr("The KIO worker for %1:// addresses is part of the “%2” package, which is not installed.")
                .arg(dialog.protocol.scheme).arg(dialog.protocol.package) : ""
        }

        NxKeyValue {
            visible: dialog.available
            label: qsTr("Address")
            labelWidth: Theme.spaceXxxl * 2
            value: dialog.valid ? dialog.address.toString() : qsTr("Enter the server's host name")
            mono: dialog.valid
        }

        CheckBox {
            id: remember
            visible: dialog.available
            text: qsTr("Keep this server in the sidebar")
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Theme.spaceXs
            spacing: Theme.spaceSm
            Item { Layout.fillWidth: true }
            NxButton {
                text: qsTr("Cancel")
                onClicked: dialog.close()
            }
            NxButton {
                text: qsTr("Connect")
                kind: "primary"
                iconName: "globe"
                enabled: dialog.valid && dialog.available
                onClicked: dialog.connectNow()
            }
        }
    }
}
