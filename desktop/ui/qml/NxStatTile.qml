// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts

// Headline figure with a label and a status line (dashboard top row).
//   tone: "neutral" | "accent" | "success" | "warning" | "danger"
NxCard {
    id: root

    property string label: ""
    property string value: ""
    property string detail: ""
    property string icon: ""
    property string tone: "neutral"
    property bool loading: false

    readonly property color toneColor: tone === "success" ? Theme.success
        : tone === "warning" ? Theme.warning
        : tone === "danger" ? Theme.danger
        : tone === "accent" ? Theme.accent : Theme.textSecondary

    implicitWidth: 220
    implicitHeight: 112
    Accessible.name: label + ": " + value + (detail ? ", " + detail : "")

    ColumnLayout {
        anchors.fill: parent
        spacing: Theme.spaceXs

        RowLayout {
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                text: root.label
                color: Theme.textSecondary
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                font.weight: Theme.weightMedium
                elide: Text.ElideRight
            }
            NxIcon {
                visible: root.icon !== ""
                name: root.icon
                size: Theme.iconSmall
                color: root.toneColor
            }
        }
        Text {
            Layout.fillWidth: true
            text: root.loading ? "…" : root.value
            color: root.tone === "warning" || root.tone === "danger" ? root.toneColor : Theme.textPrimary
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeDisplay - 2
            font.weight: Theme.weightSemibold
            elide: Text.ElideRight
        }
        Item { Layout.fillHeight: true }
        Text {
            Layout.fillWidth: true
            visible: root.detail !== ""
            text: root.detail
            color: root.toneColor
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
            elide: Text.ElideRight
        }
    }
}
