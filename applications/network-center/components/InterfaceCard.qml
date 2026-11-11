// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.networkcenter

// One network interface: header with state and live rates, expandable details.
// Used as a delegate over a RecordModel of NetState.interfaces.
NxCard {
    id: card

    required property string name
    required property string typeLabel
    required property string category
    required property string status
    required property string operstate
    required property var flags
    required property string mac
    required property int mtu
    required property int speedMbps
    required property string duplex
    required property string driver
    required property string bus
    required property string busDevice
    required property string kind
    required property string master
    required property string peer
    required property var addresses
    required property bool wireless
    required property bool physical

    property bool expanded: status === "up" && category !== "loopback"
    readonly property var live: NetState.liveStats[name] || ({})
    readonly property var device: Nm.devices.find(d => d.name === card.name) || ({})
    readonly property bool managed: Nm.running && device.name !== undefined && device.managed === true && category !== "loopback"
    readonly property int nmState: device.stateCode === undefined ? 0 : device.stateCode
    readonly property bool working: Nm.busyTargets.indexOf(name) >= 0

    Layout.fillWidth: true
    implicitHeight: body.implicitHeight + padding * 2

    ColumnLayout {
        id: body
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: Theme.spaceMd

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceMd

            NxIcon {
                name: card.category === "wifi" ? "wifi" : card.category === "loopback" ? "refresh"
                    : card.category === "tunnel" ? "shield" : card.category === "ethernet" ? "ethernet" : "layers"
                size: Theme.iconLarge
                color: card.status === "up" ? Theme.accent : Theme.textMuted
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                RowLayout {
                    spacing: Theme.spaceSm
                    Label {
                        text: card.name
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fontSizeBodyLarge
                        font.weight: Theme.weightSemibold
                    }
                    NxBadge {
                        kind: card.status === "up" ? "success" : card.status === "down" ? "neutral" : "warning"
                        text: card.status === "up" ? qsTr("Up") : card.status === "down" ? qsTr("Disabled")
                            : card.status === "dormant" ? qsTr("Dormant") : qsTr("No carrier")
                    }
                    NxBadge {
                        visible: NetState.primaryInterface === card.name
                        kind: "accent"
                        dot: false
                        text: qsTr("Default route")
                    }
                }
                Label {
                    Layout.fillWidth: true
                    text: card.addresses.length > 0 ? card.typeLabel + "  ·  " + card.addresses.map(a => a.cidr).join("  ")
                                                    : card.typeLabel + "  ·  " + qsTr("no address")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSizeSmall
                    elide: Text.ElideRight
                }
            }

            ColumnLayout {
                spacing: 2
                Label {
                    Layout.alignment: Qt.AlignRight
                    text: "↓ " + Theme.formatRate(card.live.rxRate || 0)
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.accent
                }
                Label {
                    Layout.alignment: Qt.AlignRight
                    text: "↑ " + Theme.formatRate(card.live.txRate || 0)
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fontSizeSmall
                    color: Theme.accentViolet
                }
            }

            NxButton {
                visible: card.managed && (card.nmState === 30 || card.nmState === 120)
                text: qsTr("Connect")
                busy: card.working
                onClicked: Nm.connectDevice(card.name)
            }
            NxButton {
                visible: card.managed && card.nmState >= 40 && card.nmState <= 100
                text: qsTr("Disconnect")
                busy: card.working
                onClicked: Nm.disconnectDevice(card.name)
            }
            NxIconButton {
                iconName: card.expanded ? "chevron-up" : "chevron-down"
                tooltip: card.expanded ? qsTr("Hide details of %1").arg(card.name) : qsTr("Show details of %1").arg(card.name)
                onClicked: card.expanded = !card.expanded
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            visible: card.expanded
            spacing: Theme.spaceMd

            NxDivider { Layout.fillWidth: true }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceXl
                Repeater {
                    model: [
                        { label: qsTr("Received"), history: "rxHistory", violet: false },
                        { label: qsTr("Sent"), history: "txHistory", violet: true },
                    ]
                    ColumnLayout {
                        id: lane
                        required property var modelData
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        spacing: Theme.spaceXs
                        Label {
                            text: qsTr("%1, last %2 s").arg(lane.modelData.label).arg(NetState.historyCapacity)
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSizeCaption
                        }
                        NxSparkline {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 44
                            values: card.live[lane.modelData.history] || []
                            capacity: NetState.historyCapacity
                            minScale: 1024
                            gridLines: 1
                            color: lane.modelData.violet ? Theme.accentViolet : Theme.accent
                            Accessible.name: lane.modelData.label
                        }
                    }
                }
            }

            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: Theme.spaceXl
                rowSpacing: Theme.spaceXs

                NxKeyValue {
                    Layout.preferredWidth: 1
                    labelWidth: 130
                    label: qsTr("Hardware address")
                    value: card.mac
                    mono: true
                }
                NxKeyValue {
                    Layout.preferredWidth: 1
                    labelWidth: 130
                    label: qsTr("Received")
                    value: card.live.available ? qsTr("%1 in %2 packets").arg(Theme.formatBytes(card.live.rxBytes)).arg(card.live.rxPackets) : ""
                }
                NxKeyValue {
                    Layout.preferredWidth: 1
                    labelWidth: 130
                    label: qsTr("MTU")
                    value: qsTr("%1 bytes").arg(card.mtu)
                }
                NxKeyValue {
                    Layout.preferredWidth: 1
                    labelWidth: 130
                    label: qsTr("Sent")
                    value: card.live.available ? qsTr("%1 in %2 packets").arg(Theme.formatBytes(card.live.txBytes)).arg(card.live.txPackets) : ""
                }
                NxKeyValue {
                    Layout.preferredWidth: 1
                    labelWidth: 130
                    label: qsTr("Link speed")
                    value: card.speedMbps > 0
                        ? qsTr("%1 Mbit/s").arg(card.speedMbps) + (card.duplex !== "" ? qsTr(", %1 duplex").arg(card.duplex) : "")
                        : card.device.speed ? card.device.speed
                        : card.category === "loopback" ? qsTr("Not applicable") : qsTr("Not reported by the driver")
                }
                NxKeyValue {
                    Layout.preferredWidth: 1
                    labelWidth: 130
                    label: qsTr("Errors / dropped")
                    value: card.live.available
                        ? qsTr("receive %1 / %2, send %3 / %4").arg(card.live.rxErrors).arg(card.live.rxDropped).arg(card.live.txErrors).arg(card.live.txDropped)
                        : ""
                }
                NxKeyValue {
                    Layout.preferredWidth: 1
                    labelWidth: 130
                    label: qsTr("Driver")
                    value: card.driver !== "" ? card.driver + (card.bus !== "" ? qsTr(" (%1 %2)").arg(card.bus).arg(card.busDevice) : "")
                                              : qsTr("None (kernel built-in)")
                    mono: card.driver !== ""
                }
                NxKeyValue {
                    Layout.preferredWidth: 1
                    labelWidth: 130
                    label: qsTr("Kernel state")
                    value: card.operstate + "  " + card.flags.join(" ")
                    mono: true
                }
                NxKeyValue {
                    Layout.preferredWidth: 1
                    labelWidth: 130
                    visible: card.master !== "" || card.peer !== ""
                    label: card.master !== "" ? qsTr("Member of") : qsTr("Linked to")
                    value: card.master !== "" ? card.master : card.peer
                    mono: true
                }
                NxKeyValue {
                    Layout.preferredWidth: 1
                    labelWidth: 130
                    visible: Nm.running
                    label: qsTr("NetworkManager")
                    value: card.device.name === undefined ? qsTr("Not known to NetworkManager")
                        : card.device.connection !== "" ? qsTr("%1 — “%2”").arg(card.device.stateText).arg(card.device.connection)
                        : card.device.stateText
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: card.addresses.length > 0
                spacing: Theme.spaceXs
                Label {
                    text: qsTr("Addresses")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSizeCaption
                    font.weight: Theme.weightSemibold
                    font.capitalization: Font.AllUppercase
                }
                Repeater {
                    model: card.addresses
                    RowLayout {
                        id: addressRow
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: Theme.spaceSm
                        NxBadge { text: addressRow.modelData.family; kind: "neutral"; dot: false }
                        TextEdit {
                            Layout.fillWidth: true
                            text: addressRow.modelData.cidr
                            readOnly: true
                            selectByMouse: true
                            color: Theme.textPrimary
                            selectionColor: Theme.accentFillStrong
                            selectedTextColor: Theme.textPrimary
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.fontSizeBody
                            Accessible.name: qsTr("Address %1").arg(addressRow.modelData.cidr)
                        }
                        Label {
                            text: [qsTr("scope %1").arg(addressRow.modelData.scope),
                                   addressRow.modelData.dynamic ? qsTr("assigned automatically") : qsTr("static"),
                                   addressRow.modelData.flags.filter(f => f !== "dynamic" && f !== "permanent").join(" ")]
                                .filter(s => s !== "").join(" · ")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSizeSmall
                        }
                    }
                }
            }
        }
    }
}
