// SPDX-License-Identifier: GPL-3.0-or-later
//
// The "tray" shell module: the system tray — a StatusNotifierItem host (and
// the session's StatusNotifierWatcher when there is no other) with the
// items' com.canonical.dbusmenu menus. See README.md.
#include "module.h"
#include "trayhost.h"
#include "trayicons.h"
#include "trayitem.h"
#include "traymenu.h"
#include "trayutil.h"

#include <nexusapp.h>

#include <QEventLoop>
#include <QIcon>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>

static void init(Horizon::ShellContext &ctx)
{
    auto *host = new TrayHost(ctx.parent());
    ctx.app->engine().addImageProvider(TrayImageProvider::scheme(), new TrayImageProvider(host));
    ctx.expose(QStringLiteral("tray"), host);
    ctx.addPanelItem(QStringLiteral("TrayArea.qml"), 10);
    ctx.addSurface(QStringLiteral("TrayMenuOverlay.qml"));
    ctx.addSurface(QStringLiteral("TrayToolTip.qml"));
    // Asynchronous: nothing here waits for the bus.
    host->start(TrayHost::Shell);
}
HORIZON_SHELL_MODULE(tray, init)

// horizon-shell --dump tray
//
// Runs next to the shell, as an observer: it asks the session's
// StatusNotifierWatcher (the running shell, normally) for its items and
// reads each item and its menu with the same code the tray uses — without
// registering anything and without telling an application that its menu is
// being shown.
static QJsonObject dumpTray()
{
    TrayHost host;
    QEventLoop loop;
    const auto wait = [&loop](int ms, const std::function<bool()> &done) {
        QTimer deadline, poll;
        deadline.setSingleShot(true);
        QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        QObject::connect(&poll, &QTimer::timeout, &loop, [&loop, &done] { if (done()) loop.quit(); });
        deadline.start(ms);
        poll.start(25);
        loop.exec();
    };

    bool listed = false;
    QObject::connect(&host, &TrayHost::listed, &loop, [&listed] { listed = true; });
    host.start(TrayHost::Observe);
    wait(3000, [&listed] { return listed; });
    wait(TrayLimits::PropertyTimeoutMs + 500, [&host] { return host.settled(); });

    QList<TrayMenu *> menus;
    for (TrayItem *item : host.items()) {
        if (TrayMenu *menu = item->ready() ? item->menu() : nullptr) {
            menu->peek();
            menus << menu;
        }
    }
    wait(TrayLimits::MenuTimeoutMs + 500, [&menus] {
        return std::all_of(menus.cbegin(), menus.cend(), [](const TrayMenu *menu) { return !menu->loading(); });
    });

    using namespace TrayLimits;
    QJsonObject out = host.toJson();
    // Seen from here the watcher is always somebody else; say what it is to the session.
    out.insert(QStringLiteral("watcher"), host.available() ? QStringLiteral("running") : QStringLiteral("none"));
    out.insert(QStringLiteral("observer"), true);
    out.insert(QStringLiteral("iconTheme"), QIcon::themeName());
    out.insert(QStringLiteral("xembed"), QStringLiteral("not supported"));
    out.insert(QStringLiteral("limits"), QJsonObject{
        {QStringLiteral("items"), MaxItems},
        {QStringLiteral("itemsPerClient"), MaxItemsPerClient},
        {QStringLiteral("titleLength"), MaxTitleLength},
        {QStringLiteral("toolTipLength"), MaxToolTipLength},
        {QStringLiteral("pixmapSide"), MaxPixmapSide},
        {QStringLiteral("keptPixmapSide"), KeptPixmapSide},
        {QStringLiteral("iconFileBytes"), MaxIconFileBytes},
        {QStringLiteral("menuDepth"), MaxMenuDepth},
        {QStringLiteral("menuEntries"), MaxMenuEntries},
        {QStringLiteral("menuEntriesPerLevel"), MaxMenuChildren},
        {QStringLiteral("menuLabelLength"), MaxLabelLength},
        {QStringLiteral("menuIconBytes"), MaxMenuIconBytes},
        {QStringLiteral("refreshIntervalMs"), RefreshIntervalMs},
    });
    return out;
}
HORIZON_SHELL_DUMP(tray, "tray", dumpTray)
