// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.trading
import "../components/Format.js" as Format

// Everything behind the last signal: each analyst's verdict, the risk rules
// it went through, and the readings the analysts were given.
NxPage {
    id: page

    readonly property var signalData: trading.signal
    readonly property bool ready: (signalData.state || "") === "ready"
    readonly property var readings: trading.readings

    title: qsTr("AI Analysis")
    subtitle: ready ? qsTr("%1 · %2 · analyzed at %3").arg(signalData.symbolName).arg(signalData.interval).arg(Format.clock(signalData.time))
                    : qsTr("%1 · %2").arg(trading.symbolName).arg(trading.interval)
    maxContentWidth: 1100
    actions: NxButton {
        kind: "primary"
        text: qsTr("Analyze now")
        iconName: "refresh"
        busy: trading.analyzing
        enabled: !trading.analyzing && page.readings.taValid === true
        onClicked: trading.analyzeNow()
    }

    Labels { id: words }

    NxEmptyState {
        Layout.fillWidth: true
        Layout.preferredHeight: 200
        visible: !page.ready
        icon: "radar"
        title: trading.analyzing ? qsTr("Analyzing…") : qsTr("No analysis yet")
        message: page.readings.taValid === true
            ? qsTr("The analysts are asked each time a candle closes, or when you choose Analyze now.")
            : qsTr("Waiting for 60 closed candles of price history.")
    }

    NxSection {
        visible: page.ready
        title: qsTr("Consensus")
        description: qsTr("Confidence is the agreeing analysts' confidence minus the opposing analysts', divided by everyone who answered. It measures agreement, not the chance of profit.")
        NxRow {
            icon: page.signalData.action === "buy" ? "arrow-up" : page.signalData.action === "sell" ? "arrow-down" : "pause"
            iconColor: words.actionColor(page.signalData.action || "hold")
            title: words.action(page.signalData.action || "hold")
            subtitle: (page.signalData.action || "hold") === "hold" ? (page.signalData.note || "")
                : qsTr("%1 of %2 analysts agree, %3 oppose, %4 hold").arg(page.signalData.agreeing).arg(page.signalData.voters)
                      .arg(page.signalData.opposing).arg(page.signalData.holding)
            value: qsTr("Confidence %1").arg(Format.number(page.signalData.confidence, 1))
        }
        NxRow {
            visible: (page.signalData.action || "hold") !== "hold"
            icon: "target"
            title: qsTr("Entry %1 · stop %2 · target %3").arg(Format.price(page.signalData.entry))
                .arg(Format.price(page.signalData.stop)).arg(Format.price(page.signalData.takeProfit))
            subtitle: qsTr("Stop and target are the middle of what the agreeing analysts proposed.")
            value: qsTr("Reward to risk %1").arg(Format.number(page.signalData.rewardRisk, 2))
            monoValue: true
        }
        NxRow {
            visible: (page.signalData.outcome || "") !== ""
            icon: page.signalData.traded ? "check-circle" : "info"
            iconColor: page.signalData.traded ? Theme.success : Theme.textSecondary
            title: page.signalData.outcome || ""
        }
    }

    NxSection {
        visible: page.ready
        title: qsTr("Analysts")
        description: qsTr("Each analyst reads the same snapshot of the market and answers on its own.")
        Repeater {
            model: page.ready ? page.signalData.verdicts : []
            NxRow {
                required property var modelData
                icon: words.roleIcon(modelData.role)
                iconColor: modelData.ok ? words.actionColor(modelData.action) : Theme.warning
                title: qsTr("%1 · %2").arg(modelData.name).arg(words.role(modelData.role))
                subtitle: modelData.ok ? modelData.reasoning : qsTr("No verdict: %1").arg(modelData.error)
                value: !modelData.ok ? ""
                    : modelData.action === "hold" ? qsTr("Hold")
                    : qsTr("%1 · %2").arg(words.action(modelData.action)).arg(Format.number(modelData.confidence, 0))
            }
        }
    }

    NxSection {
        visible: page.ready
        title: qsTr("Risk rules")
        description: qsTr("Fixed arithmetic on the signal and the account. No model takes part, so no model can override them.")
        Repeater {
            model: page.ready ? page.signalData.risk.checks : []
            NxRow {
                required property var modelData
                icon: modelData.pass ? "check-circle" : "x-circle"
                iconColor: modelData.pass ? Theme.success : Theme.danger
                title: words.riskLabel(modelData.key, modelData.label)
                value: modelData.detail
            }
        }
    }

    NxSection {
        title: qsTr("Readings")
        description: qsTr("What the TA Indicators and SMC Patterns skills measure on %1 %2 right now.").arg(trading.symbolName).arg(trading.interval)
        NxRow {
            visible: page.readings.taValid !== true
            icon: "clock"
            title: qsTr("Indicators are not available yet")
            subtitle: page.readings.taReason || ""
        }
        NxRow {
            visible: page.readings.taValid === true
            icon: "activity"
            title: qsTr("Trend")
            subtitle: qsTr("EMA 20 %1 · EMA 50 %2 · EMA 200 %3").arg(Format.price(page.readings.ema20))
                .arg(Format.price(page.readings.ema50)).arg(Format.price(page.readings.ema200))
            value: page.readings.trend || ""
        }
        NxRow {
            visible: page.readings.taValid === true
            icon: "gauge"
            title: qsTr("Momentum")
            subtitle: qsTr("RSI 14 at %1 · MACD histogram %2").arg(Format.number(page.readings.rsi14, 1)).arg(page.readings.macdState || "")
            value: page.readings.momentum || ""
        }
        NxRow {
            visible: page.readings.taValid === true
            icon: "sliders"
            title: qsTr("Volatility and volume")
            subtitle: qsTr("ATR 14 is %1 of price · Bollinger width %2 · volume %3 times its average")
                .arg(Format.pct(page.readings.atrPct, 2)).arg(Format.pct(page.readings.bandWidthPct, 2))
                .arg(Format.number(page.readings.volumeRatio, 2))
            value: page.readings.volumeState || ""
        }
        NxRow {
            visible: page.readings.smcValid === true
            icon: "grid"
            title: qsTr("Market structure")
            subtitle: qsTr("Range %1 to %2 · %3 open order blocks · %4 open fair value gaps · %5 liquidity sweeps")
                .arg(Format.price(page.readings.rangeLow)).arg(Format.price(page.readings.rangeHigh))
                .arg(page.readings.orderBlocks || 0).arg(page.readings.fairValueGaps || 0).arg(page.readings.sweeps || 0)
            value: qsTr("%1 · %2").arg(page.readings.structureTrend || "").arg(page.readings.zone || "")
        }
    }

    NxSection {
        title: qsTr("Activity")
        description: qsTr("What the engine did in this session, newest first.")
        NxRow {
            visible: trading.activity.length === 0
            icon: "list"
            title: qsTr("Nothing yet")
        }
        Repeater {
            model: trading.activity.slice(0, 40)
            NxRow {
                required property var modelData
                icon: modelData.kind === "trade" ? "zap" : modelData.kind === "signal" ? "radar"
                    : modelData.kind === "risk" ? "shield" : modelData.kind === "error" ? "alert-triangle" : "info"
                iconColor: modelData.kind === "error" ? Theme.warning : modelData.kind === "trade" ? Theme.accent : Theme.textSecondary
                title: modelData.text
                value: Format.clock(modelData.time)
                monoValue: true
            }
        }
    }
}
