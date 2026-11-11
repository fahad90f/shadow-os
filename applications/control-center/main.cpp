// SPDX-License-Identifier: GPL-3.0-or-later
#include "dumpregistry.h"

#include <shadowapp.h>

#include <QDir>
#include <QQmlApplicationEngine>
#include <QQmlContext>

int main(int argc, char **argv)
{
    Shadow::AppInfo info;
    info.id = QStringLiteral("org.shadow.controlcenter");
    info.displayName = QStringLiteral("Control Center");
    info.iconName = QStringLiteral("org.shadow.controlcenter");
    info.description = QStringLiteral("SHADOW system settings. Pass a page key (e.g. \"wifi\", \"display\") to open it directly.");
    info.singleInstance = true;

    Shadow::App app(argc, argv, info);
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
    // The page files compiled into this build (see the page table in Main.qml).
    const QStringList availablePages = QDir(QStringLiteral(":/qt/qml/org/shadow/controlcenter/pages"))
                                           .entryList({QStringLiteral("*.qml")}, QDir::Files);
    app.engine().rootContext()->setContextProperty(QStringLiteral("availablePages"), availablePages);
    return app.run(QStringLiteral("org.shadow.controlcenter"));
}
