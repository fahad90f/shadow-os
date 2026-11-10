// SPDX-License-Identifier: GPL-3.0-or-later
//
// horizon-shell — the NEXUS HORIZON desktop shell: wallpaper, panel, dock,
// launcher, session menu and authentication dialogs, as layer-shell surfaces
// on the Wayland compositor.
#include "applications.h"
#include "effects.h"
#include "polkitagent.h"
#include "session.h"
#include "shellsettings.h"
#include "shortcuts.h"
#include "tasks.h"
#include "workspaces.h"

#include <nexusapp.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>

int main(int argc, char **argv)
{
    Nexus::AppInfo info;
    info.id = QStringLiteral("org.nexus.horizon.shell");
    info.displayName = QStringLiteral("NEXUS HORIZON");
    info.iconName = QStringLiteral("nexus-os");
    info.description = QStringLiteral("The NEXUS HORIZON desktop shell. Started by the session; not meant to be run by hand.");

    Nexus::App app(argc, argv, info);
    QGuiApplication::setQuitOnLastWindowClosed(false);
    app.setDumpTopics({QStringLiteral("apps"), QStringLiteral("settings"), QStringLiteral("workspaces"),
                       QStringLiteral("session"), QStringLiteral("shortcuts")});
    if (!app.processArguments())
        return 0;

    ShellSettings settings;
    Applications applications;
    Session session;
    Workspaces workspaces;

    if (!app.dumpTopic().isEmpty()) {
        const QString topic = app.dumpTopic();
        if (topic == QLatin1String("settings"))
            return app.finishDump(QJsonObject::fromVariantMap(settings.toMap()));
        if (topic == QLatin1String("apps")) {
            QJsonArray apps;
            for (int row = 0; row < applications.rowCount(); ++row) {
                const QModelIndex i = applications.index(row);
                apps.append(QJsonObject{
                    {QStringLiteral("desktopId"), i.data(Applications::DesktopIdRole).toString()},
                    {QStringLiteral("name"), i.data(Applications::NameRole).toString()},
                    {QStringLiteral("icon"), i.data(Applications::IconRole).toString()},
                    {QStringLiteral("group"), i.data(Applications::GroupRole).toString()},
                });
            }
            return app.finishDump(QJsonObject{{QStringLiteral("groups"), QJsonArray::fromStringList(applications.groups())},
                                              {QStringLiteral("applications"), apps}});
        }
        if (topic == QLatin1String("workspaces")) {
            app.waitFor(&workspaces, SIGNAL(listChanged()), 3000);
            return app.finishDump(QJsonObject{{QStringLiteral("available"), workspaces.available()},
                                              {QStringLiteral("current"), workspaces.current()},
                                              {QStringLiteral("list"), QJsonArray::fromVariantList(workspaces.list())}});
        }
        if (topic == QLatin1String("session")) {
            app.waitFor(&session, SIGNAL(capabilitiesChanged()), 1500);
            return app.finishDump(QJsonObject{
                {QStringLiteral("userName"), session.userName()}, {QStringLiteral("fullName"), session.fullName()},
                {QStringLiteral("hostName"), session.hostName()}, {QStringLiteral("liveSession"), session.liveSession()},
                {QStringLiteral("canSuspend"), session.canSuspend()}, {QStringLiteral("canHibernate"), session.canHibernate()},
                {QStringLiteral("canReboot"), session.canReboot()}, {QStringLiteral("canPowerOff"), session.canPowerOff()},
                {QStringLiteral("canLock"), session.canLock()},
            });
        }
        if (topic == QLatin1String("shortcuts")) {
            const Shortcuts shortcuts;
            return app.finishDump(QJsonObject{{QStringLiteral("available"), shortcuts.available()},
                                              {QStringLiteral("shortcuts"), QJsonArray::fromVariantList(shortcuts.list())}});
        }
        return app.dumpError(QStringLiteral("unknown dump topic '%1'").arg(topic));
    }

    WindowModel windows(&applications);
    DockModel dock(&applications, &windows, &settings);
    Shortcuts shortcuts;
    PolkitAgent polkitAgent;
    ShellEffects effects;

    app.expose(QStringLiteral("shellSettings"), &settings);
    app.expose(QStringLiteral("applications"), &applications);
    app.expose(QStringLiteral("windows"), &windows);
    app.expose(QStringLiteral("dock"), &dock);
    app.expose(QStringLiteral("workspaces"), &workspaces);
    app.expose(QStringLiteral("session"), &session);
    app.expose(QStringLiteral("shortcuts"), &shortcuts);
    app.expose(QStringLiteral("polkitAgent"), &polkitAgent);
    app.expose(QStringLiteral("effects"), &effects);

    // Registering needs the event loop (and fails harmlessly outside a login
    // session, e.g. in the headless smoke test).
    QTimer::singleShot(0, &polkitAgent, [&polkitAgent] {
        if (!polkitAgent.registerAgent())
            qWarning("horizon-shell: could not register as the polkit authentication agent "
                     "(no login session, or another agent is running)");
    });

    return app.run(QStringLiteral("org.nexus.horizon"), QStringLiteral("Shell"));
}
