// SPDX-License-Identifier: GPL-3.0-or-later
#include "arsenal.h"

#include <shadowapp.h>

#include <QCommandLineOption>

namespace {
const char kDefaultProfilesDir[] = "/usr/share/shadow/arsenal/profiles";
}

int main(int argc, char **argv)
{
    Shadow::App app(argc, argv, {
        QStringLiteral("org.shadow.arsenal"),
        QStringLiteral("SHADOW Arsenal"),
        QStringLiteral("org.shadow.arsenal"),
        QStringLiteral("Install and manage security tool profiles for authorised testing"),
        true,
    });
    const QCommandLineOption profilesDirOption(
        QStringLiteral("profiles-dir"),
        QStringLiteral("Read the profile manifests from <dir> instead of %1.").arg(QLatin1String(kDefaultProfilesDir)),
        QStringLiteral("dir"), QLatin1String(kDefaultProfilesDir));
    const QCommandLineOption pageOption(
        QStringLiteral("page"),
        QStringLiteral("Open <page>: overview, tools, external, responsible-use or a profile id."),
        QStringLiteral("page"));
    app.parser().addOptions({profilesDirOption, pageOption});
    app.setDumpTopics({QStringLiteral("profiles"), QStringLiteral("tools"), QStringLiteral("state")});
    if (!app.processArguments())
        return 0;

    // Lives until the process ends: QML bindings may still be evaluated while
    // the engine is torn down, after anything declared here would be gone.
    auto &controller = *new Arsenal::Controller(app.parser().value(profilesDirOption));

    if (!app.dumpTopic().isEmpty()) {
        const QString topic = app.dumpTopic();
        if (topic != QLatin1String("profiles") && topic != QLatin1String("tools") && topic != QLatin1String("state"))
            return app.dumpError(QStringLiteral("unknown dump topic '%1'").arg(topic));
        if (!controller.loadError().isEmpty())
            return app.dumpError(controller.loadError());
        controller.setEstimatesEnabled(topic == QLatin1String("profiles"));
        controller.refresh();
        app.waitFor(controller.db(), SIGNAL(refreshed()), 90000);
        if (topic == QLatin1String("tools"))
            return app.finishDump(controller.dumpTools());
        if (topic == QLatin1String("profiles")) {
            if (controller.estimatesPending())
                app.waitFor(&controller, SIGNAL(estimatesFinished()), 300000);
            return app.finishDump(controller.dumpProfiles());
        }
        controller.packageKit()->probe();
        app.waitFor(controller.packageKit(), SIGNAL(probed()), 35000);
        return app.finishDump(controller.dumpState());
    }

    app.expose(QStringLiteral("arsenal"), &controller);
    controller.setInitialPage(app.parser().value(pageOption));
    controller.packageKit()->probe();
    controller.refresh();
    return app.run(QStringLiteral("org.shadow.arsenal"));
}
