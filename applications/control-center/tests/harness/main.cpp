// SPDX-License-Identifier: GPL-3.0-or-later
// Test harness (not part of the product): drives KillSwitchBackend from the
// command line, one operation per argument, and prints one JSON line per
// operation with the result and the back-end's whole state (toJson()).
//
//   show              nothing; print the state
//   set:MODE          setMode(MODE)
//   lan:0|1           setLan()
//   keep:0|1          keepAfterRestart = ...
//   reassert          reassert()
//   release           releasePanic()
//   check             checkLive()
//   closes:MODE:0|1   print closesConnections(MODE, lan)
//   wait:MS           let the event loop run for MS milliseconds (file watcher)
#include "privacykillswitchbackend.h"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include <cstdio>
#include <functional>

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    QStringList ops = app.arguments().mid(1);
    KillSwitchBackend backend;
    QString result, message;
    QObject::connect(&backend, &KillSwitchBackend::failed, &app, [&](const QString &m) { result = QStringLiteral("failed"); message = m; });
    QObject::connect(&backend, &KillSwitchBackend::succeeded, &app, [&](const QString &m) { result = QStringLiteral("succeeded"); message = m; });

    QJsonObject extra;
    QString current;
    QElapsedTimer clock;
    clock.start();
    std::function<void()> step;
    step = [&] {
        if (clock.elapsed() > 60000) {
            std::printf("HARNESS TIMEOUT\n");
            app.exit(3);
            return;
        }
        // Wait until the previous operation and the asynchronous reads are done.
        if (backend.busy() || !backend.settled()) {
            QTimer::singleShot(20, &app, step);
            return;
        }
        if (!current.isEmpty()) {
            QJsonObject line{{QStringLiteral("op"), current}, {QStringLiteral("result"), result},
                             {QStringLiteral("message"), message}, {QStringLiteral("backend"), backend.toJson()}};
            for (auto it = extra.begin(); it != extra.end(); ++it)
                line.insert(it.key(), it.value());
            std::printf("%s\n", QJsonDocument(line).toJson(QJsonDocument::Compact).constData());
            std::fflush(stdout);
            current.clear();
            result.clear();
            message.clear();
            extra = {};
        }
        if (ops.isEmpty()) {
            app.quit();
            return;
        }
        current = ops.takeFirst();
        const QStringList f = current.split(QLatin1Char(':'));
        const QString op = f.value(0);
        if (op == QLatin1String("set"))
            backend.setMode(f.value(1));
        else if (op == QLatin1String("lan"))
            backend.setLan(f.value(1) == QLatin1String("1"));
        else if (op == QLatin1String("keep"))
            backend.setKeepAfterRestart(f.value(1) == QLatin1String("1"));
        else if (op == QLatin1String("reassert"))
            backend.reassert();
        else if (op == QLatin1String("release"))
            backend.releasePanic();
        else if (op == QLatin1String("check"))
            backend.checkLive();
        else if (op == QLatin1String("closes"))
            extra.insert(QStringLiteral("closes"), backend.closesConnections(f.value(1), f.value(2) == QLatin1String("1")));
        else if (op != QLatin1String("show") && op != QLatin1String("wait"))
            extra.insert(QStringLiteral("unknownOp"), true);
        // Let queued signals (process start, file watcher) run first.
        QTimer::singleShot(op == QLatin1String("wait") ? qBound(50, f.value(1).toInt(), 10000) : 50, &app, step);
    };
    QTimer::singleShot(0, &app, step);
    return app.exec();
}
