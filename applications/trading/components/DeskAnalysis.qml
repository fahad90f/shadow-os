// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import "Format.js" as Format

// What the desk knows and is doing, next to the conversation. Every line is
// a value of the desk or the market engine (desk.status, desk.tasks,
// desk.analysis): nothing here moves or lights up unless something really
// happened. The clock only counts how long ago a real event was.
Flickable {
    id: root

    readonly property var status: desk.status
    readonly property var a: desk.analysis
    readonly property var tasks: desk.tasks
    property real now: Date.now()

    contentWidth: width
    contentHeight: column.implicitHeight
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    ScrollBar.vertical: ScrollBar {}

    // "12 s ago" must change on screen as time passes; nothing else is timed.
    Timer {
        interval: 1000
        repeat: true
        running: root.visible
        onTriggered: root.now = Date.now()
    }

    function sideColor(side) {
        return side === "bullish" ? Theme.success : side === "bearish" ? Theme.danger
             : side === "neutral" ? Theme.warning : Theme.textMuted
    }
    function sideText(side) {
        return side === "bullish" ? qsTr("BULLISH") : side === "bearish" ? qsTr("BEARISH")
             : side === "neutral" ? qsTr("NEUTRAL") : qsTr("NO VIEW YET")
    }
    function sourceText(entry) {
        if (entry.source === "you")
            return entry.by ? qsTr("you · %1").arg(entry.by) : qsTr("you")
        if (entry.source === "market")
            return qsTr("live data")
        if (entry.source === "team")
            return qsTr("analysis team")
        return entry.by ? entry.by : qsTr("model")
    }

    component Row2: RowLayout {
        property alias label: key.text
        property alias value: val.text
        property color tone: Theme.textPrimary
        Layout.fillWidth: true
        spacing: Theme.spaceSm
        Label {
            id: key
            Layout.preferredWidth: 118
            color: Theme.textMuted
            font.pixelSize: Theme.fontSizeSmall
        }
        Label {
            id: val
            Layout.fillWidth: true
            color: parent.tone
            font.pixelSize: Theme.fontSizeSmall
            wrapMode: Text.Wrap
        }
    }

    ColumnLayout {
        id: column
        width: root.width - Theme.spaceSm
        spacing: Theme.spaceSm

        // ---- live analysis: what is running, from real activity -----------------
        Panel {
            Layout.fillWidth: true
            implicitHeight: liveBody.implicitHeight + 56
            title: qsTr("LIVE ANALYSIS")
            icon: "zap"
            glow: root.status.running && root.status.feed !== "error"
            trailing: StatusDot {
                text: !root.status.running ? qsTr("Stopped")
                    : root.status.feed === "error" ? qsTr("Market data error")
                    : root.status.feed === "waiting" ? qsTr("Connecting")
                    : root.status.idle ? qsTr("Live · idle") : qsTr("Live · working")
                tone: !root.status.running || root.status.feed === "error" ? Theme.danger
                    : root.status.feed === "waiting" ? Theme.warning : Theme.success
            }
            ColumnLayout {
                id: liveBody
                width: parent.width
                spacing: 4
                Row2 {
                    label: qsTr("Conversation")
                    value: root.status.read > 0
                        ? qsTr("Monitoring · %1 messages read · last %2").arg(root.status.read)
                              .arg(Format.ago(root.status.lastRead, root.now))
                        : qsTr("Monitoring · no messages yet")
                }
                Row2 {
                    label: qsTr("Model reading")
                    value: root.status.modelReading === "no_key" ? qsTr("Off: no NVIDIA API key (messages are still read on this computer)")
                        : root.status.modelReading === "error" ? qsTr("Error: %1").arg(root.status.readError)
                        : root.status.understood > 0
                            ? qsTr("%1 · %2 understood · last %3").arg(desk.settings.fastName).arg(root.status.understood)
                                  .arg(Format.ago(root.status.lastUnderstood, root.now))
                            : qsTr("%1 · ready").arg(desk.settings.fastName)
                    tone: root.status.modelReading === "on" ? Theme.textPrimary : Theme.warning
                }
                Row2 {
                    label: qsTr("Market feed")
                    value: root.status.feed === "stream" ? qsTr("Live stream · %1").arg(root.status.chartMarket)
                        : root.status.feed === "polling" ? qsTr("Prices every %1 s · %2").arg(root.status.pollSeconds).arg(root.status.chartMarket)
                        : root.status.feed === "error" ? root.status.feedError
                        : qsTr("Connecting…")
                    tone: root.status.feed === "error" ? Theme.danger : Theme.textPrimary
                }
                Row2 {
                    label: qsTr("Last update")
                    value: root.status.lastUpdate > 0
                        ? qsTr("%1 · %2 · %3").arg(Format.ago(root.status.lastUpdate, root.now))
                              .arg(root.status.lastUpdateSource === "you" ? qsTr("from you")
                                   : root.status.lastUpdateSource === "model" ? qsTr("from a model")
                                   : root.status.lastUpdateSource === "team" ? qsTr("from the team")
                                   : qsTr("from the market"))
                              .arg(root.status.lastUpdateText)
                        : qsTr("Nothing yet")
                }
                Label {
                    text: qsTr("Active tasks")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSizeSmall
                    Layout.topMargin: 4
                }
                Repeater {
                    model: root.tasks
                    RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: Theme.spaceSm
                        Rectangle {
                            Layout.preferredWidth: 8
                            Layout.preferredHeight: 8
                            radius: 4
                            color: modelData.kind === "team" ? Theme.warning : Theme.accent
                            SequentialAnimation on opacity {
                                loops: Animation.Infinite
                                NumberAnimation { from: 1; to: 0.25; duration: 600 }
                                NumberAnimation { from: 0.25; to: 1; duration: 600 }
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("%1 · %2 · %3").arg(modelData.label).arg(modelData.model).arg(modelData.stage)
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontSizeSmall
                            elide: Text.ElideRight
                        }
                        Label {
                            text: Format.since(modelData.since, root.now)
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSizeSmall
                            font.family: Theme.fontMono
                        }
                    }
                }
                Label {
                    visible: root.tasks.length === 0
                    text: qsTr("None: waiting for the next message or market event")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSizeSmall
                }
            }
        }

        // ---- the market being discussed ------------------------------------------------
        Panel {
            Layout.fillWidth: true
            implicitHeight: marketBody.implicitHeight + 56
            title: root.a.name !== "" ? root.a.name : qsTr("No market named yet")
            icon: "candles"
            trailing: Label {
                visible: root.a.live && root.a.live.text !== undefined
                text: root.a.live && root.a.live.text !== undefined ? root.a.live.text : ""
                color: Theme.textPrimary
                font.family: Theme.fontMono
                font.weight: Theme.weightSemibold
            }
            ColumnLayout {
                id: marketBody
                width: parent.width
                spacing: 4
                RowLayout {
                    spacing: Theme.spaceSm
                    Label { text: qsTr("Market bias"); color: Theme.textMuted; font.pixelSize: Theme.fontSizeSmall; Layout.preferredWidth: 118 }
                    NxBadge {
                        text: root.sideText(root.a.bias.side)
                        kind: root.a.bias.side === "bullish" ? "success" : root.a.bias.side === "bearish" ? "danger"
                            : root.a.bias.side === "neutral" ? "warning" : "neutral"
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: root.a.bias.side !== ""
                        text: root.sourceText(root.a.bias) + " · " + Format.ago(root.a.bias.time, root.now)
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSizeSmall - 1
                        elide: Text.ElideRight
                    }
                }
                Label {
                    Layout.fillWidth: true
                    visible: root.a.bias.why !== ""
                    text: root.a.bias.why
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSizeSmall
                    wrapMode: Text.Wrap
                }
                Row2 {
                    label: qsTr("Your notes lean")
                    value: root.a.lean.side === "" ? qsTr("no direction said yet")
                        : qsTr("%1 · %2 bullish, %3 bearish").arg(root.a.lean.side).arg(root.a.lean.bullish).arg(root.a.lean.bearish)
                    tone: root.sideColor(root.a.lean.side)
                }
                Row2 {
                    visible: root.a.live && root.a.live.rsi14 !== undefined
                    label: qsTr("Measured now")
                    value: root.a.live && root.a.live.rsi14 !== undefined
                        ? qsTr("RSI %1 (%2) · MACD %3 · trend %4 · %5, read %6").arg(Number(root.a.live.rsi14).toFixed(1))
                              .arg(root.a.live.rsiZone).arg(root.a.live.macdState).arg(root.a.live.trend)
                              .arg(root.a.live.interval).arg(Format.ago(root.a.live.readAt, root.now))
                        : ""
                }
                Row2 {
                    visible: root.a.market !== "" && !(root.a.live && root.a.live.text !== undefined)
                    label: qsTr("Live data")
                    value: qsTr("None for this market (not on your markets list)")
                    tone: Theme.warning
                }
                Row2 {
                    visible: root.a.team && root.a.team.action !== undefined
                    label: qsTr("Analysis team")
                    value: root.a.team && root.a.team.action !== undefined
                        ? qsTr("%1 · confidence %2 · %3 of %4 agree · %5").arg(String(root.a.team.action).toUpperCase())
                              .arg(Number(root.a.team.confidence).toFixed(1)).arg(root.a.team.agreeing).arg(root.a.team.voters)
                              .arg(Format.ago(root.a.team.time, root.now))
                        : ""
                }
                Label {
                    Layout.fillWidth: true
                    visible: root.a.otherMarkets.length > 0
                    text: qsTr("Also discussed: %1").arg(root.a.otherMarkets.join(", "))
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSizeSmall - 1
                    wrapMode: Text.Wrap
                }
            }
        }

        // ---- signals ---------------------------------------------------------------------
        Panel {
            Layout.fillWidth: true
            implicitHeight: signalsBody.implicitHeight + 56
            title: qsTr("Signals")
            icon: "activity"
            ColumnLayout {
                id: signalsBody
                width: parent.width
                spacing: 3
                Repeater {
                    model: root.a.bullish.concat(root.a.bearish).concat(root.a.neutral)
                    RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: Theme.spaceSm
                        Rectangle { width: 8; height: 8; radius: 4; color: root.sideColor(modelData.side) }
                        Label {
                            Layout.fillWidth: true
                            text: modelData.text
                            color: Theme.textPrimary
                            font.pixelSize: Theme.fontSizeSmall
                            elide: Text.ElideRight
                        }
                        Label {
                            text: root.sourceText(modelData)
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSizeSmall - 1
                        }
                    }
                }
                Label {
                    visible: root.a.bullish.length + root.a.bearish.length + root.a.neutral.length === 0
                    text: qsTr("None yet. Indicators and chart patterns you mention appear here.")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSizeSmall
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
            }
        }

        // ---- levels and plan ----------------------------------------------------------------
        Panel {
            Layout.fillWidth: true
            implicitHeight: planBody.implicitHeight + 56
            title: qsTr("Levels and plan")
            icon: "target"
            ColumnLayout {
                id: planBody
                width: parent.width
                spacing: 3
                Repeater {
                    model: root.a.levels
                    RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: Theme.spaceSm
                        Label {
                            Layout.preferredWidth: 82
                            text: modelData.kind
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSizeSmall
                        }
                        Label {
                            text: modelData.text
                            color: Theme.textPrimary
                            font.family: Theme.fontMono
                            font.pixelSize: Theme.fontSizeSmall
                        }
                        Label {
                            Layout.fillWidth: true
                            text: modelData.hasDistance
                                ? qsTr("%1 % %2 the price").arg(Math.abs(modelData.distancePct).toFixed(2))
                                      .arg(modelData.above ? qsTr("below") : qsTr("above"))
                                : qsTr("no live price")
                            color: modelData.near ? Theme.warning : Theme.textMuted
                            font.pixelSize: Theme.fontSizeSmall - 1
                            horizontalAlignment: Text.AlignRight
                        }
                        NxBadge { visible: modelData.near; text: qsTr("close"); kind: "warning" }
                    }
                }
                Label {
                    visible: root.a.levels.length === 0
                    text: qsTr("No levels yet. Prices you name (support, resistance, entry, stop, target) appear here and are watched against the live price.")
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSizeSmall
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
                NxDivider { Layout.fillWidth: true; Layout.topMargin: 4 }
                Row2 {
                    label: qsTr("Trade weighed")
                    value: root.a.plan.side === "" ? qsTr("none yet")
                        : (root.a.plan.side === "long" ? qsTr("LONG") : qsTr("SHORT")) + " · " + root.sourceText(root.a.plan)
                    tone: root.a.plan.side === "long" ? Theme.success : root.a.plan.side === "short" ? Theme.danger : Theme.textSecondary
                }
                Row2 {
                    label: qsTr("Entry / stop / target")
                    value: (root.a.plan.entryText || "—") + "  /  " + (root.a.plan.stopText || "—") + "  /  " + (root.a.plan.targetText || "—")
                }
                Row2 {
                    label: qsTr("Reward / risk")
                    value: root.a.plan.rewardRisk > 0 ? Number(root.a.plan.rewardRisk).toFixed(2) : root.a.plan.problem
                    tone: root.a.plan.rewardRisk >= 1.5 ? Theme.success : root.a.plan.rewardRisk > 0 ? Theme.warning : Theme.textSecondary
                }
            }
        }

        // ---- conclusions and changes ------------------------------------------------------------
        Panel {
            Layout.fillWidth: true
            implicitHeight: changesBody.implicitHeight + 56
            title: qsTr("Conclusions and changes")
            icon: "clock"
            ColumnLayout {
                id: changesBody
                width: parent.width
                spacing: 3
                Label {
                    Layout.fillWidth: true
                    visible: root.a.summary !== ""
                    text: root.a.summary
                    color: Theme.textSecondary
                    font.pixelSize: Theme.fontSizeSmall
                    font.italic: true
                    wrapMode: Text.Wrap
                }
                Repeater {
                    model: root.a.conclusions
                    Label {
                        required property var modelData
                        Layout.fillWidth: true
                        text: "› " + modelData.text + "  (" + root.sourceText(modelData) + ")"
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontSizeSmall
                        wrapMode: Text.Wrap
                    }
                }
                Label {
                    text: qsTr("Market signal changes")
                    visible: desk.signalChanges.length > 0
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSizeSmall
                    Layout.topMargin: 4
                }
                Repeater {
                    model: desk.signalChanges.slice(0, 6)
                    Label {
                        required property var modelData
                        Layout.fillWidth: true
                        text: Format.clock(modelData.time) + "  " + modelData.name + ": " + modelData.text
                        color: Theme.textPrimary
                        font.pixelSize: Theme.fontSizeSmall - 1
                        elide: Text.ElideRight
                    }
                }
                Label {
                    text: qsTr("Recent changes to the analysis")
                    visible: desk.changes.length > 0
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSizeSmall
                    Layout.topMargin: 4
                }
                Repeater {
                    model: desk.changes.slice(0, 8)
                    Label {
                        required property var modelData
                        Layout.fillWidth: true
                        text: Format.clock(modelData.time) + "  " + modelData.text + "  · " + root.sourceText(modelData)
                        color: Theme.textSecondary
                        font.pixelSize: Theme.fontSizeSmall - 1
                        elide: Text.ElideRight
                    }
                }
            }
        }
    }
}
