// SPDX-License-Identifier: GPL-3.0-or-later
//
// Small QtDBus helpers of the power module. Everything is asynchronous: no
// call here blocks the shell's GUI thread.
#pragma once

#include <QDBusConnection>
#include <QDBusMessage>
#include <QObject>
#include <QVariantMap>

#include <functional>

namespace PowerModule {

// Sends 'message'; 'done' gets the reply (ReplyMessage or ErrorMessage) unless
// 'context' has been destroyed by then.
void dbusCall(const QDBusConnection &bus, const QDBusMessage &message, QObject *context,
              const std::function<void(const QDBusMessage &reply)> &done, int timeoutMs = 10000);

QDBusMessage dbusMethod(const QString &service, const QString &path, const QString &interface, const QString &method);
QDBusMessage dbusGetAll(const QString &service, const QString &path, const QString &interface);
QDBusMessage dbusGet(const QString &service, const QString &path, const QString &interface, const QString &property);

// a{sv} that arrived as a QDBusArgument or as a QVariantMap.
QVariantMap dbusMap(const QVariant &value);
// The value inside a "v".
QVariant dbusUnwrap(const QVariant &value);

} // namespace PowerModule
