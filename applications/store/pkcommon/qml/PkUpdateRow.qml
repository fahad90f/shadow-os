// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui

// One available update (a row of NexusPk::UpdateChecker.model): selection,
// version transition, classification, download size, and — when expanded —
// the update details PackageKit fetches for it (change log, CVE and bug
// references, restart requirement).
Rectangle {
    id: row

    required property string packageId
    required property string name
    required property string version
    required property string installedVersion
    required property string arch
    required property string repo
    required property string summary
    required property string info
    required property string infoText
    required property real size
    required property string group
    required property var tags
    required property bool selected

    // NexusPk::UpdateChecker
    property QtObject checker: null
    // False while a transaction runs: the selection is then fixed.
    property bool selectable: true
    property bool expanded: false
    property var detail: ({ "state": "none" })

    readonly property string severityKind: info === "security" || info === "critical" || info === "important" ? "danger"
        : info === "blocked" ? "warning" : info === "bugfix" ? "accent" : "neutral"

    function toggleExpanded() {
        expanded = !expanded
        if (expanded && checker) {
            checker.loadDetail(packageId)
            detail = checker.detail(packageId)
        }
    }

    implicitHeight: column.implicitHeight + Theme.spaceMd * 2
    radius: Theme.radiusMd
    color: Theme.bgRaised
    border.width: Theme.borderWidth
    border.color: hover.hovered ? Theme.border : Theme.borderSubtle

    HoverHandler { id: hover }

    Connections {
        target: row.checker
        function onDetailChanged(id) {
            if (id === row.packageId) row.detail = row.checker.detail(id)
        }
    }

    ColumnLayout {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Theme.spaceMd
        spacing: Theme.spaceSm

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceMd

            CheckBox {
                Layout.alignment: Qt.AlignTop
                checked: row.selected
                enabled: row.selectable && row.group !== "blocked"
                Accessible.name: qsTr("Install the update of %1").arg(row.name)
                onToggled: if (row.checker) row.checker.model.setSelected(row.packageId, checked)
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceSm
                    Text {
                        textFormat: Text.PlainText
                        Layout.fillWidth: true
                        text: row.name
                        color: Theme.textPrimary
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeBody
                        font.weight: Theme.weightSemibold
                        elide: Text.ElideRight
                    }
                    Repeater {
                        model: row.tags
                        NxBadge {
                            required property string modelData
                            text: modelData === "kernel" ? qsTr("Kernel") : modelData === "nexus" ? qsTr("NEXUS") : modelData
                            kind: "accent"
                            dot: false
                        }
                    }
                    NxBadge { text: row.infoText; kind: row.severityKind }
                    Text {
                        textFormat: Text.PlainText
                        text: row.size > 0 ? Theme.formatBytes(row.size) : ""
                        visible: row.size > 0
                        color: Theme.textSecondary
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fontSizeSmall
                        horizontalAlignment: Text.AlignRight
                        Layout.minimumWidth: 70
                    }
                }
                Text {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    text: (row.installedVersion !== "" ? row.installedVersion : qsTr("installed version not reported"))
                          + "  →  " + row.version + "   ·   " + row.arch + "   ·   " + (row.repo !== "" ? row.repo : qsTr("origin not reported"))
                    color: Theme.textSecondary
                    font.family: Theme.fontMono
                    font.pixelSize: Theme.fontSizeSmall
                    elide: Text.ElideMiddle
                }
                Text {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    visible: row.summary !== ""
                    text: row.summary
                    color: Theme.textMuted
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                    elide: Text.ElideRight
                }
            }

            NxIconButton {
                Layout.alignment: Qt.AlignTop
                iconName: row.expanded ? "chevron-up" : "chevron-down"
                tooltip: row.expanded ? qsTr("Hide update details") : qsTr("Show change log and update details")
                onClicked: row.toggleExpanded()
            }
        }

        // ---- details (GetUpdateDetail) ---------------------------------------
        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spaceXxl
            visible: row.expanded
            spacing: Theme.spaceSm

            RowLayout {
                visible: row.detail.state === "loading"
                spacing: Theme.spaceSm
                BusyIndicator { running: parent.visible; implicitWidth: 20; implicitHeight: 20; padding: 0 }
                Text {
                    textFormat: Text.PlainText
                    text: qsTr("PackageKit is downloading the update details…")
                    color: Theme.textSecondary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                }
            }
            Text {
                textFormat: Text.PlainText
                Layout.fillWidth: true
                visible: row.detail.state === "error"
                text: qsTr("The update details could not be loaded: %1").arg(row.detail.error || "")
                color: Theme.warning
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.WordWrap
            }
            ColumnLayout {
                Layout.fillWidth: true
                visible: row.detail.state === "ready"
                spacing: Theme.spaceXs
                NxKeyValue { visible: (row.detail.replaces || []).length > 0; label: qsTr("Replaces"); value: (row.detail.replaces || []).join(", "); mono: true; labelWidth: 120 }
                NxKeyValue { visible: (row.detail.issued || "") !== ""; label: qsTr("Issued"); value: row.detail.issued || ""; labelWidth: 120 }
                NxKeyValue { visible: (row.detail.restart || "none") !== "none" && (row.detail.restart || "") !== "unknown"; label: qsTr("After installing"); value: row.detail.restartText || ""; labelWidth: 120 }
                NxKeyValue { visible: (row.detail.cveUrls || []).length > 0; label: qsTr("CVE references"); value: (row.detail.cveUrls || []).join("\n"); mono: true; labelWidth: 120 }
                NxKeyValue { visible: (row.detail.bugzillaUrls || []).length > 0; label: qsTr("Bug references"); value: (row.detail.bugzillaUrls || []).join("\n"); mono: true; labelWidth: 120 }
                NxKeyValue { visible: (row.detail.vendorUrls || []).length > 0; label: qsTr("Vendor links"); value: (row.detail.vendorUrls || []).join("\n"); mono: true; labelWidth: 120 }
                Text {
                    textFormat: Text.PlainText
                    Layout.fillWidth: true
                    visible: (row.detail.updateText || "") === "" && (row.detail.changelog || "") === ""
                    text: qsTr("PackageKit returned no change log for this update.")
                    color: Theme.textMuted
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                }
                Rectangle {
                    Layout.fillWidth: true
                    visible: changes.text !== ""
                    implicitHeight: Math.min(240, changes.implicitHeight + Theme.spaceMd * 2)
                    radius: Theme.radiusSm
                    color: Theme.bgInput
                    border.width: Theme.borderWidth
                    border.color: Theme.border
                    clip: true
                    Flickable {
                        anchors.fill: parent
                        anchors.margins: Theme.spaceMd
                        contentHeight: changes.implicitHeight
                        boundsBehavior: Flickable.StopAtBounds
                        clip: true
                        ScrollBar.vertical: ScrollBar { }
                        TextEdit {
                            id: changes
                            width: parent.width
                            // The back-end sends plain text / light markdown; shown verbatim, never as rich text.
                            text: (row.detail.updateText || "") !== "" ? row.detail.updateText : (row.detail.changelog || "")
                            textFormat: TextEdit.PlainText
                            readOnly: true
                            selectByMouse: true
                            wrapMode: TextEdit.Wrap
                            color: Theme.textSecondary
                            selectionColor: Theme.accentFillStrong
                            selectedTextColor: Theme.textPrimary
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.fontSizeSmall
                            Accessible.name: qsTr("Change log of %1").arg(row.name)
                        }
                    }
                }
            }
        }
    }
}
