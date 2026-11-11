// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui

// A profile on the overview: icon, summary, how many of its tools are
// installed and what installing the rest would cost.
NxCard {
    id: root

    required property var profile
    signal opened(string profileId)

    hoverable: true
    activeFocusOnTab: true
    accentBorder: activeFocus
    Accessible.role: Accessible.Button
    Accessible.name: qsTr("%1 profile, %2 of %3 tools installed").arg(profile.name).arg(profile.installedCount).arg(profile.toolCount)
    Accessible.onPressAction: root.opened(profile.id)
    Keys.onReturnPressed: root.opened(profile.id)
    Keys.onSpacePressed: root.opened(profile.id)
    implicitHeight: column.implicitHeight + padding * 2

    readonly property bool complete: profile.toolCount > 0 && profile.installedCount >= profile.toolCount - profile.unavailableCount
    readonly property string sizeText: !arsenal.stateReady ? qsTr("Reading the package database…")
        : profile.sizeState === "ready" ? (profile.pendingPackages === 0
              ? (profile.installedCount > 0 ? qsTr("Nothing left to download") : qsTr("No tool of this profile is available"))
              : qsTr("%1 · %2 on disk").arg(Theme.formatBytes(profile.downloadBytes)).arg(Theme.formatBytes(profile.installBytes)))
        : profile.sizeState === "error" ? qsTr("Size could not be calculated")
        : qsTr("Calculating size…")

    TapHandler { onTapped: root.opened(root.profile.id) }

    ColumnLayout {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: Theme.spaceMd

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceMd
            ProfileIcon { profile: root.profile; size: 48 }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceXxs
                Text {
                    Layout.fillWidth: true
                    text: root.profile.name
                    color: Theme.textPrimary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeTitle
                    font.weight: Theme.weightSemibold
                    elide: Text.ElideRight
                }
                NxBadge {
                    visible: arsenal.stateReady
                    text: root.profile.metapackageInstalled ? qsTr("Profile installed")
                        : root.profile.installedCount === 0 ? qsTr("Not installed")
                        : root.complete ? qsTr("All tools installed") : qsTr("Partly installed")
                    kind: root.profile.metapackageInstalled || root.complete ? "success"
                        : root.profile.installedCount > 0 ? "accent" : "neutral"
                }
            }
        }

        Text {
            Layout.fillWidth: true
            Layout.preferredHeight: fontMetrics.lineSpacing * 3
            text: root.profile.summary
            color: Theme.textSecondary
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeBody
            wrapMode: Text.WordWrap
            maximumLineCount: 3
            elide: Text.ElideRight
            FontMetrics { id: fontMetrics; font.family: Theme.fontUi; font.pixelSize: Theme.fontSizeBody }
        }

        NxMeter {
            Layout.fillWidth: true
            label: (root.profile.toolCount === 1 ? qsTr("1 tool") : qsTr("%1 tools").arg(root.profile.toolCount))
            valueText: arsenal.stateReady ? qsTr("%1 installed").arg(root.profile.installedCount) : qsTr("—")
            value: root.profile.toolCount > 0 ? root.profile.installedCount / root.profile.toolCount : 0
            toned: false
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceSm
            NxIcon { name: root.profile.sizeState === "ready" && root.profile.pendingPackages > 0 ? "download" : "harddrive"; size: Theme.iconSmall; color: Theme.textMuted }
            Text {
                Layout.fillWidth: true
                text: root.sizeText
                color: Theme.textMuted
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                elide: Text.ElideRight
            }
            NxIcon { name: "chevron-right"; size: Theme.iconSmall; color: Theme.textMuted }
        }
    }
}
