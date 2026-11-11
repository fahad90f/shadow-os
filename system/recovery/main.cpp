// SPDX-License-Identifier: GPL-3.0-or-later
#include "recovery.h"

#include <nexusapp.h>

#include <QCommandLineOption>
#include <QJsonArray>
#include <QJsonObject>

#include <unistd.h>

int main(int argc, char **argv)
{
    Nexus::App app(argc, argv, {
        QStringLiteral("org.nexus.recovery"),
        QStringLiteral("NEXUS Recovery"),
        QStringLiteral("org.nexus.recovery"),
        QStringLiteral("Repair an installed NEXUS OS that no longer starts or updates"),
        true,
    });
    const QCommandLineOption engine(QStringLiteral("engine"),
        QStringLiteral("Use this recovery engine instead of /usr/sbin/nexus-recovery-engine (development)."),
        QStringLiteral("path"));
    const QCommandLineOption scanFile(QStringLiteral("scan-file"),
        QStringLiteral("TEST MODE: show the installations of this recorded 'scan' result; no engine is run and nothing can be repaired."),
        QStringLiteral("file.json"));
    app.parser().addOption(engine);
    app.parser().addOption(scanFile);
    app.setDumpTopics({QStringLiteral("status"), QStringLiteral("scan")});
    if (!app.processArguments())
        return 0;

    RecoveryOptions &options = RecoveryOptions::instance();
    options.enginePath = RecoveryOptions::defaultEnginePath();
    const QString fromEnvironment = qEnvironmentVariable("NEXUS_RECOVERY_ENGINE");
    if (app.parser().isSet(engine))
        options.enginePath = app.parser().value(engine);
    else if (!fromEnvironment.isEmpty())
        options.enginePath = fromEnvironment;
    options.scanFile = app.parser().value(scanFile);
    options.runningAsRoot = geteuid() == 0;

    if (!app.dumpTopic().isEmpty()) {
        Recovery recovery;
        if (app.dumpTopic() == QLatin1String("status"))
            return app.finishDump(recovery.statusJson());
        if (app.dumpTopic() == QLatin1String("scan")) {
            // The engine's scan needs root: without it (and without an
            // authentication agent, as on a console) this reports why not.
            recovery.rescan();
            if (recovery.scanning())
                app.waitFor(&recovery, SIGNAL(scanFinished()), 120000);
            return app.finishDump(recovery.scanJson());
        }
        return app.dumpError(QStringLiteral("unknown dump topic '%1'").arg(app.dumpTopic()));
    }
    return app.run(QStringLiteral("org.nexus.recovery"));
}
