// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.nexus.ui

// One step of the wizard: heading, explanation and the step's content, which
// fills the remaining height. Steps that can grow put a ScrollColumn inside.
Item {
    id: root

    default property alias content: column.data
    property string title: ""
    property string subtitle: ""
    property alias headerActions: actions.data

    // What the footer needs to know.
    property bool ready: true
    property string blockReason: ""
    // Called by the window each time the step is shown.
    function entered() { }
    // Puts keyboard focus on the heading, so that Tab continues with the
    // step's first control and a screen reader announces the new step.
    function focusHeading() { heading.forceActiveFocus() }

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: Theme.spaceXl
        anchors.rightMargin: Theme.spaceXl
        anchors.topMargin: Theme.spaceLg
        anchors.bottomMargin: Theme.spaceMd
        spacing: Theme.spaceMd

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spaceMd
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.spaceXxs
                Text {
                    id: heading
                    Layout.fillWidth: true
                    activeFocusOnTab: true
                    Rectangle {
                        x: -Theme.spaceMd
                        width: 3
                        height: parent.height
                        radius: 1.5
                        color: Theme.accent
                        visible: heading.activeFocus
                    }
                    text: root.title
                    color: Theme.textPrimary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeHeading
                    font.weight: Theme.weightSemibold
                    elide: Text.ElideRight
                    Accessible.role: Accessible.Heading
                    Accessible.name: text
                }
                Text {
                    Layout.fillWidth: true
                    visible: root.subtitle !== ""
                    text: root.subtitle
                    color: Theme.textSecondary
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBody
                    wrapMode: Text.WordWrap
                }
            }
            Row {
                id: actions
                spacing: Theme.spaceSm
                Layout.alignment: Qt.AlignTop
            }
        }

        ColumnLayout {
            id: column
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.spaceMd
        }
    }
}
