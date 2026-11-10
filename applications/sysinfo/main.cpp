// SPDX-License-Identifier: GPL-3.0-or-later
#include "systeminfo.h"

#include <nexusapp.h>

#include <QJsonArray>
#include <QJsonObject>

int main(int argc, char **argv)
{
    Nexus::App app(argc, argv, {
        QStringLiteral("org.nexus.sysinfo"),
        QStringLiteral("System Information"),
        QStringLiteral("org.nexus.sysinfo"),
        QStringLiteral("Hardware and software summary of this computer"),
    });
    app.setDumpTopics({QStringLiteral("system"), QStringLiteral("report")});
    if (!app.processArguments())
        return 0;

    if (!app.dumpTopic().isEmpty()) {
        const SystemInfo info;
        if (app.dumpTopic() == QLatin1String("report"))
            return app.finishDump(info.report());
        if (app.dumpTopic() != QLatin1String("system"))
            return app.dumpError(QStringLiteral("unknown dump topic '%1'").arg(app.dumpTopic()));
        return app.finishDump(QJsonObject{
            {QStringLiteral("osName"), info.osName()},
            {QStringLiteral("osVersion"), info.osVersion()},
            {QStringLiteral("osCodename"), info.osCodename()},
            {QStringLiteral("osBase"), info.osBase()},
            {QStringLiteral("kernel"), info.kernel()},
            {QStringLiteral("architecture"), info.architecture()},
            {QStringLiteral("hostname"), info.hostname()},
            {QStringLiteral("cpuModel"), info.cpuModel()},
            {QStringLiteral("cpuCores"), info.cpuCores()},
            {QStringLiteral("cpuThreads"), info.cpuThreads()},
            {QStringLiteral("memoryTotal"), info.memoryTotal()},
            {QStringLiteral("swapTotal"), info.swapTotal()},
            {QStringLiteral("gpus"), QJsonArray::fromStringList(info.gpus())},
            {QStringLiteral("systemVendor"), info.systemVendor()},
            {QStringLiteral("systemProduct"), info.systemProduct()},
            {QStringLiteral("firmwareType"), info.firmwareType()},
            {QStringLiteral("firmwareVersion"), info.firmwareVersion()},
            {QStringLiteral("secureBoot"), info.secureBoot()},
            {QStringLiteral("virtualization"), info.virtualization()},
            {QStringLiteral("uptimeSeconds"), info.uptimeSeconds()},
            {QStringLiteral("filesystems"), QJsonArray::fromVariantList(info.filesystems())},
        });
    }
    return app.run(QStringLiteral("org.nexus.sysinfo"));
}
