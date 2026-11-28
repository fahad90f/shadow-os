// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui
import org.shadow.trading

NxPage {
    id: page

    readonly property var paper: trading.paperSettings

    title: qsTr("Settings")

    function setPaper(name, value) {
        if (!trading.setPaperSetting(name, value))
            ApplicationWindow.window.toaster.show(qsTr("That value is outside the allowed range."), "warning")
    }

    function setSbt(name, value) {
        if (!trading.setSbtSetting(name, value))
            ApplicationWindow.window.toaster.show(qsTr("That value is outside the allowed range."), "warning")
    }

    // ---- where the strategy's prices come from ----------------------------------
    NxSection {
        visible: trading.priceSource.needsToken === true
        title: qsTr("Prices: OANDA")
        description: qsTr("The strategy reads its candles of gold (XAUUSD), silver (XAGUSD) and the dollar pairs from OANDA's price service, with your own OANDA token. A free practice account is enough (oanda.com → open a demo account → My Services → Manage API Access → Generate). FOREX.com, whose charts TradingView shows, has no public price service; OANDA's prices of the same markets differ from FOREX.com's by a few cents (gold) or a fraction of a pip (pairs). The token is saved in a file only you can read, sent only to OANDA, and never shown again.")
        headerActions: NxBadge {
            text: trading.priceSource.hasToken ? qsTr("Connected") : qsTr("No token")
            kind: trading.priceSource.hasToken ? "success" : "warning"
        }
        NxRow {
            icon: "key"
            title: trading.priceSource.hasToken ? qsTr("Token ending in %1").arg(trading.priceSource.hint || "····") : qsTr("No token yet")
            subtitle: trading.priceSource.hasToken
                ? (trading.priceSource.tokenSource === "environment" ? qsTr("In use, from the environment (SHADOW_TRADING_OANDA_TOKEN).") : qsTr("In use, saved."))
                : qsTr("Without a token there are no prices: no chart of the strategy, no setups, no trades.")
            NxPasswordField {
                id: oandaField
                Layout.preferredWidth: 280
                placeholderText: qsTr("Paste your OANDA token")
                Accessible.name: qsTr("OANDA token")
                onAccepted: oandaSave.clicked()
            }
            NxButton {
                id: oandaSave
                kind: "primary"
                text: qsTr("Save")
                enabled: oandaField.text.trim() !== ""
                onClicked: {
                    var error = trading.saveOandaToken(oandaField.text)
                    if (error === "") {
                        oandaField.text = ""
                        ApplicationWindow.window.toaster.show(qsTr("Token saved."), "success")
                    } else {
                        ApplicationWindow.window.toaster.show(error, "danger")
                    }
                }
            }
            NxButton {
                visible: trading.priceSource.tokenSource === "saved"
                text: qsTr("Remove")
                onClicked: trading.removeOandaToken()
                Accessible.name: qsTr("Remove the saved OANDA token")
            }
        }
        NxRow {
            icon: "user"
            title: qsTr("OANDA account")
            subtitle: qsTr("The kind of account the token belongs to. Practice: api-fxpractice.oanda.com; live: api-fxtrade.oanda.com. Only prices are read; nothing is ever ordered at OANDA.")
            NxSegmented {
                readonly property var accounts: ["practice", "live"]
                Accessible.name: qsTr("OANDA account")
                model: [qsTr("Practice"), qsTr("Live")]
                currentIndex: Math.max(0, accounts.indexOf(trading.priceSource.account))
                onActivated: (index) => trading.setOandaAccount(accounts[index])
            }
        }
    }

    // The strategy every trade comes from.
    NxSection {
        title: qsTr("Strategy: SBT Model 1")
        NxRow {
            icon: "target"
            title: qsTr("SBT Model 1 decides the trades")
            subtitle: qsTr("Bullish and bearish, by the rules below. The AI models do not trade on their own view; they review the setups this strategy finds.")
            NxBadge { text: qsTr("In use"); kind: "success" }
        }
        // The rules, as the trader wrote them, in full.
        component RuleList: ColumnLayout {
            property string heading
            property string body
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spaceMd + 2
            Layout.rightMargin: Theme.spaceMd
            Layout.topMargin: Theme.spaceSm
            Layout.bottomMargin: Theme.spaceSm
            spacing: 4
            Label {
                text: parent.heading
                color: Theme.textPrimary
                font.pixelSize: Theme.fontSizeBody
                font.weight: Theme.weightMedium
            }
            Label {
                Layout.fillWidth: true
                text: parent.body
                color: Theme.textSecondary
                font.pixelSize: Theme.fontSizeSmall
                wrapMode: Text.Wrap
                lineHeight: 1.15
            }
        }
        RuleList {
            heading: qsTr("SBT Model 1 (bullish; bearish mirrors it)")
            body: qsTr("1. There must be a proper 123.\n2. We should have a BOS.\n3. We will mark the lowest candle on 2 (bearish candle); bearish structure: the highest candle on 2 (bullish candle).\n4. Wait for the market to test this area.\n5. Testing candle should not close below the zone (bearish: above it).\n6. Enter on next candle opening.\n7. Target area of 3.")
        }
        RuleList {
            heading: qsTr("Filters: setups that do not meet them are ignored")
            body: qsTr("1. Bullish Structure: At a valid Higher Low (HL), identify the lowest bearish candle, measuring from its opening price to its lowest wick.\n2. Bearish Structure: At a valid Lower High (LH), identify the highest bullish candle, measuring from its opening price to its highest wick.\n3. Bullish Setup: The same candle must serve as both the Demand Zone and the Order Block.\n4. Bearish Setup: The same candle must serve as both the Supply Zone and the Order Block.\n5. Gap Filter: If the market opens with a Gap Up or Gap Down, do not analyze or execute any trades. Skip trading until normal market conditions resume.")
        }
        RuleList {
            heading: qsTr("What counts as a proper 123 (read from the two charts)")
            body: qsTr("• Proper legs: 1, 2 and 3 are highs and lows of real legs (the size below, over at least two candles). Small moves inside a leg are not structure.\n• A proper 3: the high after the BOS goes clearly beyond 1. A break that goes nowhere before price falls back is no 123.\n• The first return: the test is the first pullback from 3 into the zone. Once price has bounced by a proper leg, or closed beyond 3, the structure has moved on and the old zone is no trade.\n• Entry at the zone: the testing candle closes in the zone or right at it. A candle that only touches the zone and closes far away leaves no entry.\n• Target: the area of 3 (the body of the candle that made 3).")
        }
        SettingNumber {
            icon: "activity"
            title: qsTr("Proper leg: size")
            subtitle: qsTr("How far price has to move for a high or low to count as 1, 2 or 3, in average true ranges. Larger: fewer, bigger structures.")
            value: trading.sbtSettings.leg_atr; from: 1; to: 8; step: 0.5; decimals: 1; unit: qsTr("ATR")
            onEdited: (v) => page.setSbt("leg_atr", v)
        }
        SettingNumber {
            icon: "arrow-up"
            title: qsTr("Proper 3: beyond 1 by")
            subtitle: qsTr("How far the high (bearish: low) after the BOS must go beyond Level 1, in average true ranges. 0: any break counts.")
            value: trading.sbtSettings.extension_atr; from: 0; to: 5; step: 0.25; decimals: 2; unit: qsTr("ATR")
            onEdited: (v) => page.setSbt("extension_atr", v)
        }
        SettingNumber {
            icon: "target"
            title: qsTr("Entry at the zone: within")
            subtitle: qsTr("How far beyond the zone's edge the testing candle may close for the entry at the next open to count, in average true ranges.")
            value: trading.sbtSettings.entry_zone_atr; from: 0; to: 5; step: 0.25; decimals: 2; unit: qsTr("ATR")
            onEdited: (v) => page.setSbt("entry_zone_atr", v)
        }
        NxRow {
            icon: "flag"
            title: qsTr("Target")
            subtitle: qsTr("Area of 3: the body of the candle that made 3 (where the area begins). Exact: the very end of its wick.")
            NxSegmented {
                readonly property var values: ["body", "wick"]
                Accessible.name: qsTr("Target")
                model: [qsTr("Area of 3"), qsTr("Exact high / low")]
                currentIndex: Math.max(0, values.indexOf(trading.sbtSettings.target))
                onActivated: (index) => page.setSbt("target", values[index])
            }
        }
        NxRow {
            icon: "users"
            title: qsTr("AI team confirms each setup")
            subtitle: qsTr("The analysis team reviews every setup against the rules while it waits for its retest, and an entry is placed only when the team confirms it. Off, or without an NVIDIA key: the rules alone decide.")
            Switch {
                checked: trading.sbtSettings.ai_confirm
                onToggled: page.setSbt("ai_confirm", checked)
                Accessible.name: qsTr("AI team confirms each setup")
            }
        }
        NxRow {
            icon: "layers"
            title: qsTr("Also require a PD array")
            subtitle: qsTr("An extra filter, not one of the 7 rules (off by default): a fair value gap, order block or demand zone at or below Level 2 (bearish: supply above it), still unbroken and reached by the move into the zone, must also confirm the retest.")
            Switch {
                checked: trading.sbtSettings.require_pd_array
                onToggled: page.setSbt("require_pd_array", checked)
                Accessible.name: qsTr("Also require a PD array")
            }
        }
        SettingNumber {
            icon: "candles"
            title: qsTr("Demand / supply: move away")
            subtitle: qsTr("How far the move away from the zone candle must go (the best close of the next three candles beyond its open) for it to count as a demand or supply zone, in average true ranges.")
            value: trading.sbtSettings.zone_move_atr; from: 0.2; to: 5; step: 0.1; decimals: 1; unit: qsTr("ATR")
            onEdited: (v) => page.setSbt("zone_move_atr", v)
        }
        SettingNumber {
            icon: "alert-triangle"
            title: qsTr("Gap: size")
            subtitle: qsTr("A candle that opens beyond the previous candle's range and at least this far from its close is a gap up or down, in average true ranges.")
            value: trading.sbtSettings.gap_atr; from: 0.1; to: 5; step: 0.1; decimals: 1; unit: qsTr("ATR")
            onEdited: (v) => page.setSbt("gap_atr", v)
        }
        SettingNumber {
            icon: "clock"
            title: qsTr("Gap: pause")
            subtitle: qsTr("After a gap, no setups and no entries until the gap is filled or this many more candles have closed. Open trades keep their stop and target.")
            value: trading.sbtSettings.gap_pause_candles; from: 0; to: 100; step: 1
            onEdited: (v) => page.setSbt("gap_pause_candles", v)
        }
        SettingNumber {
            icon: "shield"
            title: qsTr("Stop beyond the zone")
            subtitle: qsTr("How far past the Level 2 zone the stop sits, in units of the average true range.")
            value: trading.sbtSettings.stop_buffer_atr; from: 0; to: 2; step: 0.05; decimals: 2; unit: qsTr("ATR")
            onEdited: (v) => page.setSbt("stop_buffer_atr", v)
        }
        SettingNumber {
            icon: "clock"
            title: qsTr("Wait for the retest")
            subtitle: qsTr("Candles a setup waits for its retest before it expires.")
            value: trading.sbtSettings.max_wait_candles; from: 10; to: 1000; step: 10
            onEdited: (v) => page.setSbt("max_wait_candles", v)
        }
    }

    NxSection {
        title: qsTr("Automation")
        NxRow {
            icon: "radar"
            title: qsTr("AI review of new setups")
            subtitle: qsTr("Ask the analysis team to review each new SBT setup as soon as it appears. Off: only when you choose Review now. Each review is one request per model to NVIDIA and counts against your NVIDIA account.")
            Switch {
                checked: trading.autoAnalyze
                onToggled: trading.autoAnalyze = checked
                Accessible.name: qsTr("AI review of new setups")
            }
        }
        NxRow {
            icon: "zap"
            title: qsTr("Auto trade")
            subtitle: qsTr("Place every signal that passes the risk rules on the paper account, without asking.")
            Switch {
                checked: trading.autoTrade
                onToggled: trading.autoTrade = checked
                Accessible.name: qsTr("Auto trade")
            }
        }
    }

    NxSection {
        title: qsTr("Trading mode")
        NxRow {
            icon: "flask"
            title: qsTr("Paper trading")
            subtitle: qsTr("Orders are simulated against live prices, with fees and slippage. Nothing leaves this computer and no real money is involved.")
            NxBadge { text: qsTr("In use"); kind: "success" }
        }
        NxRow {
            icon: "lock"
            title: qsTr("Live trading with real money")
            subtitle: qsTr("Not available in this version. It needs a connection to your exchange account and is added per exchange, after it has been tested on that exchange's test network.")
            NxBadge { text: qsTr("Not available"); kind: "neutral" }
        }
    }

    NxSection {
        title: qsTr("Paper account")
        description: qsTr("Fees and slippage apply to new orders. The starting balance applies when the account is reset (Portfolio page).")
        SettingNumber {
            icon: "database"
            title: qsTr("Starting balance")
            subtitle: qsTr("In %1: USDT with the crypto markets, US dollars with the forex markets.").arg(trading.accountCurrency)
            value: page.paper.starting_balance; from: 100; to: 10000000; step: 1000
            onEdited: (v) => page.setPaper("starting_balance", v)
        }
        SettingNumber {
            icon: "hash"
            title: qsTr("Fee per side")
            subtitle: qsTr("Share of the order value charged when a position opens and again when it closes.")
            value: page.paper.fee_pct; from: 0; to: 1; step: 0.01; decimals: 2; unit: "%"
            onEdited: (v) => page.setPaper("fee_pct", v)
        }
        SettingNumber {
            icon: "sliders"
            title: qsTr("Slippage")
            subtitle: qsTr("How much worse than the quoted price a market or stop order fills.")
            value: page.paper.slippage_pct; from: 0; to: 1; step: 0.01; decimals: 2; unit: "%"
            onEdited: (v) => page.setPaper("slippage_pct", v)
        }
    }

    NxSection {
        title: qsTr("Market data")
        NxRow {
            icon: "globe"
            title: qsTr("Source")
            subtitle: trading.priceSource.service === "oanda"
                ? qsTr("The forex markets: gold, silver and the currency pairs with the US dollar, from OANDA, asked for every %1 seconds. The forex market is closed at weekends.").arg(trading.pollSeconds)
                : trading.marketKind === "forex"
                ? qsTr("Public prices and candles of the currency pairs and gold that trade on Kraken, in real time. No account or key is used. These are that exchange's own prices: close to the interbank rates for the pairs that trade often there, and it trades at weekends too.")
                : qsTr("Public prices and candles, in real time. No account or key is used.")
            value: trading.dataSource
            monoValue: true
        }
        NxRow {
            icon: trading.dataState === "live" ? "check-circle" : trading.dataState === "error" ? "alert-triangle" : "clock"
            iconColor: trading.dataState === "live" ? Theme.success : trading.dataState === "error" ? Theme.warning : Theme.textSecondary
            title: trading.dataState === "live" ? qsTr("Receiving prices") : trading.dataState === "error" ? qsTr("No market data") : qsTr("Connecting…")
            subtitle: trading.dataError
        }
        NxRow {
            icon: "monitor"
            title: qsTr("TradingView chart")
            subtitle: trading.webViewAvailable
                ? qsTr("Available. FOREX.com's own chart of the market (FOREXCOM:XAUUSD and so on), loaded from tradingview.com; it needs an internet connection. The strategy chart beside it is TradingView's open-source chart library, shipped with the application, drawing the strategy's own candles with its setups and past trades.")
                : qsTr("Needs the package qml6-module-qtwebengine. Without it the AI chart is shown.")
            NxBadge {
                text: trading.webViewAvailable ? qsTr("Available") : qsTr("Not installed")
                kind: trading.webViewAvailable ? "success" : "neutral"
            }
        }
    }
}
