// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui

NxWindow {
    id: window
    title: "ShadowUI Gallery"
    width: 1280
    height: 900

    property var history: [0.12, 0.18, 0.16, 0.25, 0.4, 0.35, 0.52, 0.61, 0.44, 0.38, 0.47, 0.72, 0.66, 0.5, 0.58, 0.81, 0.77, 0.6, 0.55, 0.63]

    RowLayout {
        anchors.fill: parent
        spacing: 0

        NxSidebar {
            id: sidebar
            Layout.fillHeight: true
            title: "Gallery"
            currentKey: "components"
            model: [
                { key: "components", title: "Components", icon: "layers", section: "Design system" },
                { key: "colors", title: "Colours", icon: "palette", section: "Design system" },
                { key: "icons", title: "Icons", icon: "grid", section: "Design system", badge: "177" },
                { key: "wifi", title: "Wi-Fi", icon: "wifi", section: "Sample navigation" },
                { key: "security", title: "Security", icon: "shield-check", section: "Sample navigation" },
                { key: "storage", title: "Storage", icon: "harddrive", section: "Sample navigation" },
            ]
            onActivated: (key) => currentKey = key
            header: NxSearchField { Layout.fillWidth: true }
            footer: RowLayout {
                Layout.fillWidth: true
                NxIconButton {
                    iconName: Theme.dark ? "sun" : "moon"
                    tooltip: "Toggle dark / light"
                    onClicked: Appearance.dark = !Appearance.dark
                }
                Repeater {
                    model: Theme.accentNames
                    Rectangle {
                        required property string modelData
                        implicitWidth: 18; implicitHeight: 18; radius: 9
                        color: Theme.accentByName(modelData)
                        border.width: Theme.accentName === modelData ? 2 : 0
                        border.color: Theme.textPrimary
                        TapHandler { onTapped: Appearance.accent = parent.modelData }
                    }
                }
            }
        }

        NxPage {
            Layout.fillWidth: true
            Layout.fillHeight: true
            title: "Components"
            subtitle: "Every control below is rendered by the org.shadow.style Qt Quick Controls style and the org.shadow.ui component library."
            maxContentWidth: 980
            actions: [
                NxButton { text: "Secondary" },
                NxButton { text: "Primary"; kind: "primary"; iconName: "check" }
            ]

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceLg
                NxStatTile { Layout.fillWidth: true; label: "Firewall"; value: "Active"; detail: "nftables · 14 rules"; icon: "firewall"; tone: "success" }
                NxStatTile { Layout.fillWidth: true; label: "Updates"; value: "3"; detail: "2 security updates"; icon: "arrow-up-circle"; tone: "warning" }
                NxStatTile { Layout.fillWidth: true; label: "Open ports"; value: "0"; detail: "No listening services"; icon: "radar"; tone: "accent" }
                NxStatTile { Layout.fillWidth: true; label: "Disk encryption"; value: "Off"; detail: "Root is not encrypted"; icon: "disk-lock"; tone: "danger" }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceLg

                NxCard {
                    Layout.fillWidth: true
                    implicitHeight: 210
                    RowLayout {
                        anchors.fill: parent
                        spacing: Theme.spaceLg
                        Repeater {
                            model: [
                                { value: 0.72, text: "72%", label: "Memory", tone: "accent", available: true },
                                { value: 0.41, text: "41%", label: "CPU", tone: "violet", available: true },
                                { value: 0.93, text: "93%", label: "Disk", tone: "danger", available: true },
                                { value: 0, text: "", label: "GPU", tone: "accent", available: false },
                            ]
                            Item {
                                required property var modelData
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                NxGauge {
                                    anchors.centerIn: parent
                                    width: Math.min(parent.width, parent.height, 150)
                                    height: width
                                    value: parent.modelData.value
                                    text: parent.modelData.text
                                    label: parent.modelData.label
                                    available: parent.modelData.available
                                    color: parent.modelData.tone === "violet" ? Theme.accentViolet
                                        : parent.modelData.tone === "danger" ? Theme.danger : Theme.accent
                                }
                            }
                        }
                    }
                }
                NxCard {
                    Layout.preferredWidth: 340
                    implicitHeight: 210
                    ColumnLayout {
                        anchors.fill: parent
                        spacing: Theme.spaceSm
                        RowLayout {
                            Layout.fillWidth: true
                            Label { text: "CPU history"; font.weight: Theme.weightSemibold; Layout.fillWidth: true }
                            NxSegmented { model: ["1M", "5M", "1H"] }
                        }
                        NxSparkline { Layout.fillWidth: true; Layout.fillHeight: true; values: window.history; maxValue: 1 }
                        NxMeter { Layout.fillWidth: true; label: "/home"; valueText: "412 GiB of 465 GiB"; value: 0.886 }
                    }
                }
            }

            NxSection {
                title: "Buttons and inputs"
                description: "Variants, states and text entry."
                NxRow {
                    title: "Button kinds"
                    NxButton { text: "Ghost"; kind: "ghost" }
                    NxButton { text: "Secondary" }
                    NxButton { text: "Primary"; kind: "primary" }
                    NxButton { text: "Delete"; kind: "danger"; iconName: "trash" }
                    NxButton { text: "Disabled"; enabled: false }
                    NxButton { text: "Working"; kind: "primary"; busy: true }
                }
                NxRow {
                    title: "Text entry"
                    TextField { placeholderText: "Hostname"; text: "shadow"; width: 150 }
                    NxPasswordField { placeholderText: "Passphrase"; text: "correct horse"; width: 170 }
                    ComboBox { model: ["Performance", "Balanced", "Power saver"]; currentIndex: 1; width: 150 }
                    SpinBox { value: 8; from: 0; to: 64 }
                }
                NxRow {
                    title: "Toggles"
                    subtitle: "Switch, check boxes, radio buttons and a slider"
                    Switch { checked: true }
                    Switch { }
                    CheckBox { text: "Checked"; checked: true }
                    CheckBox { text: "Mixed"; tristate: true; checkState: Qt.PartiallyChecked }
                    RadioButton { text: "On"; checked: true }
                    RadioButton { text: "Off" }
                    Slider { value: 0.6; width: 140 }
                }
                NxRow {
                    title: "Progress"
                    ProgressBar { value: 0.64; width: 200; anchors.verticalCenter: parent.verticalCenter }
                    ProgressBar { indeterminate: true; width: 140; anchors.verticalCenter: parent.verticalCenter }
                    BusyIndicator { running: true }
                    NxBadge { text: "Enforcing"; kind: "success" }
                    NxBadge { text: "Complain"; kind: "warning" }
                    NxBadge { text: "Critical"; kind: "danger" }
                    NxBadge { text: "amd64"; kind: "accent"; dot: false }
                    NxBadge { text: "Idle" }
                }
            }

            NxBanner { kind: "warning"; title: "2 security updates are available"; message: "Installing them requires a restart of 1 service."; actionText: "Review"; dismissible: true }
            NxBanner { kind: "danger"; title: "Secure Boot is disabled"; message: "The firmware does not verify the boot chain on this machine." }
            NxBanner { kind: "success"; message: "AppArmor is enabled and 42 profiles are in enforce mode." }

            NxSection {
                title: "Rows"
                NxRow { icon: "language"; title: "Region & Language"; subtitle: "System language and localisation"; chevron: true }
                NxRow { icon: "clock"; title: "Date & Time"; subtitle: "Time zone and clock settings"; value: "Etc/UTC"; chevron: true }
                NxRow { icon: "users"; title: "Users"; subtitle: "Add and remove accounts, change password"; chevron: true }
                NxRow { icon: "info"; title: "Kernel"; value: "6.12.116+deb13-amd64"; monoValue: true }
            }

            NxCard {
                Layout.fillWidth: true
                implicitHeight: 220
                padding: Theme.spaceSm
                NxDataTable {
                    anchors.fill: parent
                    sortRole: "cpu"
                    sortAscending: false
                    currentRow: 1
                    columns: [
                        { title: "Process", role: "name", fill: true },
                        { title: "PID", role: "pid", width: 80, align: Qt.AlignRight, mono: true },
                        { title: "User", role: "user", width: 110 },
                        { title: "CPU", role: "cpu", width: 90, align: Qt.AlignRight, mono: true, format: (v) => v.toFixed(1) + " %" },
                        { title: "Memory", role: "mem", width: 110, align: Qt.AlignRight, mono: true, format: (v) => Theme.formatBytes(v) },
                    ]
                    model: [
                        { name: "kwin_wayland", pid: 1412, user: "shadow", cpu: 6.4, mem: 214748364 },
                        { name: "horizon-shell", pid: 1466, user: "shadow", cpu: 2.1, mem: 163577856 },
                        { name: "NetworkManager", pid: 731, user: "root", cpu: 0.3, mem: 22020096 },
                        { name: "pipewire", pid: 1398, user: "shadow", cpu: 0.2, mem: 15728640 },
                        { name: "systemd-journald", pid: 302, user: "root", cpu: 0.1, mem: 31457280 },
                    ]
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceLg
                NxCard {
                    Layout.fillWidth: true
                    implicitHeight: 230
                    NxEmptyState {
                        anchors.fill: parent
                        icon: "bluetooth"
                        title: "Bluetooth service is not running"
                        message: "bluetooth.service is inactive, so no adapters can be listed."
                        actionText: "Start service"
                        tone: "warning"
                    }
                }
                NxCard {
                    Layout.fillWidth: true
                    implicitHeight: 230
                    ColumnLayout {
                        anchors.fill: parent
                        spacing: Theme.spaceSm
                        TabBar {
                            Layout.fillWidth: true
                            TabButton { text: "Overview"; width: implicitWidth }
                            TabButton { text: "Interfaces"; width: implicitWidth }
                            TabButton { text: "Routes"; width: implicitWidth }
                        }
                        NxKeyValue { label: "Interface"; value: "enp0s31f6"; mono: true }
                        NxKeyValue { label: "IPv4 address"; value: "192.168.1.189/24"; mono: true }
                        NxKeyValue { label: "Gateway"; value: "192.168.1.1"; mono: true }
                        NxKeyValue { label: "DNS"; value: "9.9.9.9, 149.112.112.112"; mono: true }
                        Item { Layout.fillHeight: true }
                    }
                }
            }
        }
    }
}
