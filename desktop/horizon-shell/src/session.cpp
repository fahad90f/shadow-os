// SPDX-License-Identifier: GPL-3.0-or-later
#include "session.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QSysInfo>

#include <pwd.h>
#include <unistd.h>

namespace {
const QString kLogin1 = QStringLiteral("org.freedesktop.login1");
const QString kLogin1Path = QStringLiteral("/org/freedesktop/login1");
const QString kLogin1Manager = QStringLiteral("org.freedesktop.login1.Manager");
const QString kAutostartFile = QStringLiteral("/run/shadow/autostart");
} // namespace

Session::Session(QObject *parent)
    : QObject(parent)
{
    if (const passwd *pw = getpwuid(getuid())) {
        m_userName = QString::fromLocal8Bit(pw->pw_name);
        m_fullName = QString::fromLocal8Bit(pw->pw_gecos).section(QLatin1Char(','), 0, 0);
    }
    if (m_fullName.isEmpty())
        m_fullName = m_userName;
    m_hostName = QSysInfo::machineHostName();
    m_live = QFile::exists(QStringLiteral("/run/shadow/live-session"));
    queryCapabilities();
}

void Session::queryCapabilities()
{
    struct Probe { const char *method; bool Session::*flag; };
    static const Probe probes[] = {
        {"CanSuspend", &Session::m_canSuspend}, {"CanHibernate", &Session::m_canHibernate},
        {"CanReboot", &Session::m_canReboot}, {"CanPowerOff", &Session::m_canPowerOff},
    };
    for (const Probe &p : probes) {
        const QDBusMessage call = QDBusMessage::createMethodCall(kLogin1, kLogin1Path, kLogin1Manager,
                                                                 QString::fromLatin1(p.method));
        auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(call), this);
        const auto flag = p.flag;
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, flag](QDBusPendingCallWatcher *w) {
            w->deleteLater();
            const QDBusPendingReply<QString> reply = *w;
            // "yes" and "challenge" (authentication required) are both offered.
            const bool can = !reply.isError() && (reply.value() == QLatin1String("yes") || reply.value() == QLatin1String("challenge"));
            if (this->*flag != can) { this->*flag = can; Q_EMIT capabilitiesChanged(); }
        });
    }
    QDBusConnectionInterface *iface = QDBusConnection::sessionBus().interface();
    const bool lock = iface && iface->isServiceRegistered(QStringLiteral("org.freedesktop.ScreenSaver"));
    if (lock != m_canLock) { m_canLock = lock; Q_EMIT capabilitiesChanged(); }
}

QString Session::takeAutostartRequest()
{
    QFile f(kAutostartFile);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QString what = QString::fromUtf8(f.readAll()).trimmed();
    f.close();
    // Only honour it once per boot; the file lives on tmpfs and is root-owned,
    // so record that it was consumed in the user's runtime directory.
    const QString marker = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation)
                           + QStringLiteral("/shadow-autostart-done");
    if (QFile::exists(marker))
        return {};
    QFile m(marker);
    if (m.open(QIODevice::WriteOnly))
        m.close();
    return (what == QLatin1String("installer") || what == QLatin1String("recovery")) ? what : QString();
}

void Session::lock()
{
    const QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.ScreenSaver"),
        QStringLiteral("/ScreenSaver"), QStringLiteral("org.freedesktop.ScreenSaver"), QStringLiteral("Lock"));
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        if (w->isError())
            Q_EMIT actionFailed(QStringLiteral("lock"), tr("The screen locker is not available: %1").arg(w->error().message()));
    });
}

void Session::logout()
{
    // The session supervisor (horizon-session-leader) ends the session when
    // the shell exits with status 0; any other exit status is a crash and
    // gets the shell restarted.
    QCoreApplication::exit(0);
}

void Session::logind(const QString &method, const QString &what)
{
    QDBusMessage call = QDBusMessage::createMethodCall(kLogin1, kLogin1Path, kLogin1Manager, method);
    call << true;       // interactive: let polkit ask if authorisation is needed
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::systemBus().asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, what](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        if (w->isError())
            Q_EMIT actionFailed(what, w->error().message());
    });
}

void Session::suspend() { logind(QStringLiteral("Suspend"), QStringLiteral("suspend")); }
void Session::hibernate() { logind(QStringLiteral("Hibernate"), QStringLiteral("hibernate")); }
void Session::reboot() { logind(QStringLiteral("Reboot"), QStringLiteral("reboot")); }
void Session::powerOff() { logind(QStringLiteral("PowerOff"), QStringLiteral("power-off")); }

bool Session::programExists(const QString &program) const
{
    return !QStandardPaths::findExecutable(program).isEmpty();
}

bool Session::run(const QString &program, const QStringList &arguments)
{
    if (!programExists(program)) {
        Q_EMIT actionFailed(program, tr("%1 is not installed.").arg(program));
        return false;
    }
    if (!QProcess::startDetached(program, arguments)) {
        Q_EMIT actionFailed(program, tr("%1 could not be started.").arg(program));
        return false;
    }
    return true;
}
