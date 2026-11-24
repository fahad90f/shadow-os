// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtWebEngine

// TradingView's own embeddable chart for the selected market, loaded from
// tradingview.com. This file is only loaded where the Qt WebEngine QML module
// is installed (trading.webViewAvailable).
WebEngineView {
    id: view

    property string page: ""
    // True once the page reported that TradingView's script could not be
    // fetched (no internet, or the site is blocked).
    property bool unreachable: false

    function show() {
        unreachable = false
        if (page !== "")
            loadHtml(page, "https://shadow-trading.invalid/")
    }

    backgroundColor: "transparent"
    settings.javascriptEnabled: true
    settings.localContentCanAccessRemoteUrls: true
    // A neutral origin for the wrapper page; the chart itself lives in
    // TradingView's frame.
    onPageChanged: show()
    Component.onCompleted: show()
    // Links out of the chart (the TradingView attribution) open in the browser.
    onNewWindowRequested: (request) => Qt.openUrlExternally(request.requestedUrl)

    // The wrapper page shows its "could not be loaded" text when the script
    // fails; ask it a little after each load.
    onLoadingChanged: (info) => { if (info.status === WebEngineView.LoadSucceededStatus) probe.restart() }
    Timer {
        id: probe
        interval: 4000
        onTriggered: view.runJavaScript(
            "(function(){var e=document.getElementById('offline');return !!e && e.style.display==='flex'})()",
            (failed) => { view.unreachable = failed === true })
    }
}
