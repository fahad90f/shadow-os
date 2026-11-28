// SPDX-License-Identifier: GPL-3.0-or-later
#include "deskengine.h"
#include "tradingconfig.h"
#include "tradingengine.h"

#include <appearance.h>
#include <shadowapp.h>

#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
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
                       QStringLiteral("signal"), QStringLiteral("session"), QStringLiteral("desk")});
    if (!app.processArguments())
        return 0;

    // Owned by the QML engine, so they outlive every binding that reads them.
    auto *configObject = new TradingConfig(&app.engine());
    auto *engineObject = new TradingEngine(configObject, &app.engine());
    TradingEngine &engine = *engineObject;
    // The desk follows the conversation and the market from the start, on
    // whatever page the window shows.
    auto *deskObject = new DeskEngine(configObject, engineObject, &app.engine());

    const QString topic = app.dumpTopic();
    if (!topic.isEmpty()) {
        if (topic == QLatin1String("config"))
            return app.finishDump(engine.dumpConfig());
        if (topic == QLatin1String("account"))
            return app.finishDump(engine.dumpAccount());
        if (topic != QLatin1String("market") && topic != QLatin1String("signal") && topic != QLatin1String("session")
            && topic != QLatin1String("desk"))
            return app.dumpError(QStringLiteral("unknown dump topic '%1'").arg(topic));
        engine.start();
        app.waitFor(&engine, SIGNAL(marketReady()), 30000);
        if (topic == QLatin1String("desk")) {
            // The desk as a person would use it, for the tests:
            // SHADOW_TRADING_DESK_SCRIPT='[{"post": "BTC is approaching 95,000"}, {"wait": 0.5},
            //   {"post": "short?", "token": "t1"}, {"idle": 60}, {"stop": "last"}, {"retry": "last"},
            //   {"select": "ETHUSDT"}, {"clear": true}]'
            // "idle" waits until nothing is under way (at most that many seconds).
            DeskEngine &desk = *deskObject;
            desk.start();
            QObject never;
            const auto wait = [&app, &never](double seconds) {
                app.waitFor(&never, SIGNAL(destroyed()), int(seconds * 1000));
            };
            QJsonArray done;
            // The newest answer of the desk, for "stop" and "retry".
            const auto lastDesk = [&desk] {
                const QList<DeskMessages::Message> &all = desk.messages()->all();
                for (auto it = all.crbegin(); it != all.crend(); ++it) {
                    if (it->role == QLatin1String("desk"))
                        return double(it->id);
                }
                return 0.0;
            };
            const QJsonArray steps = QJsonDocument::fromJson(qgetenv("SHADOW_TRADING_DESK_SCRIPT")).array();
            for (const QJsonValue &v : steps) {
                const QJsonObject step = v.toObject();
                QJsonObject result{{QStringLiteral("step"), step}};
                if (step.contains(QLatin1String("post"))) {
                    QElapsedTimer t;
                    t.start();
                    const QVariantMap r = desk.post(step.value(QLatin1String("post")).toString(),
                                                    step.value(QLatin1String("token")).toString());
                    result.insert(QStringLiteral("us"), double(t.nsecsElapsed()) / 1000.0);
                    result.insert(QStringLiteral("result"), QJsonObject::fromVariantMap(r));
                } else if (step.contains(QLatin1String("wait"))) {
                    wait(qBound(0.0, step.value(QLatin1String("wait")).toDouble(), 300.0));
                } else if (step.contains(QLatin1String("idle"))) {
                    QElapsedTimer t;
                    t.start();
                    const double limit = qBound(0.1, step.value(QLatin1String("idle")).toDouble(), 300.0);
                    // A short settle first: a message is read on the next turn of the event loop.
                    wait(0.3);
                    while (!desk.idle() && t.elapsed() < limit * 1000)
                        wait(0.1);
                    result.insert(QStringLiteral("idle"), desk.idle());
                    result.insert(QStringLiteral("ms"), double(t.elapsed()));
                } else if (step.contains(QLatin1String("stop"))) {
                    desk.stopAnswer(lastDesk());
                } else if (step.contains(QLatin1String("retry"))) {
                    desk.retryAnswer(lastDesk());
                } else if (step.contains(QLatin1String("select"))) {
                    engine.setSymbol(step.value(QLatin1String("select")).toString());
                    app.waitFor(&engine, SIGNAL(marketReady()), 20000);
                } else if (step.contains(QLatin1String("analyze"))) {
                    result.insert(QStringLiteral("started"), engine.analyzeNow());
                } else if (step.contains(QLatin1String("clear"))) {
                    desk.clearConversation();
                } else {
                    result.insert(QStringLiteral("error"), QStringLiteral("unknown step"));
                }
                done.append(result);
            }
            QJsonObject out = desk.dump();
            out.insert(QStringLiteral("steps"), done);
            out.insert(QStringLiteral("signal"), QJsonObject::fromVariantMap(engine.signal()));
            out.insert(QStringLiteral("quote"), QJsonObject::fromVariantMap(engine.quote()));
            out.insert(QStringLiteral("symbol"), engine.symbol());
            return app.finishDump(out);
        }
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
                    if (engine.analyzeNow() || engine.analyzing())
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
                } else if (action.startsWith(QLatin1String("oanda-token:"))) {
                    result = engine.saveOandaToken(action.mid(12));
                    if (result.isEmpty())
                        app.waitFor(&engine, SIGNAL(marketReady()), 20000);
                } else if (action == QLatin1String("oanda-forget")) {
                    engine.removeOandaToken();
                } else if (action.startsWith(QLatin1String("kind:"))) {
                    // The other kind of market: crypto or forex.
                    result = engine.setMarketKind(action.mid(5));
                    if (result.isEmpty())
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
        if (engine.analyzeNow() || engine.analyzing())
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
    app.expose(QStringLiteral("desk"), deskObject);
    // For screenshots and manual testing: one analysis as soon as prices are in.
    if (qEnvironmentVariableIntValue("SHADOW_TRADING_ANALYZE_ON_START") == 1) {
        QObject::connect(&engine, &TradingEngine::marketReady, &engine, [&engine] { engine.analyzeNow(); },
                         Qt::SingleShotConnection);
    }
    engine.start();
    deskObject->start();
    return app.run(QStringLiteral("org.shadow.trading"));
}
