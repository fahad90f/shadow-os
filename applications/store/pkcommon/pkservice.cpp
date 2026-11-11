// SPDX-License-Identifier: GPL-3.0-or-later
#include "pkservice.h"

#include "pkutil.h"

#include <Daemon>

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QJsonArray>
#include <QLocale>
#include <QTimer>

namespace ShadowPk {

namespace {
const QString kService = QStringLiteral("org.freedesktop.PackageKit");
const QString kPath = QStringLiteral("/org/freedesktop/PackageKit");

QString networkKey(uint state)
{
    switch (static_cast<PackageKit::Daemon::Network>(state)) {
    case PackageKit::Daemon::NetworkOffline: return QStringLiteral("offline");
    case PackageKit::Daemon::NetworkOnline: return QStringLiteral("online");
    case PackageKit::Daemon::NetworkWired: return QStringLiteral("wired");
    case PackageKit::Daemon::NetworkWifi: return QStringLiteral("wifi");
    case PackageKit::Daemon::NetworkMobile: return QStringLiteral("mobile");
    case PackageKit::Daemon::NetworkUnknown: break;
    }
    return QStringLiteral("unknown");
}
} // namespace

Service *Service::instance()
{
    static Service *self = new Service(QCoreApplication::instance());
    return self;
}

Service::Service(QObject *parent)
    : QObject(parent)
{
}

bool Service::supports(PackageKit::Transaction::Role role) const
{
    return m_available && (m_roleBits & (1ULL << static_cast<int>(role)));
}

void Service::probe()
{
    const quint64 serial = ++m_probeSerial;
    if (!m_checking) {
        m_checking = true;
        Q_EMIT changed();
    }

    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        const QDBusError e = bus.lastError();
        const QString detail = e.isValid() ? e.message() : tr("no connection to the system message bus");
        // Report on the next event-loop turn so that probe() is always asynchronous.
        QTimer::singleShot(0, this, [this, serial, e, detail] {
            if (serial != m_probeSerial)
                return;
            setUnavailable(e.name(), tr("The system D-Bus is not available: %1").arg(detail));
            Q_EMIT probed();
        });
        return;
    }

    if (!m_watching) {
        // If the service appears later (package installed, daemon restarted) pick it up.
        m_watching = true;
        // QtDBus has no "disconnected" signal; libdbus announces the loss of
        // the connection with this local signal.
        bus.connect(QString(), QStringLiteral("/org/freedesktop/DBus/Local"), QStringLiteral("org.freedesktop.DBus.Local"),
                    QStringLiteral("Disconnected"), this, SLOT(busDisconnected()));
        auto *watcher = new QDBusServiceWatcher(kService, bus, QDBusServiceWatcher::WatchForRegistration, this);
        connect(watcher, &QDBusServiceWatcher::serviceRegistered, this, [this] {
            if (!m_available && !m_checking)
                probe();
        });
    }

    // Properties.GetAll both answers "is it there?" (the bus activates the
    // daemon if needed) and returns everything the UI shows about it.
    QDBusMessage call = QDBusMessage::createMethodCall(kService, kPath,
                                                       QStringLiteral("org.freedesktop.DBus.Properties"),
                                                       QStringLiteral("GetAll"));
    call << kService;
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(call, 30000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, serial](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        if (serial != m_probeSerial)
            return;
        const QDBusPendingReply<QVariantMap> reply = *w;
        if (reply.isError()) {
            const QDBusError e = reply.error();
            setUnavailable(e.name(), e.message());
        } else {
            applyProperties(reply.value());
            hookDaemon();
        }
        Q_EMIT probed();
    });
}

void Service::busDisconnected()
{
    // The process-wide system bus connection cannot be re-established (and
    // PackageKit-Qt only uses that one), so this state lasts until restart.
    ++m_probeSerial;
    setUnavailable(QStringLiteral("org.freedesktop.DBus.Local.Disconnected"),
                   tr("The connection to the system D-Bus was lost. Restart this application once the system bus is running again."));
    Q_EMIT probed();
}

void Service::setUnavailable(const QString &errorName, const QString &message)
{
    m_available = false;
    m_checking = false;
    m_errorName = errorName;
    m_error = errorName.isEmpty() ? message : QStringLiteral("%1 (%2)").arg(message, errorName);
    Q_EMIT changed();
}

void Service::applyProperties(const QVariantMap &p)
{
    m_available = true;
    m_checking = false;
    m_error.clear();
    m_errorName.clear();
    m_backendName = p.value(QStringLiteral("BackendName")).toString();
    m_backendDescription = p.value(QStringLiteral("BackendDescription")).toString();
    m_backendAuthor = p.value(QStringLiteral("BackendAuthor")).toString();
    m_daemonVersion = QStringLiteral("%1.%2.%3")
                          .arg(p.value(QStringLiteral("VersionMajor")).toUInt())
                          .arg(p.value(QStringLiteral("VersionMinor")).toUInt())
                          .arg(p.value(QStringLiteral("VersionMicro")).toUInt());
    m_distroId = p.value(QStringLiteral("DistroId")).toString();
    m_networkState = networkKey(p.value(QStringLiteral("NetworkState")).toUInt());
    m_locked = p.value(QStringLiteral("Locked")).toBool();
    m_roleBits = p.value(QStringLiteral("Roles")).toULongLong();
    m_roles = roleKeys(m_roleBits);
    Q_EMIT changed();
}

void Service::hookDaemon()
{
    if (m_daemonHooked)
        return;
    m_daemonHooked = true;

    // Hints apply to every transaction created from now on: localised
    // summaries, and whether polkit may ask the user (GUI session) or the
    // transaction must fail rather than prompt (headless update check).
    QStringList hints = PackageKit::Daemon::hints();
    if (hints.isEmpty()) {
        const QString locale = QLocale::system().name();
        if (!locale.isEmpty() && locale != QLatin1String("C"))
            hints << QStringLiteral("locale=%1.UTF-8").arg(locale);
        hints << (m_interactive ? QStringLiteral("interactive=true") : QStringLiteral("interactive=false"))
              << (m_interactive ? QStringLiteral("background=false") : QStringLiteral("background=true"));
        PackageKit::Daemon::setHints(hints);
    }

    PackageKit::Daemon *daemon = PackageKit::Daemon::global();
    connect(daemon, &PackageKit::Daemon::updatesChanged, this, &Service::updatesChanged);
    connect(daemon, &PackageKit::Daemon::repoListChanged, this, &Service::repoListChanged);
    connect(daemon, &PackageKit::Daemon::restartScheduled, this, &Service::restartScheduled);
    connect(daemon, &PackageKit::Daemon::daemonQuit, this, &Service::daemonQuit);
    connect(daemon, &PackageKit::Daemon::networkStateChanged, this, [this] {
        const QString state = networkKey(static_cast<uint>(PackageKit::Daemon::networkState()));
        if (state != m_networkState) {
            m_networkState = state;
            Q_EMIT changed();
        }
    });
    connect(daemon, &PackageKit::Daemon::changed, this, [this] {
        // The daemon's own (asynchronously fetched) copy of the properties changed.
        if (!PackageKit::Daemon::isRunning())
            return;
        const bool locked = PackageKit::Daemon::locked();
        if (locked != m_locked) {
            m_locked = locked;
            Q_EMIT changed();
        }
    });
}

QJsonObject Service::toJson() const
{
    QJsonObject o{
        {QStringLiteral("reachable"), m_available},
        {QStringLiteral("service"), kService},
        {QStringLiteral("bus"), QStringLiteral("system")},
    };
    if (!m_available) {
        o.insert(QStringLiteral("error"), m_error);
        o.insert(QStringLiteral("errorName"), m_errorName);
        return o;
    }
    o.insert(QStringLiteral("backendName"), m_backendName);
    o.insert(QStringLiteral("backendDescription"), m_backendDescription);
    o.insert(QStringLiteral("backendAuthor"), m_backendAuthor);
    o.insert(QStringLiteral("version"), m_daemonVersion);
    o.insert(QStringLiteral("distroId"), m_distroId);
    o.insert(QStringLiteral("networkState"), m_networkState);
    o.insert(QStringLiteral("locked"), m_locked);
    o.insert(QStringLiteral("roles"), QJsonArray::fromStringList(m_roles));
    return o;
}

} // namespace ShadowPk
