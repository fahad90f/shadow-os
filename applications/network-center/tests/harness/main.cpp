// SPDX-License-Identifier: GPL-3.0-or-later
// Test harness (not part of the product): drives Nm operations from the command line.
#include "nmclient.h"
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <cstdio>
static QString uuidOf(Nm &nm, const QString &name) {
    for (const QVariant &v : nm.connections()) if (v.toMap().value("name") == name) return v.toMap().value("uuid").toString();
    return name;
}
int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    Nm nm;
    QStringList ops = app.arguments().mid(1);
    std::function<void()> later;
    auto next = [&]() {
        if (ops.isEmpty()) { app.quit(); return; }
        const QStringList f = ops.takeFirst().split('|');
        printf("--> %s\n", qPrintable(f.join(' '))); fflush(stdout);
        const QString c = f.at(0);
        if (c == "up") nm.activateConnection(uuidOf(nm, f.at(1)));
        else if (c == "down") nm.deactivateConnection(uuidOf(nm, f.at(1)));
        else if (c == "del") nm.deleteConnection(uuidOf(nm, f.at(1)));
        else if (c == "auto") nm.setAutoconnect(uuidOf(nm, f.at(1)), f.at(2) == "1");
        else if (c == "devup") nm.connectDevice(f.at(1));
        else if (c == "devdown") nm.disconnectDevice(f.at(1));
        else if (c == "wifi") nm.connectWifi(f.at(1), f.at(2), f.value(3), false);
        else if (c == "radio") nm.setWifiEnabled(f.at(1) == "1");
        else if (c == "secret") nm.activateWithSecret(uuidOf(nm, f.at(1)), f.at(2));
        else if (c == "import") nm.importVpn(f.at(1), f.at(2));
        else if (c == "plan" || c == "ip") {
            const QVariantMap cfg = QJsonDocument::fromJson(f.at(2).toUtf8()).object().toVariantMap();
            const QVariantMap plan = nm.planIpChange(uuidOf(nm, f.at(1)), cfg);
            printf("    plan: %s\n", QJsonDocument(QJsonObject::fromVariantMap(plan)).toJson(QJsonDocument::Compact).constData());
            if (c == "ip") nm.applyIpChange(uuidOf(nm, f.at(1)), cfg, f.value(3) == "1");
            else later();
        } else { printf("    unknown op\n"); later(); }
    };
    later = [&] { QTimer::singleShot(1200, &nm, [&] { next(); }); };
    QObject::connect(&nm, &Nm::operationFinished, &nm, [&](const QString &op, const QString &target, bool ok, const QString &msg) {
        printf("    RESULT %s [%s] %s: %s\n", qPrintable(op), qPrintable(target), ok ? "OK" : "FAILED", qPrintable(msg)); fflush(stdout);
        later();
    });
    QObject::connect(&nm, &Nm::secretsRequired, &nm, [&](const QString &uuid, const QString &name) {
        printf("    SECRETS REQUIRED %s %s\n", qPrintable(uuid), qPrintable(name)); fflush(stdout);
        later();
    });
    bool first = true;
    QObject::connect(&nm, &Nm::refreshed, &nm, [&] { if (first) { first = false; next(); } });
    nm.refresh();
    QTimer::singleShot(240000, &app, [&] { printf("HARNESS TIMEOUT\n"); app.exit(3); });
    return app.exec();
}
