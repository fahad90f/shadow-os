// SPDX-License-Identifier: GPL-3.0-or-later
#include "dumpregistry.h"

#include <nexusapp.h>

int main(int argc, char **argv)
{
    Nexus::AppInfo info;
    info.id = QStringLiteral("org.nexus.controlcenter");
    info.displayName = QStringLiteral("Control Center");
    info.iconName = QStringLiteral("org.nexus.controlcenter");
    info.description = QStringLiteral("NEXUS system settings. Pass a page key (e.g. \"wifi\", \"display\") to open it directly.");
    info.singleInstance = true;

    Nexus::App app(argc, argv, info);
    app.parser().addPositionalArgument(QStringLiteral("page"), QStringLiteral("Settings page to open."), QStringLiteral("[page]"));
    app.setDumpTopics(ControlCenter::dumpTopics());
    if (!app.processArguments())
        return 0;

    if (!app.dumpTopic().isEmpty()) {
        QJsonValue value;
        if (!ControlCenter::runDump(app.dumpTopic(), &value))
            return app.dumpError(QStringLiteral("unknown dump topic '%1'").arg(app.dumpTopic()));
        return app.finishDump(value);
    }
    return app.run(QStringLiteral("org.nexus.controlcenter"));
}
