// SPDX-License-Identifier: GPL-3.0-or-later
#include "tradingconfig.h"
#include "tradingengine.h"

#include <appearance.h>
#include <shadowapp.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonObject>

int main(int argc, char **argv)
{
    // The TradingView tab is a Qt WebEngine view, which needs the window's
    // OpenGL contexts to be shareable; this must be set before the
    // application object exists and costs nothing when the tab is not used.
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    // The desk is always dark with a violet accent: its look is part of it.
    Shadow::Appearance::pin(true, QStringLiteral("violet"));

    Shadow::AppInfo info{
        QStringLiteral("org.shadow.trading"),
        QStringLiteral("Shadow Trading"),
        QStringLiteral("org.shadow.trading"),
        QStringLiteral("Market analysis by a panel of NVIDIA-hosted AI models, with trades placed on a paper account"),
    };
    info.singleInstance = true;
    Shadow::App app(argc, argv, info);
    app.setDumpTopics({QStringLiteral("config"), QStringLiteral("account"), QStringLiteral("market"),
                       QStringLiteral("signal"), QStringLiteral("session")});
    if (!app.processArguments())
        return 0;

    // Owned by the QML engine, so they outlive every binding that reads them.
    auto *configObject = new TradingConfig(&app.engine());
    auto *engineObject = new TradingEngine(configObject, &app.engine());
    TradingEngine &engine = *engineObject;

    const QString topic = app.dumpTopic();
    if (!topic.isEmpty()) {
        if (topic == QLatin1String("config"))
            return app.finishDump(engine.dumpConfig());
        if (topic == QLatin1String("account"))
            return app.finishDump(engine.dumpAccount());
        if (topic != QLatin1String("market") && topic != QLatin1String("signal") && topic != QLatin1String("session"))
            return app.dumpError(QStringLiteral("unknown dump topic '%1'").arg(topic));
        engine.start();
        app.waitFor(&engine, SIGNAL(marketReady()), 30000);
        if (topic == QLatin1String("session")) {
            // The engine as it runs behind the window (prices every few
            // seconds, an analysis at each candle close, stops and targets
            // watched) for SHADOW_TRADING_SESSION_SECONDS, then what happened.
            if (qEnvironmentVariableIntValue("SHADOW_TRADING_ANALYZE_ON_START") == 1 && engine.analyzeNow())
                app.waitFor(&engine, SIGNAL(analysisFinished()), 180000);
            QObject never;
            const auto wait = [&app, &never](int seconds) { app.waitFor(&never, SIGNAL(destroyed()), seconds * 1000); };
            // What a person would do in the window, for the tests:
            // SHADOW_TRADING_SESSION_ACTIONS=analyze,place,wait:5,close-all,reset,resume
            QJsonArray done;
            const QStringList actions = qEnvironmentVariable("SHADOW_TRADING_SESSION_ACTIONS")
                                            .split(QLatin1Char(','), Qt::SkipEmptyParts);
            for (const QString &action : actions) {
                QString result;
                if (action == QLatin1String("analyze")) {
                    if (engine.analyzeNow())
                        app.waitFor(&engine, SIGNAL(analysisFinished()), 180000);
                    else
                        result = QStringLiteral("not started");
                } else if (action == QLatin1String("place")) {
                    result = engine.placeSuggestedTrade();
                } else if (action == QLatin1String("close-all")) {
                    const QVariantList open = engine.positions();
                    for (const QVariant &p : open)
                        engine.closePosition(p.toMap().value(QStringLiteral("id")).toLongLong());
                    result = QString::number(open.size());
                } else if (action == QLatin1String("older")) {
                    // What moving the chart into the past does.
                    const int before = int(engine.chartCandles().size());
                    engine.loadOlder();
                    if (engine.loadingOlder())
                        app.waitFor(&engine, SIGNAL(chartChanged()), 20000);
                    result = QString::number(int(engine.chartCandles().size()) - before);
                } else if (action.startsWith(QLatin1String("add:"))) {
                    result = engine.addMarket(action.mid(4));
                } else if (action.startsWith(QLatin1String("remove:"))) {
                    result = engine.removeMarket(action.mid(7));
                } else if (action.startsWith(QLatin1String("select:"))) {
                    engine.setSymbol(action.mid(7));
                    app.waitFor(&engine, SIGNAL(marketReady()), 20000);
                } else if (action.startsWith(QLatin1String("find:"))) {
                    engine.refreshMarkets();
                    app.waitFor(&engine, SIGNAL(marketsChanged()), 20000);
                    QStringList names;
                    const QVariantList found = engine.findMarkets(action.mid(5));
                    for (const QVariant &row : found)
                        names.append(row.toMap().value(QStringLiteral("symbol")).toString());
                    result = names.join(QLatin1Char(' '));
                } else if (action == QLatin1String("reset")) {
                    engine.resetPaperAccount();
                } else if (action == QLatin1String("resume")) {
                    engine.resumeTrading();
                } else if (action.startsWith(QLatin1String("wait:"))) {
                    wait(qBound(1, action.mid(5).toInt(), 300));
                } else {
                    result = QStringLiteral("unknown action");
                }
                done.append(QJsonObject{{QStringLiteral("action"), action}, {QStringLiteral("result"), result}});
            }
            if (qEnvironmentVariableIsSet("SHADOW_TRADING_SESSION_SECONDS"))
                wait(qBound(1, qEnvironmentVariableIntValue("SHADOW_TRADING_SESSION_SECONDS"), 600));
            return app.finishDump(QJsonObject{{QStringLiteral("signal"), QJsonObject::fromVariantMap(engine.signal())},
                                              {QStringLiteral("market"), engine.dumpMarket()},
                                              {QStringLiteral("account"), engine.dumpAccount()},
                                              {QStringLiteral("actions"), done}});
        }
        if (topic == QLatin1String("market")) {
            // The prices of the watchlist arrive separately from the candles
            // (and so do the 24-hour lines, which also say "market changed").
            const auto priced = [&engine] {
                return engine.watchlist().value(0).toMap().value(QStringLiteral("hasPrice")).toBool();
            };
            for (int i = 0; i < 10 && !priced() && engine.dataState() != QLatin1String("error"); ++i)
                app.waitFor(&engine, SIGNAL(marketChanged()), 2000);
            return app.finishDump(engine.dumpMarket());
        }
        // One analysis of the selected market, by every analyst that is set up.
        if (engine.analyzeNow())
            app.waitFor(&engine, SIGNAL(analysisFinished()), 180000);
        QJsonObject out{{QStringLiteral("signal"), QJsonObject::fromVariantMap(engine.signal())},
                        {QStringLiteral("market"), engine.dumpMarket()},
                        {QStringLiteral("account"), engine.dumpAccount()}};
        return app.finishDump(out);
    }

    // The TradingView tab is offered only where the Qt WebEngine QML module
    // is installed (package qml6-module-qtwebengine).
    bool webView = false;
    const QStringList importPaths = app.engine().importPathList();
    for (const QString &path : importPaths)
        webView = webView || QFile::exists(path + QStringLiteral("/QtWebEngine/qmldir"));
    engine.setWebViewAvailable(webView);

    app.expose(QStringLiteral("trading"), &engine);
    // For screenshots and manual testing: one analysis as soon as prices are in.
    if (qEnvironmentVariableIntValue("SHADOW_TRADING_ANALYZE_ON_START") == 1) {
        QObject::connect(&engine, &TradingEngine::marketReady, &engine, [&engine] { engine.analyzeNow(); },
                         Qt::SingleShotConnection);
    }
    engine.start();
    return app.run(QStringLiteral("org.shadow.trading"));
}
