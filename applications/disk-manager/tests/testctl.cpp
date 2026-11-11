// SPDX-License-Identifier: GPL-3.0-or-later
//
// Test driver for the Disk Manager back-end: performs one UDisksClient
// action — exactly the calls the user interface makes — and prints the
// result as one line of JSON. Exit code 0 = the action succeeded,
// 1 = it failed or was refused, 2 = usage.
//
//   shadow-disks-testctl <action> <device> [arguments…]
//     mount|unmount|lock|check|repair|delete|refusal  /dev/X
//     unlock  /dev/X                      (passphrase on stdin)
//     label   /dev/X <label>
//     resize  /dev/X <bytes>
//     format  /dev/X <fstype> [label]
//     table   /dev/X gpt|dos
//     create  /dev/X <offset> <size> [fstype] [label]
//     image   /dev/X <file>     restore /dev/X <file>
//     image-cancel /dev/X <file> <cancel-after-bytes>
//     watch-jobs   /dev/X <seconds>       print every change of the job list
//
// The driver itself refuses to run unless SHADOW_DISKS_TEST_LOOP_ONLY=1 is
// set, which makes the back-end reject every modifying action on anything
// that is not a loop device.
#include "udisksclient.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QTimer>

#include <cstdio>
#include <iostream>

static QString pathOf(const UDisksClient &client, const QString &device)
{
    const QVariantList devices = client.devices();
    for (const QVariant &d : devices) {
        const QVariantMap dm = d.toMap();
        if (dm.value(QStringLiteral("device")).toString() == device)
            return dm.value(QStringLiteral("path")).toString();
        const QVariantList volumes = dm.value(QStringLiteral("volumes")).toList();
        for (const QVariant &v : volumes) {
            const QVariantMap vm = v.toMap();
            if (vm.value(QStringLiteral("device")).toString() == device)
                return vm.value(QStringLiteral("path")).toString();
            // The clear-text side of an unlocked volume is addressed through its container.
            if (vm.value(QStringLiteral("cleartextDevice")).toString() == device)
                return vm.value(QStringLiteral("contentPath")).toString();
        }
    }
    return {};
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments().mid(1);
    if (args.size() < 2) {
        std::fprintf(stderr, "usage: shadow-disks-testctl <action> <device> [arguments...]\n");
        return 2;
    }
    if (qEnvironmentVariableIntValue("SHADOW_DISKS_TEST_LOOP_ONLY") != 1) {
        std::fprintf(stderr, "shadow-disks-testctl: refusing to run without SHADOW_DISKS_TEST_LOOP_ONLY=1\n");
        return 2;
    }
    const QString action = args.at(0);
    const QString device = args.at(1);

    UDisksClient client;
    QEventLoop loop;
    if (!client.loaded()) {
        QObject::connect(&client, &UDisksClient::availabilityChanged, &loop, &QEventLoop::quit);
        QTimer::singleShot(30000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    if (!client.available()) {
        std::fprintf(stderr, "shadow-disks-testctl: UDisks unavailable: %s\n", qPrintable(client.unavailableReason()));
        return 1;
    }
    if (!client.capabilitiesLoaded()) {
        QObject::connect(&client, &UDisksClient::capabilitiesChanged, &loop, &QEventLoop::quit);
        QTimer::singleShot(30000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    const QString path = pathOf(client, device);
    if (path.isEmpty()) {
        std::fprintf(stderr, "shadow-disks-testctl: %s is not known to UDisks\n", qPrintable(device));
        return 1;
    }

    int rc = 1;
    QObject::connect(&client, &UDisksClient::operationFinished, &app, [&](const QString &operation, bool ok, const QString &message) {
        const QJsonObject o{{QStringLiteral("operation"), operation}, {QStringLiteral("ok"), ok}, {QStringLiteral("message"), message}};
        std::printf("%s\n", QJsonDocument(o).toJson(QJsonDocument::Compact).constData());
        std::fflush(stdout);
        rc = ok ? 0 : 1;
        // Let the object tree settle so the next command sees the result.
        QTimer::singleShot(400, &app, &QCoreApplication::quit);
    });

    const auto arg = [&args](int i) { return args.value(i); };
    if (action == QLatin1String("watch-jobs")) {
        // The job list exactly as the user interface receives it (jobsChanged).
        QObject::connect(&client, &UDisksClient::jobsChanged, &app, [&client] {
            const QVariantList jobs = client.jobs();
            for (const QVariant &j : jobs) {
                const QVariantMap m = j.toMap();
                std::printf("job %s [%s] on %s progressValid=%d progress=%.2f cancelable=%d\n",
                            qPrintable(m.value(QStringLiteral("operation")).toString()), qPrintable(m.value(QStringLiteral("label")).toString()),
                            qPrintable(m.value(QStringLiteral("targets")).toStringList().join(QLatin1Char(','))),
                            int(m.value(QStringLiteral("progressValid")).toBool()), m.value(QStringLiteral("progress")).toDouble(),
                            int(m.value(QStringLiteral("cancelable")).toBool()));
            }
            if (jobs.isEmpty())
                std::printf("no jobs\n");
            std::fflush(stdout);
        });
        QTimer::singleShot(int(arg(2).toDouble() * 1000), &app, &QCoreApplication::quit);
        app.exec();
        return 0;
    }
    if (action == QLatin1String("refusal")) {
        const QString refusal = client.destructiveRefusal(path);
        std::printf("%s\n", QJsonDocument(QJsonObject{{QStringLiteral("refusal"), refusal}}).toJson(QJsonDocument::Compact).constData());
        return refusal.isEmpty() ? 0 : 1;
    } else if (action == QLatin1String("mount")) {
        client.mount(path);
    } else if (action == QLatin1String("unmount")) {
        client.unmount(path);
    } else if (action == QLatin1String("unlock")) {
        std::string passphrase;
        std::getline(std::cin, passphrase);
        client.unlock(path, QString::fromStdString(passphrase));
    } else if (action == QLatin1String("lock")) {
        client.lock(path);
    } else if (action == QLatin1String("check")) {
        client.check(path);
    } else if (action == QLatin1String("repair")) {
        client.repair(path);
    } else if (action == QLatin1String("delete")) {
        client.deletePartition(path);
    } else if (action == QLatin1String("label")) {
        client.setLabel(path, arg(2));
    } else if (action == QLatin1String("resize")) {
        client.resize(path, arg(2).toDouble());
    } else if (action == QLatin1String("format")) {
        client.format(path, arg(2), arg(3));
    } else if (action == QLatin1String("table")) {
        client.createPartitionTable(path, arg(2));
    } else if (action == QLatin1String("create")) {
        client.createPartition(path, arg(2).toDouble(), arg(3).toDouble(), arg(4), arg(5));
    } else if (action == QLatin1String("image")) {
        client.createImage(path, arg(2));
    } else if (action == QLatin1String("restore")) {
        client.restoreImage(path, arg(2));
    } else if (action == QLatin1String("image-cancel")) {
        client.createImage(path, arg(2));
        auto *poll = new QTimer(&app);
        const double threshold = arg(3).toDouble();
        QObject::connect(poll, &QTimer::timeout, &app, [&client, threshold] {
            const QVariantMap job = client.imageJob();
            if (job.value(QStringLiteral("active")).toBool() && job.value(QStringLiteral("done")).toDouble() >= threshold) {
                std::printf("{\"progressAtCancel\":%.0f,\"total\":%.0f}\n", job.value(QStringLiteral("done")).toDouble(),
                            job.value(QStringLiteral("total")).toDouble());
                client.cancelImageJob();
            }
        });
        poll->start(5);
    } else {
        std::fprintf(stderr, "shadow-disks-testctl: unknown action '%s'\n", qPrintable(action));
        return 2;
    }
    QTimer::singleShot(600000, &app, [&] { std::fprintf(stderr, "shadow-disks-testctl: timeout\n"); app.exit(1); });
    app.exec();
    return rc;
}
