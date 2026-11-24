// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.trading
import "../components/Format.js" as Format

// The rules every trade has to pass, and where the account stands against them.
NxPage {
    id: page

    readonly property var account: trading.account
    readonly property var limits: trading.riskSettings

    title: qsTr("Risk Manager")
    subtitle: qsTr("Fixed rules applied to every signal before anything is placed. No model can override them.")

    function set(name, value) {
        if (!trading.setRiskSetting(name, value))
            ApplicationWindow.window.toaster.show(qsTr("That value is outside the allowed range."), "warning")
    }

    NxBanner {
        Layout.fillWidth: true
        visible: page.account.halted
        kind: "danger"
        title: qsTr("Trading is halted")
        message: qsTr("The account fell %1 below its peak, past the %2 drawdown limit. Open positions keep their stops; no new trade is placed until you resume. Resuming measures drawdown from the current value.")
            .arg(Format.pct(page.account.drawdownPct, 1)).arg(Format.pct(page.limits.max_drawdown_pct, 0))
        actionText: qsTr("Resume trading")
        onActionTriggered: trading.resumeTrading()
    }
    NxBanner {
        Layout.fillWidth: true
        visible: !page.account.halted && page.account.dailyLossReached
        kind: "warning"
        title: qsTr("No new trades today")
        message: qsTr("Today's loss reached the daily limit of %1. Trading continues on the next UTC day.").arg(Format.pct(page.limits.max_daily_loss_pct, 1))
    }

    RiskCard {
        Layout.fillWidth: true
        implicitHeight: 160
        showLink: false
    }

    NxSection {
        title: qsTr("Position size")
        SettingNumber {
            icon: "pie"
            title: qsTr("Risk per trade")
            subtitle: qsTr("Share of the account lost if the stop is hit. The order size is worked out from this and the distance to the stop, and is capped at one position’s share of the account.")
            value: page.limits.risk_per_trade_pct; from: 0.1; to: 5; step: 0.1; decimals: 1; unit: "%"
            onEdited: (v) => page.set("risk_per_trade_pct", v)
        }
        SettingNumber {
            icon: "layers"
            title: qsTr("Open positions at once")
            subtitle: qsTr("One position per market, and no more than this many in total. Each may use at most its share of the account: a third with three allowed.")
            value: page.limits.max_open_positions; from: 1; to: 10
            onEdited: (v) => page.set("max_open_positions", v)
        }
    }

    NxSection {
        title: qsTr("Which signals are traded")
        SettingNumber {
            icon: "gauge"
            title: qsTr("Minimum confidence")
            subtitle: qsTr("Consensus score a signal needs, from 0 to 100.")
            value: page.limits.min_confidence; from: 30; to: 100; step: 5
            onEdited: (v) => page.set("min_confidence", v)
        }
        SettingNumber {
            icon: "users"
            title: qsTr("Analysts that must agree")
            subtitle: qsTr("At most the number that answered: with no model connected, the rule-based analyst decides alone.")
            value: page.limits.min_agreeing; from: 1; to: 5
            onEdited: (v) => page.set("min_agreeing", v)
        }
        SettingNumber {
            icon: "target"
            title: qsTr("Minimum reward to risk")
            subtitle: qsTr("Distance to the target divided by distance to the stop.")
            value: page.limits.min_reward_risk; from: 1; to: 10; step: 0.1; decimals: 1
            onEdited: (v) => page.set("min_reward_risk", v)
        }
    }

    NxSection {
        title: qsTr("Loss limits")
        SettingNumber {
            icon: "clock"
            title: qsTr("Daily loss limit")
            subtitle: qsTr("After losing this share of the day's starting value, no new trade is placed until the next UTC day.")
            value: page.limits.max_daily_loss_pct; from: 0.5; to: 20; step: 0.5; decimals: 1; unit: "%"
            onEdited: (v) => page.set("max_daily_loss_pct", v)
        }
        SettingNumber {
            icon: "shield-alert"
            title: qsTr("Maximum drawdown")
            subtitle: qsTr("When the account is this far below its peak, trading halts until you resume it.")
            value: page.limits.max_drawdown_pct; from: 1; to: 50; step: 1; unit: "%"
            onEdited: (v) => page.set("max_drawdown_pct", v)
        }
        SettingNumber {
            icon: "timer"
            title: qsTr("Cooldown after a loss")
            subtitle: qsTr("Minutes to wait before trading the same market again after a losing trade.")
            value: page.limits.cooldown_minutes; from: 0; to: 1440; step: 5; unit: qsTr("min")
            onEdited: (v) => page.set("cooldown_minutes", v)
        }
    }
}
