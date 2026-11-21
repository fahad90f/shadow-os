// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import QtQuick.Templates as T
import org.shadow.assistant

// Pill button. kind: primary (ink pill) | accent (brand light) | danger |
// secondary (hairline) | ghost (text only).
T.Button {
    id: b
    property string kind: "secondary"
    property string iconName: ""
    property bool compact: false
    property string hint: ""          // keyboard hint shown after the label, e.g. "Alt+Y"
    property string tip: ""
    // what a screen reader says (default: the label, else the tip)
    property string accessibleName: text !== "" ? text : tip

    readonly property color fg: !enabled ? Pe.inkFaint
        : kind === "primary" ? Pe.bg
        : kind === "accent" ? "#06080C"
        : kind === "danger" ? (Pe.dark ? "#1A0508" : "#FFFFFF")
        : kind === "ghost" ? (hovered ? Pe.ink : Pe.inkSoft) : Pe.ink

    implicitHeight: compact ? 30 : 36
    implicitWidth: Math.max(implicitHeight, implicitContentWidth + leftPadding + rightPadding)
    leftPadding: text === "" ? 0 : (compact ? 12 : 16)
    rightPadding: text === "" ? 0 : (compact ? 12 : 16)
    font.family: Pe.sans
    font.pixelSize: compact ? Pe.small : Pe.ui
    font.weight: Font.DemiBold
    hoverEnabled: true
    // Tab reaches it; a click does not take the keyboard away from where it
    // was (typing after clicking a button must not press it again).
    focusPolicy: Qt.TabFocus
    Accessible.role: checkable ? Accessible.CheckBox : Accessible.Button
    Accessible.name: accessibleName
    Accessible.description: tip
    ToolTip.visible: tip !== "" && hovered
    ToolTip.text: tip
    ToolTip.delay: 500

    background: Rectangle {
        radius: height / 2
        opacity: b.enabled ? 1 : (b.kind === "ghost" || b.kind === "secondary" ? 1 : 0.4)
        color: b.kind === "primary" ? (b.down ? Pe.inkSoft : b.hovered ? Pe.mix(Pe.ink, Pe.cyan, 0.12) : Pe.ink)
             : b.kind === "danger" ? (b.down ? Qt.darker(Pe.alarm, 1.15) : b.hovered ? Qt.lighter(Pe.alarm, 1.06) : Pe.alarm)
             : b.kind === "accent" ? "transparent"
             : b.down ? Pe.alpha(Pe.ink, 0.12) : b.hovered ? Pe.alpha(Pe.ink, 0.07) : "transparent"
        border.width: b.kind === "secondary" ? 1 : 0
        border.color: b.hovered ? Pe.inkMute : Pe.lineStrong
        Rectangle {
            anchors.fill: parent
            radius: parent.radius
            visible: b.kind === "accent"
            opacity: b.down ? 0.85 : 1
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0.0; color: b.hovered ? "#4BE3F8" : "#22D8F5" }
                GradientStop { position: 1.0; color: b.hovered ? "#B59BFF" : "#A585FF" }
            }
        }
        Rectangle {
            anchors.fill: parent
            anchors.margins: -3
            radius: height / 2
            color: "transparent"
            border.width: 2
            border.color: Pe.cyan
            visible: b.visualFocus
        }
    }
    contentItem: Item {
        implicitWidth: row.implicitWidth
        implicitHeight: row.implicitHeight
        Row {
            id: row
            anchors.centerIn: parent
            spacing: 7
            PeIcon {
                anchors.verticalCenter: parent.verticalCenter
                name: b.iconName
                size: b.compact ? 14 : 16
                color: b.fg
            }
            Text {
                textFormat: Text.PlainText
                anchors.verticalCenter: parent.verticalCenter
                visible: b.text !== ""
                text: b.text
                font: b.font
                color: b.fg
            }
            Text {
                textFormat: Text.PlainText
                anchors.verticalCenter: parent.verticalCenter
                visible: b.hint !== ""
                text: b.hint
                font.family: Pe.mono
                font.pixelSize: 10
                // quieter than the label, and still at least 4.5:1 on every fill
                color: Pe.alpha(b.fg, b.kind === "secondary" ? 0.7 : 0.9)
            }
        }
    }
}
