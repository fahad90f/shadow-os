// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Layouts
import QtQuick.Shapes
import org.shadow.assistant

// A tool call inside Shadow's line, written like a stage direction:
// "Ran a command  du -sh … · done   Output". The exact preview is always
// shown; the result opens in a slab. A pending call only points at the
// question card (the persona asks it there).
Item {
    id: act
    required property StageModel stage
    property var call: ({})
    readonly property string status: call.status !== undefined ? call.status : ""
    readonly property bool pending: status === "pending"
    readonly property string preview: call.preview || ""
    readonly property string result: call.result || ""
    property bool expanded: status === "error"

    implicitHeight: col.implicitHeight

    Accessible.role: Accessible.StaticText
    Accessible.name: stage.pastTense(call.summary || "", status) + ": " + preview + ", " + statusText.text

    Column {
        id: col
        width: parent.width
        spacing: 8

        // One line: glyph, what she did, the exact preview (elided in the
        // middle when space is short, never wrapped away from its summary),
        // the status, and the output toggle.
        Item {
            width: parent.width
            height: 26

            // status glyph, on the line's centre
            Item {
                id: glyph
                width: 18; height: 18
                y: 4
                Rectangle {
                    anchors.fill: parent
                    radius: 9
                    color: "transparent"
                    border.width: 1.5
                    border.color: act.status === "done" ? Pe.ok
                                : act.status === "error" ? Pe.alarm
                                : act.status === "denied" ? Pe.inkMute
                                : act.pending ? Pe.her : Pe.cyan
                    opacity: act.status === "running" ? 0.35 : 1
                }
                PeIcon {
                    anchors.centerIn: parent
                    size: 12
                    name: act.status === "done" ? "check" : act.status === "error" ? "close"
                        : act.status === "denied" ? "minus" : act.pending ? "help" : ""
                    color: act.status === "done" ? Pe.ok : act.status === "error" ? Pe.alarm
                         : act.pending ? Pe.her : Pe.inkMute
                }
                // running: a short arc that turns (render-thread animator)
                Shape {
                    anchors.fill: parent
                    visible: act.status === "running"
                    preferredRendererType: Shape.CurveRenderer
                    ShapePath {
                        strokeColor: Pe.cyan
                        strokeWidth: 1.8
                        fillColor: "transparent"
                        capStyle: ShapePath.RoundCap
                        PathAngleArc { centerX: 9; centerY: 9; radiusX: 8.2; radiusY: 8.2; startAngle: -90; sweepAngle: 110 }
                    }
                    RotationAnimator on rotation {
                        from: 0; to: 360; duration: 1100
                        loops: Animation.Infinite
                        running: act.status === "running" && act.stage.animate && !act.stage.frozen
                    }
                }
            }

            RowLayout {
                id: summaryRow
                anchors.left: glyph.right
                anchors.leftMargin: 10
                anchors.right: outBtn.visible ? outBtn.left : parent.right
                anchors.rightMargin: 8
                height: parent.height
                spacing: 8
                Text {
                    textFormat: Text.PlainText
                    Layout.alignment: Qt.AlignVCenter
                    text: act.stage.pastTense(act.call.summary || "", act.status)
                    font.family: Pe.sans
                    font.pixelSize: Pe.ui
                    font.weight: Font.Medium
                    color: Pe.inkSoft
                }
                Rectangle {
                    visible: act.preview !== ""
                    Layout.alignment: Qt.AlignVCenter
                    Layout.fillWidth: true
                    // (+2: layouts round to whole pixels, a short preview must not elide)
                    Layout.preferredWidth: Math.ceil(previewText.implicitWidth) + 18
                    Layout.maximumWidth: Math.ceil(previewText.implicitWidth) + 18
                    Layout.minimumWidth: Math.min(Math.ceil(previewText.implicitWidth) + 18, 56)
                    implicitHeight: 24
                    radius: 6
                    color: Pe.dark ? "#0C121C" : "#ECEEF4"
                    border.width: 1
                    border.color: Pe.line
                    Text {
                        id: previewText
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        verticalAlignment: Text.AlignVCenter
                        text: act.stage.oneLine(act.preview)
                        elide: Text.ElideMiddle
                        maximumLineCount: 1
                        font.family: Pe.mono
                        font.pixelSize: Pe.small
                        color: Pe.ink
                        textFormat: Text.PlainText
                    }
                }
                Text {
                    id: statusText
                    textFormat: Text.PlainText
                    Layout.alignment: Qt.AlignVCenter
                    text: act.status === "done" ? qsTr("done")
                        : act.status === "error" ? qsTr("failed")
                        : act.status === "denied" ? qsTr("you said no")
                        : act.status === "running" ? qsTr("running…")
                        : act.pending ? qsTr("waiting for your answer") : act.status
                    font.family: Pe.sans
                    font.pixelSize: Pe.small
                    font.weight: act.pending ? Font.DemiBold : Font.Normal
                    color: act.status === "done" ? Pe.ok : act.status === "error" ? Pe.alarm
                         : act.pending ? Pe.her : Pe.inkMute
                }
                Item { Layout.fillWidth: true }
            }
            PeButton {
                id: outBtn
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                visible: !act.pending && act.result !== ""
                compact: true
                kind: "ghost"
                text: act.expanded ? qsTr("Hide output") : qsTr("Output")
                iconName: act.expanded ? "chevron-up" : "chevron-down"
                // a toggle: its state is announced (shown / hidden)
                checkable: true
                checked: act.expanded
                accessibleName: qsTr("Output of: %1").arg(act.stage.oneLine(act.preview !== "" ? act.preview : (act.call.summary || "")))
                onClicked: act.expanded = !act.expanded
            }
        }
        CodeSlab {
            visible: act.expanded && act.result !== ""
            stage: act.stage
            x: 28
            width: parent.width - 28
            code: act.result
            lang: qsTr("output")
            plainLabel: true
            maxBodyHeight: 220
        }
    }
}
