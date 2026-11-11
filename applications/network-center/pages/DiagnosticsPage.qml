// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.networkcenter

NxPage {
    id: page
    title: qsTr("Diagnostics")
    subtitle: qsTr("Find out where a connection problem lies, or run a single test against one host.")
    maxContentWidth: 1040

    // ---- tools -------------------------------------------------------------------
    readonly property var toolKeys: ["ping", "trace", "dns", "port", "mtu"]
    property int toolIndex: 0
    readonly property string toolKey: toolKeys[toolIndex]
    property int familyIndex: 0
    readonly property string family: ["auto", "IPv4", "IPv6"][familyIndex]
    readonly property string target: targetField.text.trim()
    readonly property string targetError: target === "" ? ""
        : toolKey === "dns" ? NetUtil.dnsNameError(target) : NetUtil.hostError(target)
    readonly property string serverError: toolKey === "dns" && serverField.text.trim() !== "" ? NetUtil.hostError(serverField.text.trim()) : ""
    readonly property int portNumber: parseInt(portField.text)
    readonly property bool portValid: /^[0-9]{1,5}$/.test(portField.text) && portNumber >= 1 && portNumber <= 65535
    // Why the selected tool cannot run at all on this system, or "".
    readonly property string missingTool: {
        const tools = Diagnostics.tools
        if ((toolKey === "ping" || toolKey === "mtu") && !tools.ping)
            return qsTr("ping is not installed (package iputils-ping).")
        if (toolKey === "trace" && tools.trace === "")
            return qsTr("Neither traceroute (package traceroute) nor tracepath (package iputils-tracepath) is installed.")
        return ""
    }
    readonly property string toolHint: {
        const tools = Diagnostics.tools
        switch (toolKey) {
        case "ping": return qsTr("Sends ICMP echo requests and reports the replies and round-trip times.")
        case "trace": return tools.trace === "tracepath" ? qsTr("Lists the routers on the way to the host (tracepath).")
                                                         : qsTr("Lists the routers on the way to the host (traceroute, UDP probes, at most 30 hops).")
        case "dns": return tools.dig ? qsTr("Queries DNS records with dig. Leave the server empty to ask the system's resolvers; an IP address is looked up in reverse.")
            : tools.resolvectl ? qsTr("dig is not installed: the lookup goes through systemd-resolved (resolvectl).")
            : qsTr("dig is not installed (package bind9-dnsutils): only A and AAAA lookups through the system resolver are possible.")
        case "port": return qsTr("Tries one TCP connection from this computer to the port and reports whether it is open, refused or filtered.")
        case "mtu": return qsTr("Finds the largest packet that reaches the host without fragmentation, using echo requests with the don't-fragment flag.")
        }
        return ""
    }
    readonly property bool ownOutput: Diagnostics.tool === toolKey
    readonly property bool canRun: !Diagnostics.running && !Diagnostics.troubleshooting && missingTool === ""
        && target !== "" && targetError === "" && serverError === "" && (toolKey !== "port" || portValid)

    function runTool() {
        if (!canRun)
            return
        switch (toolKey) {
        case "ping": Diagnostics.ping(target, family, countBox.value); break
        case "trace": Diagnostics.traceRoute(target, family); break
        case "dns": Diagnostics.dnsLookup(target, typeBox.currentText, serverField.text.trim()); break
        case "port": Diagnostics.portCheck(target, portNumber, family, timeoutBox.value); break
        case "mtu": Diagnostics.mtuProbe(target, family === "auto" ? "IPv4" : family); break
        }
    }

    // ---- troubleshooting ----------------------------------------------------------------
    readonly property string testHost: testHostField.text.trim()
    readonly property string testHostError: testHost === "" ? "" : NetUtil.hostError(testHost)

    function stepIcon(state) {
        return state === "ok" ? "check-circle" : state === "warn" ? "alert-triangle" : state === "fail" ? "x-circle"
            : state === "running" ? "clock" : state === "skipped" ? "minus" : "circle"
    }
    function stepColor(state) {
        return state === "ok" ? Theme.success : state === "warn" ? Theme.warning : state === "fail" ? Theme.danger
            : state === "running" ? Theme.accent : Theme.textMuted
    }

    NxSection {
        title: qsTr("Troubleshoot")
        description: qsTr("Checks the layers a connection depends on, in order, and stops at the first one that fails: link, IP address, default gateway, name resolution, HTTPS.")
        headerActions: [
            NxButton {
                visible: Diagnostics.troubleshooting
                text: qsTr("Stop")
                kind: "danger"
                iconName: "stop"
                onClicked: Diagnostics.stop()
            },
            NxButton {
                visible: !Diagnostics.troubleshooting
                text: qsTr("Run checks")
                kind: "primary"
                iconName: "play"
                enabled: !Diagnostics.running && page.testHostError === ""
                onClicked: Diagnostics.troubleshoot(page.testHost)
            }
        ]

        FormRow {
            label: qsTr("Test host")
            hint: qsTr("Resolved and contacted on port 443 in the last two steps")
            error: page.testHostError
            TextField {
                id: testHostField
                Layout.fillWidth: true
                text: Diagnostics.defaultTestHost
                enabled: !Diagnostics.troubleshooting
                font.family: Theme.fontMono
                placeholderText: Diagnostics.defaultTestHost
                Accessible.name: qsTr("Test host")
                onAccepted: if (!Diagnostics.troubleshooting && !Diagnostics.running && page.testHostError === "") Diagnostics.troubleshoot(page.testHost)
            }
        }
        Repeater {
            model: Diagnostics.steps
            NxRow {
                id: stepRow
                required property var modelData
                icon: page.stepIcon(modelData.state)
                iconColor: page.stepColor(modelData.state)
                title: modelData.title
                subtitle: modelData.detail
                BusyIndicator {
                    visible: stepRow.modelData.state === "running"
                    running: visible
                    implicitWidth: 28
                    implicitHeight: 28
                }
                NxBadge {
                    visible: stepRow.modelData.state !== "running" && stepRow.modelData.state !== "pending"
                    kind: stepRow.modelData.state === "ok" ? "success" : stepRow.modelData.state === "warn" ? "warning"
                        : stepRow.modelData.state === "fail" ? "danger" : "neutral"
                    text: stepRow.modelData.state === "ok" ? qsTr("Passed") : stepRow.modelData.state === "warn" ? qsTr("Remark")
                        : stepRow.modelData.state === "fail" ? qsTr("Failed") : qsTr("Skipped")
                }
            }
        }
    }

    NxBanner {
        visible: Diagnostics.verdict !== ""
        kind: Diagnostics.verdictKind === "" ? "info" : Diagnostics.verdictKind
        title: Diagnostics.verdict
        message: Diagnostics.verdictDetail
    }

    NxCard {
        Layout.fillWidth: true
        implicitHeight: toolColumn.implicitHeight + padding * 2

        ColumnLayout {
            id: toolColumn
            anchors.left: parent.left
            anchors.right: parent.right
            spacing: Theme.spaceMd

            TabBar {
                id: tabs
                Layout.fillWidth: true
                enabled: !Diagnostics.running
                currentIndex: page.toolIndex
                onCurrentIndexChanged: page.toolIndex = currentIndex
                TabButton { text: qsTr("Ping"); width: implicitWidth }
                TabButton { text: qsTr("Trace route"); width: implicitWidth }
                TabButton { text: qsTr("DNS lookup"); width: implicitWidth }
                TabButton { text: qsTr("Port check"); width: implicitWidth }
                TabButton { text: qsTr("MTU probe"); width: implicitWidth }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceSm

                TextField {
                    id: targetField
                    Layout.fillWidth: true
                    enabled: !Diagnostics.running
                    font.family: Theme.fontMono
                    placeholderText: page.toolKey === "dns" ? qsTr("Name or IP address to look up") : qsTr("Host name or IP address")
                    Accessible.name: placeholderText
                    onAccepted: page.runTool()
                }

                // ---- per-tool options ----
                TextField {
                    id: portField
                    visible: page.toolKey === "port"
                    Layout.preferredWidth: 90
                    enabled: !Diagnostics.running
                    text: "443"
                    font.family: Theme.fontMono
                    placeholderText: qsTr("Port")
                    inputMethodHints: Qt.ImhDigitsOnly
                    Accessible.name: qsTr("TCP port")
                    onAccepted: page.runTool()
                }
                ComboBox {
                    id: typeBox
                    visible: page.toolKey === "dns"
                    Layout.preferredWidth: 110
                    enabled: !Diagnostics.running
                    model: ["A", "AAAA", "CNAME", "MX", "NS", "TXT", "SOA", "SRV", "CAA", "PTR"]
                    Accessible.name: qsTr("Record type")
                }
                TextField {
                    id: serverField
                    visible: page.toolKey === "dns"
                    Layout.preferredWidth: 200
                    enabled: !Diagnostics.running && Diagnostics.tools.dig === true
                    font.family: Theme.fontMono
                    placeholderText: qsTr("Server (optional)")
                    Accessible.name: qsTr("DNS server to ask")
                    onAccepted: page.runTool()
                }
                NxSegmented {
                    visible: page.toolKey !== "dns"
                    enabled: !Diagnostics.running
                    model: page.toolKey === "mtu" ? ["IPv4", "IPv6"] : [qsTr("Auto"), "IPv4", "IPv6"]
                    currentIndex: page.toolKey === "mtu" ? Math.max(0, page.familyIndex - 1) : page.familyIndex
                    onActivated: (index) => page.familyIndex = page.toolKey === "mtu" ? index + 1 : index
                }
                SpinBox {
                    id: countBox
                    visible: page.toolKey === "ping"
                    enabled: !Diagnostics.running
                    from: 1
                    to: 100
                    value: 4
                    editable: true
                    Accessible.name: qsTr("Number of echo requests")
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Number of echo requests")
                }
                SpinBox {
                    id: timeoutBox
                    visible: page.toolKey === "port"
                    enabled: !Diagnostics.running
                    from: 1
                    to: 30
                    value: 5
                    editable: true
                    Accessible.name: qsTr("Timeout in seconds")
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Timeout in seconds")
                }

                NxButton {
                    visible: !Diagnostics.running
                    text: qsTr("Run")
                    kind: "primary"
                    iconName: "play"
                    enabled: page.canRun
                    onClicked: page.runTool()
                }
                NxButton {
                    visible: Diagnostics.running
                    text: qsTr("Stop")
                    kind: "danger"
                    iconName: "stop"
                    onClicked: Diagnostics.stop()
                }
            }

            Label {
                Layout.fillWidth: true
                text: page.missingTool !== "" ? page.missingTool
                    : page.targetError !== "" ? page.targetError
                    : page.serverError !== "" ? qsTr("Server: %1").arg(page.serverError)
                    : page.toolKey === "port" && !page.portValid ? qsTr("The port must be a number from 1 to 65535.")
                    : page.toolHint
                color: page.missingTool !== "" || page.targetError !== "" || page.serverError !== "" || (page.toolKey === "port" && !page.portValid)
                    ? Theme.danger : Theme.textMuted
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WordWrap
            }

            // Result and output belong to the tool that produced them.
            NxBanner {
                visible: page.ownOutput && Diagnostics.result !== ""
                kind: Diagnostics.resultKind === "" ? "info" : Diagnostics.resultKind
                message: Diagnostics.result
            }

            OutputConsole {
                Layout.fillWidth: true
                Layout.preferredHeight: 280
                text: page.ownOutput ? Diagnostics.output : ""
                placeholder: qsTr("The output of the tool appears here while it runs.")
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceSm
                BusyIndicator {
                    visible: Diagnostics.running
                    running: visible
                    implicitWidth: 28
                    implicitHeight: 28
                }
                Label {
                    Layout.fillWidth: true
                    text: Diagnostics.running ? qsTr("Running…") : ""
                    color: Theme.textMuted
                }
                NxButton {
                    text: qsTr("Copy output")
                    iconName: "copy"
                    enabled: page.ownOutput && Diagnostics.output !== ""
                    onClicked: {
                        NetUtil.copyToClipboard(Diagnostics.output)
                        ApplicationWindow.window.toaster.show(qsTr("Output copied to the clipboard"), "success")
                    }
                }
                NxButton {
                    text: qsTr("Clear")
                    enabled: !Diagnostics.running && page.ownOutput && Diagnostics.output !== ""
                    onClicked: Diagnostics.clearOutput()
                }
            }
        }
    }
}
