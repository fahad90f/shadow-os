// SPDX-License-Identifier: GPL-3.0-or-later
// Test harness (not part of the product): runs Diagnostics tools from the command line.
#include "diagnostics.h"
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <cstdio>
int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    Diagnostics d;
    QStringList ops = app.arguments().mid(1);
    int liveChunks = 0;
    QObject::connect(&d, &Diagnostics::outputChanged, &d, [&] { ++liveChunks; });
    std::function<void()> next = [&] {
        if (ops.isEmpty()) { app.quit(); return; }
        const QStringList f = ops.takeFirst().split('|');
        printf("--> %s\n", qPrintable(f.join(' '))); fflush(stdout);
        liveChunks = 0; d.clearOutput();
        const QString c = f.at(0);
        if (c == "ping") d.ping(f.at(1), f.value(2, "auto"), f.value(3, "2").toInt());
        else if (c == "trace") d.traceRoute(f.at(1), f.value(2, "auto"));
        else if (c == "dns") d.dnsLookup(f.at(1), f.value(2, "A"), f.value(3));
        else if (c == "port") d.portCheck(f.at(1), f.at(2).toInt(), f.value(3, "auto"), 3);
        else if (c == "mtu") d.mtuProbe(f.at(1), f.value(2, "auto"));
        else if (c == "ts") d.troubleshoot(f.value(1));
        else if (c == "pingstop") { d.ping(f.at(1), "auto", 50); QTimer::singleShot(2500, &d, [&] { printf("    (stop requested, running=%d)\n", d.running()); d.stop(); }); }
    };
    QObject::connect(&d, &Diagnostics::finished, &d, [&] {
        printf("%s\n    live updates: %d\n    RESULT[%s]: %s\n", qPrintable(d.output().trimmed()), liveChunks, qPrintable(d.resultKind()), qPrintable(d.result())); fflush(stdout);
        QTimer::singleShot(200, &d, next);
    });
    QObject::connect(&d, &Diagnostics::troubleshootFinished, &d, [&] {
        for (const QVariant &s : d.steps()) { const QVariantMap m = s.toMap(); printf("    STEP %-10s %-8s %s\n", qPrintable(m.value("key").toString()), qPrintable(m.value("state").toString()), qPrintable(m.value("detail").toString())); }
        printf("    VERDICT[%s] %s — %s\n", qPrintable(d.verdictKind()), qPrintable(d.verdict()), qPrintable(d.verdictDetail())); fflush(stdout);
        QTimer::singleShot(200, &d, next);
    });
    QTimer::singleShot(100, &d, next);
    QTimer::singleShot(200000, &app, [&] { printf("HARNESS TIMEOUT\n"); app.exit(3); });
    return app.exec();
}
