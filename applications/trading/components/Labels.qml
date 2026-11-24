// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.shadow.ui

// The words and colours for the engine's states, in one place so every page
// says the same thing.
QtObject {
    function role(key) {
        return key === "analysis" ? qsTr("Analysis") : key === "reasoning" ? qsTr("Reasoning")
            : key === "vision" ? qsTr("Vision") : key === "compute" ? qsTr("Compute")
            : key === "rules" ? qsTr("Fixed rules") : key
    }
    function roleIcon(key) {
        return key === "analysis" ? "activity" : key === "reasoning" ? "layers"
            : key === "vision" ? "eye" : key === "compute" ? "cpu" : "sliders"
    }
    function skillName(id) {
        return id === "ta" ? qsTr("TA") : id === "smc" ? qsTr("SMC") : id === "risk" ? qsTr("Risk") : qsTr("Execution")
    }
    function skillRole(id) {
        return id === "ta" ? qsTr("Indicators") : id === "smc" ? qsTr("Patterns") : qsTr("Engine")
    }
    function skillIcon(id) {
        return id === "ta" ? "sliders" : id === "smc" ? "grid" : id === "risk" ? "shield" : "zap"
    }
    function stateText(state) {
        return state === "active" ? qsTr("Active") : state === "working" ? qsTr("Working")
            : state === "ready" ? qsTr("Ready") : state === "no_key" ? qsTr("No key")
            : state === "off" ? qsTr("Off") : state === "waiting" ? qsTr("Waiting")
            : state === "halted" ? qsTr("Halted") : state === "unavailable" ? qsTr("Unavailable") : qsTr("Error")
    }
    function stateKind(state) {
        return state === "active" ? "success" : state === "working" ? "accent"
            : state === "no_key" || state === "waiting" || state === "unavailable" ? "warning"
            : state === "error" || state === "halted" ? "danger" : "neutral"
    }
    function stateColor(state) {
        var kind = stateKind(state)
        return kind === "success" ? Theme.success : kind === "accent" ? Theme.accent
            : kind === "warning" ? Theme.warning : kind === "danger" ? Theme.danger : Theme.textMuted
    }
    function action(key) {
        return key === "buy" ? qsTr("BUY") : key === "sell" ? qsTr("SELL") : qsTr("HOLD")
    }
    function actionColor(key) {
        return key === "buy" ? Theme.success : key === "sell" ? Theme.danger : Theme.textSecondary
    }
    function actionKind(key) {
        return key === "buy" ? "success" : key === "sell" ? "danger" : "neutral"
    }
    function closeReason(key) {
        return key === "take_profit" ? qsTr("Target reached") : key === "stop_loss" ? qsTr("Stopped out")
            : key === "signal_flip" ? qsTr("Signal reversed") : key === "risk_halt" ? qsTr("Risk halt")
            : qsTr("Closed by you")
    }
    function riskLabel(key, fallback) {
        return key === "signal" ? qsTr("Signal") : key === "confidence" ? qsTr("Confidence")
            : key === "agreement" ? qsTr("Analysts agreeing") : key === "stop" ? qsTr("Stop loss")
            : key === "reward" ? qsTr("Reward to risk") : key === "positions" ? qsTr("Open positions")
            : key === "daily_loss" ? qsTr("Daily loss limit") : key === "drawdown" ? qsTr("Max drawdown")
            : key === "cooldown" ? qsTr("Cooldown after a loss") : key === "size" ? qsTr("Position size")
            : fallback
    }
    function skillTitle(id) {
        return id === "ta" ? qsTr("TA Indicators") : id === "smc" ? qsTr("SMC Patterns")
            : id === "risk" ? qsTr("Risk Engine") : id === "chart" ? qsTr("Chart Reading") : qsTr("Execution Engine")
    }
    function keySource(source) {
        return source === "environment" ? qsTr("from the environment")
            : source === "assistant" ? qsTr("shared with Shadow Assistant")
            : source === "saved" ? qsTr("saved here") : ""
    }
}
