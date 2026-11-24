// SPDX-License-Identifier: GPL-3.0-or-later
// Test harness (not part of the product): drives NetPrivacy from the command
// line, one operation per argument, and prints one JSON line per operation
// with the result of operationFinished() and the back-end's state (dump()).
//
//   show                              refresh, then print
//   dns|MODE|PROVIDER|DNSSEC          setDns()
//   mac|MODE                          setMac()
//   keep|UUID|0|1                     setMacException()
//   check|MODE|PROVIDER|DNSSEC        print dnsRequestError()
//   custom|TEXT                       print customServersError() and customProvider()
#include "netprivacy.h"

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
    NetPrivacy privacy;
    QJsonObject finished;
    QObject::connect(&privacy, &NetPrivacy::operationFinished, &app,
                     [&](const QString &operation, const QString &target, bool ok, const QString &message) {
        finished = {{QStringLiteral("operation"), operation}, {QStringLiteral("target"), target},
                    {QStringLiteral("ok"), ok}, {QStringLiteral("message"), message}};
    });
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
        if (privacy.busy() || privacy.refreshing() || !privacy.ready()) {
            QTimer::singleShot(20, &app, step);
            return;
        }
        if (!current.isEmpty()) {
            QJsonObject line{{QStringLiteral("op"), current}, {QStringLiteral("finished"), finished},
                             {QStringLiteral("state"), privacy.dump()}};
            for (auto it = extra.begin(); it != extra.end(); ++it)
                line.insert(it.key(), it.value());
            std::printf("%s\n", QJsonDocument(line).toJson(QJsonDocument::Compact).constData());
            std::fflush(stdout);
            current.clear();
            finished = {};
            extra = {};
        }
        if (ops.isEmpty()) {
            app.quit();
            return;
        }
        current = ops.takeFirst();
        const QStringList f = current.split(QLatin1Char('|'));
        const QString op = f.value(0);
        if (op == QLatin1String("show"))
            privacy.refresh();
        else if (op == QLatin1String("dns"))
            privacy.setDns(f.value(1), f.value(2), f.value(3));
        else if (op == QLatin1String("mac"))
            privacy.setMac(f.value(1));
        else if (op == QLatin1String("keep"))
            privacy.setMacException(f.value(1), f.value(2) == QLatin1String("1"));
        else if (op == QLatin1String("check"))
            extra.insert(QStringLiteral("error"), privacy.dnsRequestError(f.value(1), f.value(2), f.value(3)));
        else if (op == QLatin1String("custom")) {
            extra.insert(QStringLiteral("error"), privacy.customServersError(f.value(1)));
            extra.insert(QStringLiteral("provider"), privacy.customProvider(f.value(1)));
        } else
            extra.insert(QStringLiteral("unknownOp"), true);
        QTimer::singleShot(50, &app, step);
    };
    privacy.refresh();
    QTimer::singleShot(0, &app, step);
    return app.exec();
}
