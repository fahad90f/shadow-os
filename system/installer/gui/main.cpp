// SPDX-License-Identifier: GPL-3.0-or-later
#include "installer.h"
#include "keyboard.h"
#include "locales.h"
#include "options.h"
#include "timezones.h"
#include "validation.h"

#include <nexusapp.h>

#include <QCommandLineOption>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

#include <cstdio>
#include <unistd.h>

Options &Options::instance()
{
    static Options options;
    return options;
}

namespace {
const QStringList kSteps = {
    QStringLiteral("welcome"), QStringLiteral("keyboard"), QStringLiteral("check"), QStringLiteral("timezone"),
    QStringLiteral("disk"), QStringLiteral("user"), QStringLiteral("software"), QStringLiteral("summary"),
    QStringLiteral("progress"), QStringLiteral("result"),
};
}

int main(int argc, char **argv)
{
    Nexus::App app(argc, argv, {
        QStringLiteral("org.nexus.installer"),
        QStringLiteral("NEXUS OS Installer"),
        QStringLiteral("org.nexus.installer"),
        QStringLiteral("Install NEXUS OS on this computer"),
        true,
    });

    const QCommandLineOption engineOption(QStringLiteral("engine"),
        QStringLiteral("Installation engine to run instead of %1 (also $NEXUS_INSTALLER_ENGINE).").arg(Options::defaultEnginePath()),
        QStringLiteral("path"));
    const QCommandLineOption probeOption(QStringLiteral("probe-file"),
        QStringLiteral("TEST MODE: take the hardware description from this JSON file instead of the engine's "
                       "probe. Nothing can be installed in this mode."), QStringLiteral("json"));
    const QCommandLineOption stepOption(QStringLiteral("step"),
        QStringLiteral("Test mode only: open the wizard at this step (%1).").arg(kSteps.join(QStringLiteral(", "))),
        QStringLiteral("name"));
    const QCommandLineOption presetOption(QStringLiteral("preset"),
        QStringLiteral("Test mode only: pre-fill the wizard from an answer file."), QStringLiteral("json"));
    const QCommandLineOption simulateOption(QStringLiteral("simulate"),
        QStringLiteral("Test mode only, with --engine <test-engine>: let the test engine play the event stream "
                       "of an installation (scenario: success, fail-copy, plan-changed, …). No disk is touched."),
        QStringLiteral("scenario"));
    const QCommandLineOption profilesOption(QStringLiteral("profiles-dir"),
        QStringLiteral("Directory of tool profile manifests (default %1).").arg(Options::defaultProfilesDir()),
        QStringLiteral("dir"));
    const QCommandLineOption allowOption(QStringLiteral("allow-installed-system"),
        QStringLiteral("Permit the installation although this is not a live session (%1 is missing).")
            .arg(Options::liveSessionMarker()));
    app.parser().addOptions({engineOption, probeOption, stepOption, presetOption, simulateOption, profilesOption, allowOption});
    app.setDumpTopics({QStringLiteral("environment"), QStringLiteral("locales"), QStringLiteral("keyboard"),
                       QStringLiteral("timezones"), QStringLiteral("profiles"), QStringLiteral("probe"),
                       QStringLiteral("rules")});
    if (!app.processArguments())
        return 0;

    Options &o = Options::instance();
    const QCommandLineParser &parser = app.parser();
    o.enginePath = Options::defaultEnginePath();
    const QString envEngine = qEnvironmentVariable("NEXUS_INSTALLER_ENGINE");
    if (parser.isSet(engineOption) || !envEngine.isEmpty()) {
        o.enginePath = QFileInfo(parser.isSet(engineOption) ? parser.value(engineOption) : envEngine).absoluteFilePath();
        o.customEngine = true;
    }
    if (parser.isSet(probeOption))
        o.probeFile = QFileInfo(parser.value(probeOption)).absoluteFilePath();
    o.profilesDir = parser.isSet(profilesOption) ? parser.value(profilesOption) : Options::defaultProfilesDir();
    o.allowInstalledSystem = parser.isSet(allowOption);
    o.liveSession = QFileInfo::exists(Options::liveSessionMarker());
    o.runningAsRoot = geteuid() == 0;

    if (parser.isSet(stepOption) && !kSteps.contains(parser.value(stepOption))) {
        std::fprintf(stderr, "nexus-installer: unknown step '%s' (steps: %s)\n",
                     qPrintable(parser.value(stepOption)), qPrintable(kSteps.join(QLatin1Char(' '))));
        return 2;
    }
    if (o.testMode()) {
        o.startStep = parser.value(stepOption);
        o.presetFile = parser.value(presetOption);
        o.simulateScenario = parser.value(simulateOption);
        if (!o.simulateScenario.isEmpty() && !o.customEngine) {
            std::fprintf(stderr, "nexus-installer: --simulate needs --engine <test-engine>\n");
            return 2;
        }
    } else if (parser.isSet(stepOption) || parser.isSet(presetOption) || parser.isSet(simulateOption)) {
        std::fprintf(stderr, "nexus-installer: --step, --preset and --simulate are test options and need --probe-file\n");
        return 2;
    }

    if (!app.dumpTopic().isEmpty()) {
        const QString topic = app.dumpTopic();
        if (topic == QLatin1String("locales"))
            return app.finishDump(Locales().toJson());
        if (topic == QLatin1String("keyboard")) {
            Keyboard keyboard;
            app.waitFor(&keyboard, SIGNAL(sessionChanged()), 1500);
            return app.finishDump(keyboard.toJson());
        }
        if (topic == QLatin1String("timezones"))
            return app.finishDump(TimeZones().toJson());
        if (topic == QLatin1String("rules")) {
            // Verdicts of the mirrored input rules (validation.h) for the strings in
            // $NEXUS_INSTALLER_RULES_INPUT; testing/check-rules.py compares them with
            // the engine's own code.
            QFile input(qEnvironmentVariable("NEXUS_INSTALLER_RULES_INPUT"));
            if (!input.open(QIODevice::ReadOnly))
                return app.dumpError(QStringLiteral("set NEXUS_INSTALLER_RULES_INPUT to a JSON file "
                                                    "{usernames, hostnames, fullnames, passphrases, passwords}"));
            const QJsonObject in = QJsonDocument::fromJson(input.readAll()).object();
            const auto verdicts = [&in](const char *key, QString (*problem)(const QString &)) {
                QJsonObject out;
                const QJsonArray values = in.value(QLatin1String(key)).toArray();
                for (const QJsonValue &value : values)
                    out.insert(value.toString(), problem(value.toString()));
                return out;
            };
            return app.finishDump(QJsonObject{
                {QStringLiteral("usernames"), verdicts("usernames", Validation::userNameProblem)},
                {QStringLiteral("hostnames"), verdicts("hostnames", Validation::hostNameProblem)},
                {QStringLiteral("fullnames"), verdicts("fullnames", Validation::fullNameProblem)},
                {QStringLiteral("passphrases"), verdicts("passphrases", Validation::passphraseProblem)},
                {QStringLiteral("passwords"), verdicts("passwords", Validation::passwordProblem)},
            });
        }
        Installer installer;
        if (topic == QLatin1String("environment"))
            return app.finishDump(installer.environmentJson());
        if (topic == QLatin1String("profiles"))
            return app.finishDump(installer.profilesJson());
        if (topic == QLatin1String("probe")) {
            installer.runProbe(false);
            while (installer.probeState() == QLatin1String("running")) {
                if (!app.waitFor(&installer, SIGNAL(probeChanged()), 150000))
                    return app.dumpError(QStringLiteral("the engine did not answer"));
            }
            if (installer.probeState() != QLatin1String("ready"))
                return app.dumpError(installer.probeError() + QLatin1Char('\n') + installer.probeErrorDetail());
            return app.finishDump(QJsonObject::fromVariantMap(installer.probe()));
        }
        return app.dumpError(QStringLiteral("unknown dump topic '%1'").arg(topic));
    }
    return app.run(QStringLiteral("org.nexus.installer"));
}
