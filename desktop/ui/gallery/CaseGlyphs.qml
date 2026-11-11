// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui

// Test page: the level glyphs (Wi-Fi signal, battery charge, volume) at the
// sizes the shell and the applications use, in three colours.
NxWindow {
    id: window
    title: "ShadowUI Gallery — glyphs"
    width: 1000
    height: 560

    readonly property var groups: [
        { title: "Wi-Fi signal: wifi-0 … wifi-4, wifi-off", names: ["wifi-0", "wifi-1", "wifi-2", "wifi-3", "wifi-4", "wifi", "wifi-off"] },
        { title: "Battery: empty, low, half, high, full, charging", names: ["battery-empty", "battery-low", "battery-half", "battery-high", "battery-full", "battery-charging", "battery"] },
        { title: "Volume: zero, low, high, mute, off", names: ["volume-zero", "volume-low", "volume-high", "volume-mute", "volume-off"] },
    ]

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spaceXl
        spacing: Theme.spaceLg
        Repeater {
            model: window.groups
            NxCard {
                id: card
                required property var modelData
                Layout.fillWidth: true
                implicitHeight: 140
                ColumnLayout {
                    anchors.fill: parent
                    spacing: Theme.spaceSm
                    Label { text: card.modelData.title; font.weight: Theme.weightSemibold }
                    RowLayout {
                        spacing: Theme.spaceXl
                        Repeater {
                            model: card.modelData.names
                            ColumnLayout {
                                id: cell
                                required property string modelData
                                spacing: Theme.spaceXs
                                RowLayout {
                                    spacing: Theme.spaceSm
                                    NxIcon { name: cell.modelData; size: 16; color: Theme.textPrimary; Layout.alignment: Qt.AlignBottom }
                                    NxIcon { name: cell.modelData; size: 24; color: Theme.accent; Layout.alignment: Qt.AlignBottom }
                                    NxIcon { name: cell.modelData; size: 40; color: Theme.textSecondary; Layout.alignment: Qt.AlignBottom }
                                }
                                Label { text: cell.modelData; font.family: Theme.fontMono; font.pixelSize: Theme.fontSizeCaption; color: Theme.textMuted }
                            }
                        }
                    }
                }
            }
        }
        Item { Layout.fillHeight: true }
    }
}
