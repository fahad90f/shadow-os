// SPDX-License-Identifier: GPL-3.0-or-later
// Test harness (not part of the product): drives the Hide Me and OS integrity
// actions of PrivacyBackend from the command line, one operation per
// argument, and prints one JSON line per operation with the result, the
// message the page would show, the back-end's Hide Me state and its Network
// identity values ("network", as in --dump privacy).
//
//   show                    nothing; print the state
//   ghost-on | ghost-off | rotate | nuke | leak-test | wipe-logs | harden-kernel | stop
//   start:SECONDS           startAutoRotate(SECONDS)
//   threat:LEVEL            setThreatModel(LEVEL)
//   ks:0|1                  enableKillSwitch()
//   proxy:0|1 dns:0|1 bt:0|1 usb:0|1
//   harden:fs               hardenFilesystems()
#include "privacybackend.h"

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
    PrivacyBackend backend;
    QString result, message;
    QObject::connect(&backend, &PrivacyBackend::failed, &app, [&](const QString &m) { result = QStringLiteral("failed"); message = m; });
    QObject::connect(&backend, &PrivacyBackend::succeeded, &app, [&](const QString &m) { result = QStringLiteral("succeeded"); message = m; });

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
        // Wait until the previous command and the state reads it started are done.
        if (!backend.settled()) {
            QTimer::singleShot(20, &app, step);
            return;
        }
        if (!current.isEmpty()) {
            const QJsonObject line{{QStringLiteral("op"), current}, {QStringLiteral("result"), result},
                                   {QStringLiteral("message"), message},
                                   {QStringLiteral("hideMe"), backend.toJson().value(QStringLiteral("hideMe"))},
                                   {QStringLiteral("network"), backend.toJson().value(QStringLiteral("network"))},
                                   {QStringLiteral("hardenOutput"), backend.hardenOutput()}};
            std::printf("%s\n", QJsonDocument(line).toJson(QJsonDocument::Compact).constData());
            std::fflush(stdout);
            current.clear();
            result.clear();
            message.clear();
        }
        if (ops.isEmpty()) {
            app.quit();
            return;
        }
        current = ops.takeFirst();
        const QStringList f = current.split(QLatin1Char(':'));
        const QString op = f.value(0);
        const bool on = f.value(1) == QLatin1String("1");
        if (op == QLatin1String("ghost-on"))
            backend.enableGhostMode();
        else if (op == QLatin1String("ghost-off"))
            backend.disableGhostMode();
        else if (op == QLatin1String("rotate"))
            backend.rotateIdentity();
        else if (op == QLatin1String("nuke"))
            backend.nukeIdentity();
        else if (op == QLatin1String("leak-test"))
            backend.runLeakTest();
        else if (op == QLatin1String("wipe-logs"))
            backend.wipeLogsNow();
        else if (op == QLatin1String("harden-kernel"))
            backend.hardenKernelNow();
        else if (op == QLatin1String("stop"))
            backend.stopAutoRotate();
        else if (op == QLatin1String("start"))
            backend.startAutoRotate(f.value(1).toInt());
        else if (op == QLatin1String("threat"))
            backend.setThreatModel(f.value(1));
        else if (op == QLatin1String("ks"))
            backend.enableKillSwitch(on);
        else if (op == QLatin1String("proxy"))
            backend.enableTransparentProxy(on);
        else if (op == QLatin1String("dns"))
            backend.enableDnsProtect(on);
        else if (op == QLatin1String("bt"))
            backend.killBluetooth(on);
        else if (op == QLatin1String("usb"))
            backend.killUsbStorage(on);
        else if (op == QLatin1String("harden") && f.value(1) == QLatin1String("fs"))
            backend.hardenFilesystems();
        else if (op != QLatin1String("show"))
            message = QStringLiteral("unknown operation");
        QTimer::singleShot(50, &app, step);
    };
    QTimer::singleShot(0, &app, step);
    return app.exec();
}
