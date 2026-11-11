// SPDX-License-Identifier: GPL-3.0-or-later
#include "powerdbus.h"

#include <QDBusArgument>
#include <QDBusPendingCallWatcher>
#include <QDBusVariant>

namespace PowerModule {

void dbusCall(const QDBusConnection &bus, const QDBusMessage &message, QObject *context,
              const std::function<void(const QDBusMessage &reply)> &done, int timeoutMs)
{
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message, timeoutMs), context);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, context, [done](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        if (done)
            done(w->reply());
    });
}

QDBusMessage dbusMethod(const QString &service, const QString &path, const QString &interface, const QString &method)
{
    return QDBusMessage::createMethodCall(service, path, interface, method);
}

QDBusMessage dbusGetAll(const QString &service, const QString &path, const QString &interface)
{
    QDBusMessage m = dbusMethod(service, path, QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("GetAll"));
    m << interface;
    return m;
}

QDBusMessage dbusGet(const QString &service, const QString &path, const QString &interface, const QString &property)
{
    QDBusMessage m = dbusMethod(service, path, QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
    m << interface << property;
    return m;
}

QVariant dbusUnwrap(const QVariant &value)
{
    if (value.metaType() == QMetaType::fromType<QDBusVariant>())
        return value.value<QDBusVariant>().variant();
    return value;
}

QVariantMap dbusMap(const QVariant &value)
{
    const QVariant inner = dbusUnwrap(value);
    if (inner.metaType() == QMetaType::fromType<QDBusArgument>()) {
        QVariantMap map;
        inner.value<QDBusArgument>() >> map;
        return map;
    }
    return inner.toMap();
}

} // namespace PowerModule
