// SPDX-License-Identifier: GPL-3.0-or-later
#include "processreader.h"
#include "servicemodel.h"
#include "storage.h"
#include "systemsampler.h"

#include <shadowapp.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QThread>

#include <cstdio>
#include <cstring>
#include <vector>

using namespace Qt::StringLiterals;

namespace {

const char *const kPages[] = {"overview", "processes", "services", "disks", "network", "sensors"};

bool isPage(const QString &key)
{
    for (const char *page : kPages) {
        if (key == QLatin1StringView(page))
            return true;
    }
    return false;
}

QString pageList()
{
    QStringList names;
    for (const char *page : kPages)
        names << QString::fromLatin1(page);
    return names.join(u", "_s);
}

// "--page <key>" and "--page=<key>" are rewritten into the positional form
// "<key>". Shadow::App hands only positional arguments over to an already
// running instance, so this is what lets "shadow-monitor --page processes"
// (the Task Manager launcher) switch the page of the open window.
std::vector<char *> promotePageOption(int argc, char **argv)
{
    std::vector<char *> args;
    for (int i = 0; i < argc; ++i) {
        if (i > 0 && std::strcmp(argv[i], "--page") == 0 && i + 1 < argc) {
            args.push_back(argv[++i]);
        } else if (i > 0 && std::strncmp(argv[i], "--page=", 7) == 0) {
            args.push_back(argv[i] + 7);
        } else {
            args.push_back(argv[i]);
        }
    }
    args.push_back(nullptr);
    return args;
}

// Delta-based figures (CPU utilisation, disk and network rates, per-process
// CPU share) are real only between two readings, so those topics read twice,
// half a second apart.
const unsigned long kDumpIntervalMs = 500;

int dump(Shadow::App &app)
{
    const QString topic = app.dumpTopic();

    if (topic == "processes"_L1) {
        ProcessReader reader;
        reader.scan();
        QThread::msleep(kDumpIntervalMs);
        const QList<ProcessRecord> processes = reader.scan();
        QJsonArray list;
        for (const ProcessRecord &process : processes)
            list.append(QJsonObject::fromVariantMap(process.toVariant()));
        return app.finishDump(QJsonObject{
            {u"count"_s, list.size()},
            {u"intervalMs"_s, qint64(kDumpIntervalMs)},
            {u"processes"_s, list},
        });
    }

    if (topic == "services"_L1) {
        ServiceModel services;
        if (services.available()) {
            services.refresh();
            if (!app.waitFor(&services, SIGNAL(refreshed()), 45000))
                return app.dumpError(u"timed out waiting for systemd"_s);
        }
        return app.finishDump(services.toJson());
    }

    const bool twoReadings = topic == "cpu"_L1 || topic == "disks"_L1 || topic == "network"_L1;
    const bool oneReading = topic == "memory"_L1 || topic == "gpu"_L1 || topic == "sensors"_L1 || topic == "battery"_L1;
    if (!twoReadings && !oneReading)
        return app.dumpError(u"unknown dump topic '%1'"_s.arg(topic));

    SystemSampler sampler;
    SystemSnapshot snapshot = sampler.sample();
    if (twoReadings) {
        QThread::msleep(kDumpIntervalMs);
        snapshot = sampler.sample();
    }
    if (topic == "cpu"_L1)
        return app.finishDump(snapshot.cpuJson());
    if (topic == "memory"_L1)
        return app.finishDump(snapshot.memoryJson());
    if (topic == "gpu"_L1)
        return app.finishDump(snapshot.gpuJson());
    if (topic == "disks"_L1)
        return app.finishDump(snapshot.disksJson(FilesystemSample::read()));
    if (topic == "network"_L1)
        return app.finishDump(snapshot.networkJson());
    if (topic == "sensors"_L1)
        return app.finishDump(snapshot.sensorsJson());
    return app.finishDump(snapshot.batteryJson());
}

} // namespace

int main(int argc, char **argv)
{
    std::vector<char *> args = promotePageOption(argc, argv);
    int count = int(args.size()) - 1;

    Shadow::AppInfo info;
    info.id = QStringLiteral("org.shadow.monitor");
    info.displayName = QStringLiteral("System Monitor");
    info.iconName = QStringLiteral("org.shadow.monitor");
    info.description = QStringLiteral("Monitor processor, memory, graphics, disk, network, sensors, processes "
                                      "and services of this computer.");
    info.singleInstance = true;

    Shadow::App app(count, args.data(), info);
    const QString pages = pageList();
    app.parser().addPositionalArgument(u"page"_s, u"Page to open: %1."_s.arg(pages), u"[page]"_s);
    app.parser().addOption(QCommandLineOption(u"page"_s, u"Open <page> (same as the positional argument)."_s, u"page"_s));
    app.setDumpTopics({u"cpu"_s, u"memory"_s, u"gpu"_s, u"disks"_s, u"network"_s, u"processes"_s,
                       u"services"_s, u"sensors"_s, u"battery"_s});

    // Checked before a running instance could be handed a page it does not have.
    for (int i = 1; i < count; ++i) {
        const QString argument = QString::fromLocal8Bit(args[size_t(i)]);
        if (argument == "--dump"_L1) {
            ++i;                    // its value is a topic, not a page
        } else if (!argument.startsWith(u'-') && !isPage(argument)) {
            std::fprintf(stderr, "shadow-monitor: unknown page '%s' (pages: %s)\n",
                         qPrintable(argument), qPrintable(pages));
            return 2;
        }
    }

    if (!app.processArguments())
        return 0;
    if (!app.dumpTopic().isEmpty())
        return dump(app);
    return app.run(u"org.shadow.monitor"_s);
}
