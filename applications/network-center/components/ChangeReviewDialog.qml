// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.nexus.ui

// Shows exactly what an edit of a connection profile will change before it
// is applied: every property with its old and new value, and warnings.
// 'plan' is the result of Nm.planIpChange().
Dialog {
    id: root

    property var plan: ({})
    readonly property var changes: plan.changes || []
    readonly property var warnings: plan.warnings || []
    readonly property bool active: plan.active === true

    signal apply(bool reactivate)

    title: qsTr("Apply changes to “%1”?").arg(plan.name || "")
    width: Math.min(640, parent ? parent.width - Theme.spaceXxl : 640)
    closePolicy: Popup.CloseOnEscape
    footer: null
    onOpened: reactivate.checked = true

    contentItem: ColumnLayout {
        spacing: Theme.spaceMd

        Repeater {
            model: root.changes
            ColumnLayout {
                id: change
                required property var modelData
                Layout.fillWidth: true
                spacing: 2
                Label {
                    Layout.fillWidth: true
                    text: change.modelData.label
                    font.weight: Theme.weightSemibold
                    elide: Text.ElideRight
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spaceSm
                    Label {
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        text: change.modelData.from
                        color: Theme.textMuted
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fontSizeSmall
                        wrapMode: Text.WrapAnywhere
                    }
                    NxIcon { name: "arrow-right"; size: Theme.iconSmall; color: Theme.accent }
                    Label {
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        text: change.modelData.to
                        font.family: Theme.fontMono
                        font.pixelSize: Theme.fontSizeSmall
                        wrapMode: Text.WrapAnywhere
                    }
                }
            }
        }

        Repeater {
            model: root.warnings
            NxBanner {
                required property string modelData
                kind: "warning"
                message: modelData
            }
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("The profile is updated through NetworkManager, which may ask you to authenticate.")
            color: Theme.textMuted
            font.pixelSize: Theme.fontSizeSmall
            wrapMode: Text.WordWrap
        }

        CheckBox {
            id: reactivate
            visible: root.active
            text: qsTr("Re-activate the connection now so the change takes effect")
        }
        Label {
            Layout.fillWidth: true
            visible: root.active && reactivate.checked
            text: qsTr("The connection on %1 is interrupted for a moment. A wrong address or gateway can cut this computer off the network.").arg(root.plan.device || "")
            color: Theme.warning
            font.pixelSize: Theme.fontSizeSmall
            wrapMode: Text.WordWrap
        }
        Label {
            Layout.fillWidth: true
            visible: !root.active
            text: qsTr("The connection is not active; the new settings are used the next time it is activated.")
            color: Theme.textMuted
            font.pixelSize: Theme.fontSizeSmall
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Theme.spaceXs
            spacing: Theme.spaceSm
            Item { Layout.fillWidth: true }
            NxButton {
                text: qsTr("Cancel")
                onClicked: root.reject()
            }
            NxButton {
                text: root.active && reactivate.checked ? qsTr("Save and re-activate") : qsTr("Save")
                kind: "primary"
                enabled: root.changes.length > 0
                onClicked: {
                    const again = root.active && reactivate.checked
                    root.accept()
                    root.apply(again)
                }
            }
        }
    }
}
