// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui

// One tile of "Models & Skills in Action": an icon in a lit square, a name
// and what it does. A role of the model panel or one of the built-in skills.
Item {
    id: root

    required property var analyst   // a "role" or a "skill" entry of trading.analysts
    property Labels labels

    readonly property bool role: analyst.kind === "role"
    readonly property string name: role ? (analyst.lead !== "" ? analyst.lead : labels.role(analyst.role))
                                        : labels.skillName(analyst.id)
    readonly property string caption: role ? labels.role(analyst.role) : labels.skillRole(analyst.id)
    readonly property color tone: labels.stateColor(analyst.state)

    implicitWidth: 52
    implicitHeight: column.implicitHeight
    Accessible.role: Accessible.StaticText
    Accessible.name: qsTr("%1, %2: %3").arg(name).arg(caption).arg(labels.stateText(analyst.state))

    HoverHandler { id: hover }
    ToolTip.visible: hover.hovered
    ToolTip.delay: 300
    ToolTip.text: role ? qsTr("%1 · %2\n%3").arg(caption).arg(labels.stateText(analyst.state)).arg(analyst.detail)
                       : labels.skillTitle(analyst.id) + " · " + labels.stateText(analyst.state)
                         + (analyst.detail ? "\n" + analyst.detail : "")

    ColumnLayout {
        id: column
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 2

        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: 42
            Layout.preferredHeight: 42
            radius: Theme.radiusMd
            color: Theme.alpha(root.tone, 0.10)
            border.width: Theme.borderWidth
            border.color: Theme.alpha(root.tone, 0.7)
            NxIcon {
                anchors.centerIn: parent
                name: root.role ? root.labels.roleIcon(root.analyst.role) : root.labels.skillIcon(root.analyst.id)
                size: Theme.icon
                color: root.tone
            }
            // How many models stand behind a role.
            Rectangle {
                visible: root.role && root.analyst.models > 1
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: -5
                width: 16
                height: 16
                radius: 8
                color: Theme.accent
                Label {
                    anchors.centerIn: parent
                    text: root.role ? root.analyst.models : ""
                    color: Theme.bgBase
                    font.pixelSize: Theme.fontSizeCaption - 1
                    font.weight: Theme.weightSemibold
                }
            }
            BusyIndicator {
                anchors.fill: parent
                anchors.margins: -3
                running: root.analyst.state === "working"
                visible: running
            }
        }
        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: root.name
            color: Theme.textPrimary
            font.pixelSize: Theme.fontSizeCaption
            font.weight: Theme.weightSemibold
            fontSizeMode: Text.HorizontalFit
            minimumPixelSize: 8
        }
        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: root.caption
            color: Theme.textMuted
            font.pixelSize: Theme.fontSizeCaption - 1
            fontSizeMode: Text.HorizontalFit
            minimumPixelSize: 7
        }
    }
}
