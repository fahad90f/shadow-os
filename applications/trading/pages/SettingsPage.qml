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

    NxSection {
        title: qsTr("Automation")
        NxRow {
            icon: "radar"
            title: qsTr("Analyze when a candle closes")
            subtitle: qsTr("Ask the analysts each time a candle of the selected market closes. Off: only when you choose Analyze now. Each analysis is one request per model to NVIDIA and counts against your NVIDIA account.")
            Switch {
                checked: trading.autoAnalyze
                onToggled: trading.autoAnalyze = checked
                Accessible.name: qsTr("Analyze when a candle closes")
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
            icon: "layers"
            title: qsTr("Markets")
            subtitle: qsTr("Crypto: coins against USDT. Forex: the US dollar against the euro, the pound and other currencies, and gold. Each has its own list of markets. The paper account is one and the same; switch with no position open.")
            NxSegmented {
                readonly property var kinds: ["crypto", "forex"]
                Accessible.name: qsTr("Markets")
                model: [qsTr("Crypto"), qsTr("Forex")]
                currentIndex: Math.max(0, kinds.indexOf(trading.marketKind))
                onActivated: (index) => {
                    var error = trading.setMarketKind(kinds[index])
                    if (error !== "") {
                        ApplicationWindow.window.toaster.show(error, "warning")
                        currentIndex = Qt.binding(() => Math.max(0, kinds.indexOf(trading.marketKind)))
                    }
                }
            }
        }
        NxRow {
            icon: "globe"
            title: qsTr("Source")
            subtitle: trading.marketKind === "forex"
                ? qsTr("Public prices and candles of the currency pairs and gold that trade on Kraken, in real time. No account or key is used. These are that exchange's own prices: close to the interbank rates for the pairs that trade often there, and it trades at weekends too.")
                : qsTr("Public prices and candles, in real time. No account or key is used. Gold and the euro are shown as they trade on this exchange (PAXG and EUR against USDT); the currency pairs themselves are the forex markets.")
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
                ? qsTr("Available. The chart is loaded from tradingview.com and needs an internet connection.")
                : qsTr("Needs the package qml6-module-qtwebengine. Without it the AI chart is shown.")
            NxBadge {
                text: trading.webViewAvailable ? qsTr("Available") : qsTr("Not installed")
                kind: trading.webViewAvailable ? "success" : "neutral"
            }
        }
    }
}
