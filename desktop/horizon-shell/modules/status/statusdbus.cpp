// SPDX-License-Identifier: GPL-3.0-or-later
#include "statusdbus.h"

#include <QDBusArgument>
#include <QDBusError>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusServiceWatcher>
#include <QDBusVariant>

namespace StatusModule {

namespace {
const QString kProperties = QStringLiteral("org.freedesktop.DBus.Properties");

QString errorText(const QDBusMessage &reply)
{
    return reply.errorMessage().isEmpty() ? reply.errorName() : reply.errorMessage();
}
} // namespace

QString dbusObjectPath(const QVariant &value)
{
    QString path;
    if (value.metaType() == QMetaType::fromType<QDBusObjectPath>())
        path = value.value<QDBusObjectPath>().path();
    else if (value.metaType() == QMetaType::fromType<QString>())
        path = value.toString();
    return path == QLatin1String("/") ? QString() : path;
}

QStringList dbusObjectPaths(const QVariant &value)
{
    QStringList out;
    const auto paths = qdbus_cast<QList<QDBusObjectPath>>(value);
    for (const QDBusObjectPath &path : paths) {
        if (!path.path().isEmpty() && path.path() != QLatin1String("/"))
            out << path.path();
    }
    return out;
}

QVariantMap dbusMap(const QVariant &value)
{
    return qdbus_cast<QVariantMap>(value);
}

QList<QVariantMap> dbusMapList(const QVariant &value)
{
    return qdbus_cast<QList<QVariantMap>>(value);
}

void dbusCall(const QDBusConnection &bus, const QDBusMessage &message, QObject *context,
              const std::function<void(const QDBusMessage &)> &done, int timeoutMs)
{
    if (!bus.isConnected()) {
        // A pending call on a bus that is not there never finishes; report
        // the failure from the event loop like any other reply.
        const QDBusMessage error = QDBusMessage::createError(QDBusError::Disconnected,
                                                             QStringLiteral("Not connected to the message bus"));
        QMetaObject::invokeMethod(context, [done, error] {
            if (done)
                done(error);
        }, Qt::QueuedConnection);
        return;
    }
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message, timeoutMs), context);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, context, [done](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        if (done)
            done(w->reply());
    });
}

QDBusMessage dbusMethod(const QString &service, const QString &path, const QString &interface,
                        const QString &method, bool autoStart)
{
    QDBusMessage call = QDBusMessage::createMethodCall(service, path, interface, method);
    call.setAutoStartService(autoStart);
    return call;
}

QDBusServiceWatcher *dbusWatchService(const QDBusConnection &bus, const QString &service, QObject *context,
                                      const std::function<void()> &up, const std::function<void()> &down)
{
    auto *watcher = new QDBusServiceWatcher(service, bus, QDBusServiceWatcher::WatchForOwnerChange, context);
    QObject::connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, context,
                     [up, down](const QString &, const QString &, const QString &newOwner) {
        if (newOwner.isEmpty()) {
            if (down)
                down();
        } else if (up) {
            up();
        }
    });
    return watcher;
}

DBusProperties::DBusProperties(const QDBusConnection &bus, const QString &service, const QString &path,
                               const QString &interface, QObject *parent)
    : QObject(parent), m_bus(bus), m_service(service), m_path(path), m_interface(interface)
{
    // arg0 of PropertiesChanged is the interface name, so the bus only
    // delivers changes of the interface this object caches.
    m_bus.connect(m_service, m_path, kProperties, QStringLiteral("PropertiesChanged"),
                  QStringList{m_interface}, QStringLiteral("sa{sv}as"), this,
                  SLOT(propertiesChanged(QString,QVariantMap,QStringList)));
}

bool DBusProperties::boolean(const QString &name, bool fallback) const
{
    const QVariant v = m_values.value(name);
    return v.isValid() ? v.toBool() : fallback;
}

qlonglong DBusProperties::integer(const QString &name, qlonglong fallback) const
{
    const QVariant v = m_values.value(name);
    bool ok = false;
    const qlonglong n = v.toLongLong(&ok);
    return ok ? n : fallback;
}

double DBusProperties::real(const QString &name, double fallback) const
{
    const QVariant v = m_values.value(name);
    bool ok = false;
    const double n = v.toDouble(&ok);
    return ok ? n : fallback;
}

void DBusProperties::load()
{
    const quint64 serial = ++m_serial;
    QDBusMessage call = dbusMethod(m_service, m_path, kProperties, QStringLiteral("GetAll"), m_autoStart);
    call << m_interface;
    dbusCall(m_bus, call, this, [this, serial](const QDBusMessage &reply) {
        if (serial != m_serial)
            return;     // superseded by a newer load() or by clear()
        m_loaded = true;
        if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
            m_values = qdbus_cast<QVariantMap>(reply.arguments().constFirst());
            m_valid = true;
            m_error.clear();
        } else {
            m_values.clear();
            m_valid = false;
            m_error = errorText(reply);
        }
        Q_EMIT loaded();
        Q_EMIT changed();
    });
}

void DBusProperties::clear()
{
    ++m_serial;     // a reply that is still on its way is stale
    m_values.clear();
    m_valid = false;
    m_loaded = true;
    m_error = QStringLiteral("%1 is not running").arg(m_service);
    Q_EMIT changed();
}

void DBusProperties::set(const QString &name, const QVariant &value,
                         const std::function<void(const QString &)> &done)
{
    QDBusMessage call = dbusMethod(m_service, m_path, kProperties, QStringLiteral("Set"), m_autoStart);
    call << m_interface << name << QVariant::fromValue(QDBusVariant(value));
    // The user asked for this change: let polkit ask for authentication if
    // the system's policy wants it (the reply then takes as long as the user).
    call.setInteractiveAuthorizationAllowed(true);
    dbusCall(m_bus, call, this, [done](const QDBusMessage &reply) {
        if (done)
            done(reply.type() == QDBusMessage::ErrorMessage ? errorText(reply) : QString());
    }, 120000);
}

void DBusProperties::propertiesChanged(const QString &interface, const QVariantMap &changedValues,
                                       const QStringList &invalidated)
{
    if (interface != m_interface || !m_valid)
        return;     // before the first GetAll reply there is nothing to patch
    for (auto it = changedValues.cbegin(); it != changedValues.cend(); ++it)
        m_values.insert(it.key(), it.value());
    if (!changedValues.isEmpty())
        Q_EMIT changed();
    if (!invalidated.isEmpty())
        load();     // the new values were not sent along
}

} // namespace StatusModule
