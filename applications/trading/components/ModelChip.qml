// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui

// One role of the panel in the header: its leading model and whether the
// role's models are answering. Click to open the Model Panel.
Rectangle {
    id: root

    required property var analyst   // a "role" entry of trading.analysts
    property Labels labels
    signal clicked()

    readonly property color tone: labels.stateColor(analyst.state)
    readonly property string title: analyst.lead !== "" ? analyst.lead : labels.role(analyst.role)

    implicitWidth: row.implicitWidth + Theme.spaceMd * 2
    implicitHeight: 44
    radius: Theme.radiusMd
    color: hover.hovered ? Theme.bgHover : Theme.alpha(Theme.bgRaised, 0.9)
    border.width: activeFocus ? 2 : Theme.borderWidth
    border.color: activeFocus ? Theme.textPrimary : Theme.alpha(Theme.accent, 0.3)
    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: qsTr("%1, %2 models, %3: %4. Open the Model Panel.").arg(labels.role(analyst.role))
        .arg(analyst.models).arg(title).arg(labels.stateText(analyst.state))
    Keys.onReturnPressed: clicked()
    Keys.onSpacePressed: clicked()

    HoverHandler { id: hover }
    TapHandler { onTapped: root.clicked() }
    ToolTip.visible: hover.hovered
    ToolTip.delay: 400
    ToolTip.text: labels.role(analyst.role) + "\n" + analyst.detail

    RowLayout {
        id: row
        anchors.centerIn: parent
        spacing: Theme.spaceSm
        Rectangle {
            Layout.preferredWidth: 28
            Layout.preferredHeight: 28
            radius: Theme.radiusSm
            color: Theme.alpha(root.tone, 0.14)
            NxIcon {
                anchors.centerIn: parent
                name: root.labels.roleIcon(root.analyst.role)
                size: Theme.iconSmall
                color: root.tone
            }
        }
        ColumnLayout {
            spacing: 0
            RowLayout {
                spacing: Theme.spaceXs
                Label {
                    text: root.title
                    color: Theme.textPrimary
                    font.pixelSize: Theme.fontSizeSmall
                    font.weight: Theme.weightSemibold
                }
                Label {
                    visible: root.analyst.models > 1
                    text: "+" + (root.analyst.models - 1)
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSizeCaption
                }
            }
            Label {
                text: root.labels.stateText(root.analyst.state)
                color: root.tone
                font.pixelSize: Theme.fontSizeCaption
            }
        }
    }
}
