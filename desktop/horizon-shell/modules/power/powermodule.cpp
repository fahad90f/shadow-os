// SPDX-License-Identifier: GPL-3.0-or-later
//
// The "power" shell module: idle actions (screens off, lock, suspend) from
// ~/.config/shadow/power.conf, low-battery notices and update notices.
#include "module.h"
#include "powerservice.h"

#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QTimer>

static void init(Horizon::ShellContext &ctx)
{
    // Everything starts with asynchronous queries; nothing here waits.
    auto *service = new PowerModule::Service(ctx.session, ctx.parent());
    ctx.expose(QStringLiteral("power"), service);
}
HORIZON_SHELL_MODULE(power, init)

// horizon-shell --dump power (no compositor needed): the settings, the power
// source and the inhibitors as this process reads them, and — under
// "shell" — the state of the running shell's module (timers, application
// inhibitors, update checks), which that shell keeps in a file in
// $XDG_RUNTIME_DIR.
static QJsonObject dumpPower()
{
    PowerModule::Service service(nullptr);
    if (!service.ready()) {
        QEventLoop loop;
        QObject::connect(&service, &PowerModule::Service::readyChanged, &loop, [&] {
            if (service.ready())
                loop.quit();
        });
        QTimer::singleShot(4000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    QJsonObject dump = service.toJson();

    QJsonValue shell = QJsonValue::Null;
    QString shellState = QStringLiteral("not-running");
    QFile file(PowerModule::Service::stateFile());
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonObject state = QJsonDocument::fromJson(file.readAll()).object();
        const qint64 pid = state.value(QLatin1String("pid")).toInteger();
        if (pid > 0 && QFile::exists(QStringLiteral("/proc/%1").arg(pid))) {
            shell = state;
            shellState = QStringLiteral("running");
        } else {
            shellState = QStringLiteral("stale-state-file");
        }
    }
    dump.insert(QStringLiteral("shellState"), shellState);
    dump.insert(QStringLiteral("shell"), shell);
    return dump;
}
HORIZON_SHELL_DUMP(power, "power", dumpPower)
