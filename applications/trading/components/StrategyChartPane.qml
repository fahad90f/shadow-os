// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtWebEngine

// The strategy chart: TradingView's Lightweight Charts library, compiled into
// the application, drawing the candles the strategy reads, the SBT Model 1
// setup in play and the past trades (entry, take profit, stop loss). The page
// fetches nothing; this view hands it the data (web/strategy-chart.js). Only
// loaded where the Qt WebEngine QML module is installed.
WebEngineView {
    id: view

    // {background, text, grid, up, down, accent, muted, font}
    property var colours: ({})
    property bool ready: false

    function css(c) {
        return "rgba(" + Math.round(c.r * 255) + "," + Math.round(c.g * 255) + "," + Math.round(c.b * 255) + "," + c.a + ")"
    }
    function show() {
        ready = false
        var theme = {}
        for (var name in colours)
            theme[name] = typeof colours[name] === "string" ? colours[name] : css(colours[name])
        loadHtml(trading.strategyChartPage(JSON.stringify(theme)), "https://shadow-trading.invalid/")
    }
    // The candles: all of them for a new market, else the newest.
    function sync() {
        if (!ready)
            return
        runJavaScript("JSON.stringify(window.shadow ? window.shadow.state() : null)", function (text) {
            if (!text)
                return
            var state = JSON.parse(text)
            var json = trading.chartJson(state.key, state.first, state.last)
            if (json === "")
                return
            var full = json.indexOf('"full":true') >= 0
            runJavaScript("window.shadow.apply(" + json + ")", function () { if (full) view.drawOverlay() })
        })
    }
    function drawOverlay() {
        if (!ready)
            return
        var data = {
            intervalMs: trading.intervalMs,
            sbt: trading.sbt,
            history: trading.sbtHistory,
            position: trading.position.symbol === trading.symbol ? trading.position : {}
        }
        runJavaScript("window.shadow.overlay(" + JSON.stringify(data) + ")")
    }

    backgroundColor: "transparent"
    settings.javascriptEnabled: true
    settings.localContentCanAccessRemoteUrls: false
    Component.onCompleted: show()
    // The TradingView attribution link opens in the browser.
    onNewWindowRequested: (request) => Qt.openUrlExternally(request.requestedUrl)
    onNavigationRequested: (request) => {
        if (request.url.toString().startsWith("http") && !request.url.toString().startsWith("https://shadow-trading.invalid")) {
            request.reject()
            Qt.openUrlExternally(request.url)
        }
    }
    onLoadingChanged: (info) => {
        if (info.status === WebEngineView.LoadSucceededStatus) {
            ready = true
            sync()
        }
    }
    // The page asks for older candles when the chart reaches its left edge.
    onJavaScriptConsoleMessage: (level, message, line, source) => {
        if (message === "shadow:older" && trading.hasOlder && !trading.loadingOlder)
            trading.loadOlder()
    }

    Timer { id: candleTimer; interval: 250; onTriggered: view.sync() }
    Timer { id: overlayTimer; interval: 250; onTriggered: view.drawOverlay() }
    Connections {
        target: trading
        function onChartChanged() { candleTimer.restart() }
        function onSignalChanged() { overlayTimer.restart() }
        function onSbtHistoryChanged() { overlayTimer.restart() }
        function onAccountChanged() { overlayTimer.restart() }
    }
}
