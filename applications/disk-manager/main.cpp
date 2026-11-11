// SPDX-License-Identifier: GPL-3.0-or-later
#include "udisksclient.h"

#include <nexusapp.h>

#include <QJsonArray>
#include <QJsonObject>

int main(int argc, char **argv)
{
    Nexus::App app(argc, argv, {
        QStringLiteral("org.nexus.diskmanager"),
        QStringLiteral("Disk Manager"),
        QStringLiteral("org.nexus.diskmanager"),
        QStringLiteral("Drives, partitions, file systems and disk images"),
        true,
    });
    app.setDumpTopics({QStringLiteral("status"), QStringLiteral("devices"), QStringLiteral("jobs"),
                       QStringLiteral("capabilities"), QStringLiteral("protected")});
    if (!app.processArguments())
        return 0;

    // Belongs to the application object so that it outlives the QML engine.
    auto *client = new UDisksClient(qApp);

    if (!app.dumpTopic().isEmpty()) {
        const QString topic = app.dumpTopic();
        if (topic == QLatin1String("protected"))
            return app.finishDump(client->protectedDisksJson());
        if (!client->loaded())
            app.waitFor(client, SIGNAL(availabilityChanged()), 35000);
        if (topic == QLatin1String("status")) {
            return app.finishDump(QJsonObject{
                {QStringLiteral("available"), client->available()},
                {QStringLiteral("unavailableReason"), client->unavailableReason()},
                {QStringLiteral("udisksVersion"), client->version()},
                {QStringLiteral("deviceCount"), int(client->devices().size())},
            });
        }
        if (topic == QLatin1String("devices"))
            return app.finishDump(QJsonArray::fromVariantList(client->devices()));
        if (topic == QLatin1String("jobs"))
            return app.finishDump(QJsonArray::fromVariantList(client->jobs()));
        if (topic == QLatin1String("capabilities")) {
            if (client->available() && !client->capabilitiesLoaded())
                app.waitFor(client, SIGNAL(capabilitiesChanged()), 35000);
            return app.finishDump(QJsonObject{
                {QStringLiteral("available"), client->available()},
                {QStringLiteral("formatTypes"), QJsonArray::fromStringList(client->formatTypes())},
                {QStringLiteral("fileSystems"), QJsonObject::fromVariantMap(client->capabilities())},
            });
        }
        return app.dumpError(QStringLiteral("unknown dump topic '%1'").arg(topic));
    }

    app.expose(QStringLiteral("disks"), client);
    return app.run(QStringLiteral("org.nexus.diskmanager"));
}
