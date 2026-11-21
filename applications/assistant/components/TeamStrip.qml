// SPDX-License-Identifier: GPL-3.0-or-later
pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls
import org.shadow.assistant

// The model team at work: one dot per model (the lead first, ringed) and a
// line saying what is happening. Shown while a request is being worked on.
Rectangle {
    id: strip
    required property StageModel stage
    readonly property var team: stage.team
    height: 30
    width: Math.min(row.implicitWidth + 28, maxWidth)
    property real maxWidth: 600
    radius: 15
    color: Pe.card
    border.width: 1
    border.color: Pe.alpha(Pe.violet, 0.45)
    Accessible.role: Accessible.Indicator
    Accessible.name: stage.teamText

    function toneOf(state: string): color {
        return state === "done" ? Pe.ok : state === "working" ? Pe.her
             : state === "failed" ? Pe.alarm : Pe.inkFaint
    }
    function wordOf(m: var): string {
        const secs = m.seconds > 0 ? " · " + m.seconds.toFixed(1) + " s" : ""
        if (m.role === "lead")
            return m.state === "working" ? qsTr("lead · answering") : m.state === "done" ? qsTr("lead · answered")
                 : m.state === "failed" ? qsTr("lead · failed") : qsTr("lead · waiting for the others")
        return m.state === "done" ? qsTr("ready") + secs : m.state === "working" ? qsTr("working…")
             : m.state === "failed" ? qsTr("no answer") : m.state === "late" ? qsTr("too slow, left out") : qsTr("waiting")
    }
    readonly property string detail: {
        const lines = []
        for (let i = 0; i < team.length; ++i)
            lines.push(team[i].name + "  —  " + wordOf(team[i]))
        return lines.join("\n")
    }

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 10
        Row {
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4
            Repeater {
                model: strip.team
                delegate: Rectangle {
                    id: dot
                    required property var modelData
                    readonly property bool lead: modelData.role === "lead"
                    readonly property bool busy: modelData.state === "working"
                    anchors.verticalCenter: parent ? parent.verticalCenter : undefined
                    width: lead ? 10 : 7
                    height: width
                    radius: width / 2
                    color: lead && modelData.state === "waiting" ? "transparent" : strip.toneOf(modelData.state)
                    border.width: lead ? 1.5 : 0
                    border.color: Pe.her
                    SequentialAnimation on opacity {
                        running: dot.busy && strip.visible && strip.stage.animate && !strip.stage.frozen
                        loops: Animation.Infinite
                        NumberAnimation { to: 0.35; duration: 520; easing.type: Easing.InOutSine }
                        NumberAnimation { to: 1.0; duration: 520; easing.type: Easing.InOutSine }
                        onRunningChanged: if (!running) dot.opacity = 1
                    }
                }
            }
        }
        Text {
            textFormat: Text.PlainText
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(implicitWidth, strip.maxWidth - 28 - 10 - strip.team.length * 11)
            elide: Text.ElideRight
            text: strip.stage.teamText
            font.family: Pe.sans
            font.pixelSize: Pe.small
            font.weight: Font.DemiBold
            color: Pe.ink
        }
    }
    HoverHandler { id: hover }
    ToolTip.visible: hover.hovered && strip.detail !== ""
    ToolTip.delay: 300
    ToolTip.text: strip.detail
}
