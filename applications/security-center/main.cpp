// SPDX-License-Identifier: GPL-3.0-or-later
#include "securitycenter.h"

#include <shadowapp.h>

#include <QCommandLineOption>
#include <QJsonObject>

#include <cstring>
#include <vector>

namespace {
const QStringList kPages{QStringLiteral("overview"), QStringLiteral("firewall"), QStringLiteral("apparmor"),
                         QStringLiteral("encryption"), QStringLiteral("secureboot"), QStringLiteral("updates"),
                         QStringLiteral("network"), QStringLiteral("services"), QStringLiteral("logins"),
                         QStringLiteral("hardening")};
}

int main(int argc, char **argv)
{
    // "--page NAME" is handed on as the positional argument "NAME": that is
    // what a second launch passes to the already running instance.
    std::vector<char *> args;
    for (int i = 0; i < argc; ++i) {
        if (i > 0 && std::strcmp(argv[i], "--page") == 0 && i + 1 < argc)
            continue;
        if (i > 0 && std::strncmp(argv[i], "--page=", 7) == 0) {
            args.push_back(argv[i] + 7);
            continue;
        }
        args.push_back(argv[i]);
    }
    int count = int(args.size());
    args.push_back(nullptr);

    Shadow::AppInfo info;
    info.id = QStringLiteral("org.shadow.securitycenter");
    info.displayName = QStringLiteral("SHADOW Security Center");
    info.iconName = QStringLiteral("org.shadow.securitycenter");
    info.description = QStringLiteral("The security state of this computer and its firewall.");
    info.singleInstance = true;

    Shadow::App app(count, args.data(), info);
    const QCommandLineOption toolsDir(
        QStringLiteral("tools-dir"),
        QStringLiteral("Developer override: run shadow-security-status and shadow-firewall directly (without pkexec) "
                       "from <dir> (flat, or the repository layout <dir>/audit and <dir>/firewall). "
                       "Environment: SHADOW_SECURITY_TOOLS_DIR, SHADOW_SECURITY_STATUS_TOOL, SHADOW_FIREWALL_TOOL."),
        QStringLiteral("dir"));
    const QCommandLineOption pageOption(
        QStringLiteral("page"), QStringLiteral("Page to open (same as the positional argument)."), QStringLiteral("name"));
    app.parser().addOption(toolsDir);
    app.parser().addOption(pageOption);
    app.parser().addPositionalArgument(QStringLiteral("page"),
        QStringLiteral("Page to open: %1.").arg(kPages.join(QStringLiteral(", "))), QStringLiteral("[page]"));

    QStringList topics = SecurityCenter::sectionNames();
    topics << QStringLiteral("report") << QStringLiteral("report-text") << QStringLiteral("tools");
    app.setDumpTopics(topics);
    if (!app.processArguments())
        return 0;

    const QStringList positional = app.parser().positionalArguments();
    SecurityCenter::configure(app.parser().value(toolsDir), positional.isEmpty() ? QString() : positional.first());

    if (!app.dumpTopic().isEmpty()) {
        const QString topic = app.dumpTopic();
        if (!topics.contains(topic))
            return app.dumpError(QStringLiteral("unknown dump topic '%1'").arg(topic));
        if (topic == QLatin1String("tools"))
            return app.finishDump(SecurityCenter::toolsJson());

        // The same path the UI takes: the status tool, through pkexec unless
        // the developer override is active.
        SecurityCenter center;
        const bool whole = topic == QLatin1String("report") || topic == QLatin1String("report-text");
        if (whole)
            center.refresh();
        else
            center.refreshSections({topic});
        if (center.loading())
            app.waitFor(&center, SIGNAL(refreshFinished(bool)), 7 * 60 * 1000);
        if (!center.error().isEmpty() || !center.hasReport())
            return app.dumpError(center.error().isEmpty() ? QStringLiteral("the status tool did not finish") : center.error());
        if (topic == QLatin1String("report-text"))
            return app.finishDump(center.reportAsText());
        if (whole) {
            QJsonObject out = center.reportJson();
            out.insert(QStringLiteral("generated"), center.generated());
            out.insert(QStringLiteral("privileged"), center.privileged());
            return app.finishDump(out);
        }
        return app.finishDump(center.reportJson().value(topic));
    }
    return app.run(QStringLiteral("org.shadow.securitycenter"));
}
