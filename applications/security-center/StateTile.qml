// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.nexus.ui

// Overview tile: one protection mechanism, the state the status tool reported
// for it, the evidence behind that state, and a link to its page.
//   tone: "neutral" | "success" | "warning" | "danger"
NxCard {
    id: root

    property string label: ""
    property string icon: "shield"
    property string stateText: ""
    property string tone: "neutral"
    property string evidence: ""
    property string linkText: qsTr("Details")
    property bool loading: false
    // False while a dialog is open above the tile: pointer handlers below a
    // popup still see taps that the popup's own handlers did not consume.
    property bool interactive: true
    signal clicked()

    readonly property color toneColor: tone === "success" ? Theme.success
        : tone === "warning" ? Theme.warning
        : tone === "danger" ? Theme.danger : Theme.textSecondary

    hoverable: true
    accentBorder: activeFocus
    Layout.fillWidth: true
    Layout.fillHeight: true
    Layout.preferredWidth: 240
    implicitHeight: column.implicitHeight + padding * 2
    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: label + ": " + stateText + (evidence !== "" ? ". " + evidence : "")
    Keys.onReturnPressed: clicked()
    Keys.onSpacePressed: clicked()

    HoverHandler { enabled: root.interactive; cursorShape: Qt.PointingHandCursor }
    TapHandler { enabled: root.interactive; onTapped: { root.forceActiveFocus(); root.clicked() } }

    ColumnLayout {
        id: column
        anchors.fill: parent
        spacing: Theme.spaceSm

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceMd
            Rectangle {
                implicitWidth: 40
                implicitHeight: 40
                radius: Theme.radiusSm
                color: Theme.alpha(root.toneColor, 0.14)
                NxIcon {
                    anchors.centerIn: parent
                    name: root.icon
                    size: Theme.icon
                    color: root.toneColor
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                Text {
                    Layout.fillWidth: true
                    text: root.label
                    color: Theme.textSecondary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                    font.weight: Theme.weightMedium
                    elide: Text.ElideRight
                }
                Text {
                    Layout.fillWidth: true
                    text: root.loading ? qsTr("Checking…") : root.stateText
                    color: root.loading ? Theme.textMuted
                        : root.tone === "neutral" ? Theme.textPrimary : root.toneColor
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeTitle
                    font.weight: Theme.weightSemibold
                    elide: Text.ElideRight
                }
            }
        }
        Text {
            Layout.fillWidth: true
            visible: text !== ""
            text: root.loading ? "" : root.evidence
            color: Theme.textMuted
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeSmall
            wrapMode: Text.WordWrap
            maximumLineCount: 5
            elide: Text.ElideRight
            lineHeight: 1.15
        }
        Item { Layout.fillHeight: true }
        RowLayout {
            spacing: Theme.spaceXs
            Text {
                text: root.linkText
                color: Theme.accent
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                font.weight: Theme.weightMedium
            }
            NxIcon { name: "chevron-right"; size: 12; color: Theme.accent }
        }
    }
}
