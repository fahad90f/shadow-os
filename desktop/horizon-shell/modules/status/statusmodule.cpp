// SPDX-License-Identifier: GPL-3.0-or-later
//
// The "status" shell module: status indicators in the panel, the quick
// settings popup, the on-screen display of the volume, brightness and media
// keys, the calendar popup, and the network kill switch / panic state (panel
// indicator, quick-settings tile, panic banner).
#include "module.h"
#include "statusservice.h"

#include <QEventLoop>
#include <QTimer>

static void init(Horizon::ShellContext &ctx)
{
    // Every back-end starts with asynchronous queries; nothing here waits.
    auto *service = new StatusModule::Service(ctx.parent());
    ctx.expose(QStringLiteral("status"), service);
    ctx.addPanelItem(QStringLiteral("StatusPanelItem.qml"), 30);
    ctx.addSurface(QStringLiteral("StatusQuickSettings.qml"));
    ctx.addSurface(QStringLiteral("StatusCalendar.qml"));
    ctx.addSurface(QStringLiteral("StatusOsd.qml"));
    // The panic banner (state from status.killSwitch, actions of the core's
    // "panic" object).
    ctx.addSurface(QStringLiteral("StatusPanicBanner.qml"));
}
HORIZON_SHELL_MODULE(status, init)

// horizon-shell --dump status: every back-end's availability and values,
// once each has had its answer (or after a few seconds, with the stragglers
// reported as "still being queried").
static QJsonObject dumpStatus()
{
    StatusModule::Service service;
    if (!service.allReady()) {
        QEventLoop loop;
        QObject::connect(&service, &StatusModule::Service::backendReady, &loop, [&] {
            if (service.allReady())
                loop.quit();
        });
        QTimer::singleShot(8000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return service.toJson();
}
HORIZON_SHELL_DUMP(status, "status", dumpStatus)
