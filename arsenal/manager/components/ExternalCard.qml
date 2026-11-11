// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui

// A well-known tool that the NEXUS repositories do not carry: what it is, why
// there is no package, and where its vendor publishes it. Nothing here
// downloads or installs anything.
NxCard {
    id: root

    required property var entry
    signal message(string text, string kind)

    Layout.fillWidth: true

    readonly property string reasonText: entry.reason === "proprietary" ? qsTr("Proprietary software")
        : entry.reason === "vendor-terms" ? qsTr("Vendor licence terms")
        : entry.reason === "not-in-debian-main" ? qsTr("Not in Debian main")
        : qsTr("Not packaged in Debian")

    function open(url) {
        if (!arsenal.openUrl(url)) {
            arsenal.copyText(url)
            root.message(qsTr("No web browser could be opened. The address was copied instead: %1").arg(url), "warning")
        }
    }

    ColumnLayout {
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: Theme.spaceMd

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceMd
            NxIcon { name: root.entry.categoryIcon; size: Theme.iconLarge; color: Theme.textSecondary }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceXxs
                Text {
                    Layout.fillWidth: true
                    text: root.entry.name
                    color: Theme.textPrimary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBodyLarge
                    font.weight: Theme.weightSemibold
                    elide: Text.ElideRight
                }
                Text {
                    Layout.fillWidth: true
                    text: root.entry.summary
                    color: Theme.textSecondary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                    wrapMode: Text.WordWrap
                }
            }
            NxBadge { Layout.alignment: Qt.AlignTop; text: root.reasonText; kind: "warning"; dot: false }
        }

        NxBanner {
            Layout.fillWidth: true
            kind: "info"
            title: qsTr("Not installable from NEXUS repositories")
            message: root.entry.why
        }

        NxKeyValue { Layout.fillWidth: true; label: qsTr("Licence"); value: root.entry.license; labelWidth: 120 }
        NxKeyValue { Layout.fillWidth: true; label: qsTr("How to get it"); value: root.entry.how; labelWidth: 120 }
        NxKeyValue { Layout.fillWidth: true; label: qsTr("Official source"); value: root.entry.howUrl; mono: true; labelWidth: 120 }
        NxKeyValue {
            Layout.fillWidth: true
            visible: (root.entry.profileNames || []).length > 0
            label: qsTr("Listed in")
            value: (root.entry.profileNames || []).join(", ")
            labelWidth: 120
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceSm
            Item { Layout.fillWidth: true }
            NxButton {
                text: qsTr("Homepage")
                iconName: "globe"
                Accessible.name: qsTr("Open the homepage of %1").arg(root.entry.name)
                onClicked: root.open(root.entry.homepage)
            }
            NxButton {
                text: qsTr("Official instructions")
                iconName: "external-link"
                kind: "primary"
                Accessible.name: qsTr("Open the official instructions for %1").arg(root.entry.name)
                onClicked: root.open(root.entry.howUrl)
            }
        }
    }
}
