// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import org.shadow.ui

NxPage {
    title: qsTr("About Shadow Trading")
    subtitle: qsTr("Version %1").arg(shadowApp.version)

    NxSection {
        title: qsTr("How a trade is decided")
        NxRow {
            icon: "activity"
            title: qsTr("1. Prices")
            subtitle: qsTr("Candles of the selected market come from the exchange's public data every few seconds.")
        }
        NxRow {
            icon: "sliders"
            title: qsTr("2. Skills")
            subtitle: qsTr("TA Indicators and SMC Patterns measure the candles: trend, momentum, volatility, market structure. These are fixed formulas.")
        }
        NxRow {
            icon: "layers"
            title: qsTr("3. Analysts")
            subtitle: qsTr("Every model on the panel (NVIDIA-hosted models in four roles: analysis, reasoning, vision, compute) gets the same snapshot and its role's skill briefs, and answers buy, sell or hold with a stop and a target. A rule-based analyst always votes too.")
        }
        NxRow {
            icon: "radar"
            title: qsTr("4. Consensus")
            subtitle: qsTr("The votes are combined. A trade needs enough analysts on the same side; the confidence number says how strongly and how unanimously they agree.")
        }
        NxRow {
            icon: "shield"
            title: qsTr("5. Risk rules")
            subtitle: qsTr("Fixed limits size the order and can refuse it. No model takes part in this step.")
        }
        NxRow {
            icon: "zap"
            title: qsTr("6. Execution")
            subtitle: qsTr("With auto trade on, the order goes to the paper account. Its stop and target are watched on every price update.")
        }
    }

    NxSection {
        title: qsTr("What to keep in mind")
        NxRow {
            icon: "info"
            title: qsTr("This is not financial advice")
            subtitle: qsTr("Language models and indicators describe the past. Neither predicts prices reliably, and agreement between models is not evidence that a trade will win.")
        }
        NxRow {
            icon: "flask"
            title: qsTr("Results are simulated")
            subtitle: qsTr("The paper account uses live prices with fees and slippage, but real markets also have spreads, partial fills and outages. Simulated results usually look better than real ones.")
        }
        NxRow {
            icon: "key"
            title: qsTr("Your key")
            subtitle: qsTr("The NVIDIA API key is stored in a file only you can read, sent only to NVIDIA, and never written to logs.")
        }
        NxRow {
            icon: "external-link"
            title: qsTr("TradingView")
            subtitle: qsTr("The strategy chart uses TradingView Lightweight Charts™ 5.2.1, © TradingView, Inc. (https://www.tradingview.com/), under the Apache License 2.0, shipped with the application. The FOREX.com tab shows TradingView's own embeddable chart of FOREX.com's prices, loaded from tradingview.com. Prices for the strategy come from OANDA. Shadow Trading is not affiliated with TradingView, FOREX.com, OANDA, NVIDIA or the makers of the models.")
        }
    }
}
